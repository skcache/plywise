#include "pct/service/hosted_runtime.hpp"

#include "pct/common/error.hpp"

#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
#include "pct/app/hosted_browser_observations.hpp"
#include "pct/app/hosted_identity.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/service/oidc_token_verifier.hpp"

#include <curl/curl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace pct::service {
namespace {

#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)

constexpr std::size_t max_jwks_bytes = 512U * 1024U;
constexpr std::int64_t guest_lifetime_ms = 24LL * 60 * 60 * 1000;

template <std::size_t Size>
std::string hex_encode(const std::array<unsigned char, Size>& bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(Size * 2);
    for (const unsigned char byte : bytes) {
        encoded.push_back(alphabet[byte >> 4U]);
        encoded.push_back(alphabet[byte & 0x0fU]);
    }
    return encoded;
}

std::array<unsigned char, 32> sha256_token(std::string_view token) {
    std::array<unsigned char, 32> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest.data());
    return digest;
}

bool valid_guest_token(std::string_view token) {
    if (token.size() != 64)
        return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

bool valid_jwks_url(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > 2048 ||
        std::any_of(url.begin(), url.end(), [](unsigned char character) {
            return character < 0x20U || character == 0x7fU || character == '\\' ||
                   character == '@';
        }))
        return false;
    const std::string_view authority = url.substr(std::string_view("https://").size());
    const std::size_t path = authority.find_first_of("/?#");
    const std::string_view host = authority.substr(0, path);
    return !host.empty() && host.find(':') == std::string_view::npos &&
           host.find('.') != std::string_view::npos;
}

struct JwksResponse {
    std::string body;
    bool overflowed{false};
};

std::size_t append_jwks(char* data, std::size_t size, std::size_t count, void* context) {
    auto& response = *static_cast<JwksResponse*>(context);
    if (response.body.size() > max_jwks_bytes ||
        (size != 0 && count > (max_jwks_bytes - response.body.size()) / size)) {
        response.overflowed = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    response.body.append(data, bytes);
    return bytes;
}

bool curl_ready() {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return initialized;
}

std::optional<std::string> fetch_jwks(std::string url) {
    if (!valid_jwks_url(url) || !curl_ready())
        return std::nullopt;
    CURL* raw = curl_easy_init();
    if (raw == nullptr)
        return std::nullopt;
    JwksResponse response;
    curl_easy_setopt(raw, CURLOPT_URL, url.c_str());
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(raw, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(raw, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(raw, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(raw, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(raw, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, append_jwks);
    curl_easy_setopt(raw, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(raw);
    long status = 0;
    if (result == CURLE_OK)
        curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(raw);
    if (result != CURLE_OK || response.overflowed || status != 200 || response.body.empty())
        return std::nullopt;
    return response.body;
}

#endif

} // namespace

struct HostedRuntime::Impl {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    struct OwnerResources {
        std::unique_ptr<app::PostgresRepository> repository;
        std::unique_ptr<app::JobManager> jobs;
        bool retired{false};
        std::atomic<std::size_t> active_leases{0};
        std::chrono::steady_clock::time_point last_used{};
    };

    // ApiScope contains raw repository/job pointers for the hot path. This lease is the lifetime
    // token that keeps those pointers valid through an HTTP request or owner WebSocket observer.
    struct ScopeLease {
        explicit ScopeLease(std::shared_ptr<OwnerResources> input)
            : resources(std::move(input)) {
            resources->active_leases.fetch_add(1, std::memory_order_relaxed);
        }

        ~ScopeLease() {
            resources->active_leases.fetch_sub(1, std::memory_order_relaxed);
        }

        std::shared_ptr<OwnerResources> resources;
    };

    Impl(HostedRuntimeOptions input, analysis::Analyzer& input_analyzer,
         app::JobManagerOptions input_job_options)
        : options(std::move(input)), analyzer(input_analyzer),
          job_options(input_job_options) {
        if (options.postgres_connection.empty() || options.oidc_issuer.empty() ||
            options.oidc_audience.empty() || options.oidc_provider.empty() ||
            !valid_jwks_url(options.oidc_jwks_url) || options.max_owner_resources == 0 ||
            options.owner_resource_idle_ttl <= std::chrono::seconds::zero())
            throw Error(ErrorCode::InvalidArgument, "hosted runtime configuration is invalid");
        identity = std::make_unique<app::HostedIdentityStore>(options.postgres_connection);
        browser_observations =
            std::make_unique<app::HostedBrowserObservationStore>(options.postgres_connection);
        verifier = std::make_unique<OidcTokenVerifier>(OidcTokenVerifierOptions{
            options.oidc_issuer,
            options.oidc_audience,
            options.oidc_provider,
            60,
            [url = options.oidc_jwks_url] { return fetch_jwks(url); },
            [this](std::string_view provider,
                   std::string_view subject) { return resolve_account(provider, subject); },
            [this](std::string_view provider, std::string_view subject,
                   std::int64_t issued_at_ms) {
                return resolve_account(provider, subject, issued_at_ms);
            },
        });
    }

    std::optional<app::OwnerId> resolve_account(std::string_view provider,
                                                 std::string_view subject,
                                                 std::optional<std::int64_t> issued_at_ms =
                                                     std::nullopt) {
        if (provider != options.oidc_provider)
            return std::nullopt;
        try {
            return identity
                ->ensure_account(std::string(provider), std::string(subject), issued_at_ms)
                .owner();
        } catch (...) {
            // Identity-store failures are deliberately indistinguishable from an invalid token
            // at this boundary; no provider or database details reach the HTTP response.
            return std::nullopt;
        }
    }

    std::optional<app::OwnerId> guest_owner(std::string_view token) const {
        if (!valid_guest_token(token))
            return std::nullopt;
        try {
            return identity->owner_for_guest_token(sha256_token(token));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<GuestSessionCredential> create_guest_session() const {
        std::array<unsigned char, 32> token_bytes{};
        std::array<unsigned char, 16> guest_bytes{};
        if (RAND_bytes(token_bytes.data(), static_cast<int>(token_bytes.size())) != 1 ||
            RAND_bytes(guest_bytes.data(), static_cast<int>(guest_bytes.size())) != 1)
            return std::nullopt;
        const std::string token = hex_encode(token_bytes);
        const std::string guest_id = "guest-" + hex_encode(guest_bytes);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        try {
            const auto session = identity->create_guest_session(
                guest_id, sha256_token(token), now + guest_lifetime_ms);
            return GuestSessionCredential{session.id, token, session.expires_at_ms};
        } catch (...) {
            return std::nullopt;
        }
    }

    void reserve_guest_analysis(const app::OwnerId& guest, std::string_view game_id) const {
        if (guest.kind() != app::OwnerKind::Guest)
            throw Error(ErrorCode::InvalidArgument, "guest analysis reservation is invalid");
        try {
            identity->reserve_guest_analysis(std::string(guest.value()), std::string(game_id));
        } catch (const Error& error) {
            if (error.code() == ErrorCode::InvalidArgument ||
                error.code() == ErrorCode::NotFound ||
                error.code() == ErrorCode::QuotaExceeded)
                throw;
            throw Error(ErrorCode::IoError, "guest analysis quota is unavailable");
        } catch (...) {
            throw Error(ErrorCode::IoError, "guest analysis quota is unavailable");
        }
    }

    void begin_browser_observation(
        const app::OwnerId& owner, const analysis::BrowserObservationRunContext& context) const {
        try {
            browser_observations->begin(owner, context);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::InvalidArgument ||
                error.code() == ErrorCode::NotFound)
                throw;
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        } catch (...) {
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        }
    }

    analysis::BrowserObservationReceipt submit_browser_observation(
        const app::OwnerId& owner, const analysis::BrowserObservationContext& context,
        const analysis::BrowserEngineObservation& observation) const {
        try {
            return browser_observations->submit(owner, context, observation);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::InvalidArgument ||
                error.code() == ErrorCode::NotFound)
                throw;
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        } catch (...) {
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        }
    }

    analysis::BrowserObservationBundle finalize_browser_observation(
        const app::OwnerId& owner, std::string_view game_id,
        std::string_view analysis_run_id) const {
        try {
            return browser_observations->finalize(owner, game_id, analysis_run_id);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::InvalidArgument ||
                error.code() == ErrorCode::NotFound || error.code() == ErrorCode::Corruption)
                throw;
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        } catch (...) {
            throw Error(ErrorCode::IoError, "browser analysis staging is unavailable");
        }
    }

    GuestClaimResult claim_guest(std::string_view token, const app::OwnerId& account,
                                 std::string_view idempotency_key) const {
        if (account.kind() != app::OwnerKind::Account || !valid_guest_token(token))
            throw Error(ErrorCode::InvalidArgument, "guest claim is invalid");
        const auto guest = guest_owner(token);
        if (!guest)
            throw Error(ErrorCode::NotFound, "guest session does not exist");
        try {
            const auto receipt = identity->claim_guest(std::string(guest->value()),
                                                       std::string(account.value()),
                                                       std::string(idempotency_key));
            return GuestClaimResult{receipt.guest_id, receipt.account_id,
                                    receipt.transferred_games, receipt.already_claimed};
        } catch (const Error& error) {
            if (error.code() == ErrorCode::InvalidArgument ||
                error.code() == ErrorCode::NotFound)
                throw;
            throw Error(ErrorCode::IoError, "guest claim is unavailable");
        } catch (...) {
            throw Error(ErrorCode::IoError, "guest claim is unavailable");
        }
    }

    std::optional<ApiScope> scope_for_owner(const app::OwnerId& owner) {
        if (owner.kind() != app::OwnerKind::Account && owner.kind() != app::OwnerKind::Guest)
            return std::nullopt;
        // Keep evicted resources alive until after resources_mutex is released. JobManager's
        // destructor waits for workers and must never run while the scope registry is locked.
        std::vector<std::shared_ptr<OwnerResources>> released;
        std::lock_guard lock(resources_mutex);
        const auto now = std::chrono::steady_clock::now();
        const std::string key =
            (owner.kind() == app::OwnerKind::Account ? "account:" : "guest:") +
            std::string(owner.value());
        if (const auto existing = resources.find(key); existing != resources.end()) {
            if (existing->second->retired)
                return std::nullopt;
            auto& owner_resources = existing->second;
            owner_resources->last_used = now;
            auto lease = std::make_shared<ScopeLease>(owner_resources);
            return ApiScope{owner_resources->repository.get(), owner_resources->jobs.get(),
                            std::move(lease)};
        }

        // Reclaim the oldest resource only when it has no active request/observer and no work
        // still running. Completed jobs reload from PostgreSQL on demand, so this does not lose
        // durable state and keeps a burst of new accounts from exhausting DB connections.
        const auto evict_idle = [&](bool ignore_ttl) {
            auto candidate = resources.end();
            for (auto iterator = resources.begin(); iterator != resources.end(); ++iterator) {
                const auto& value = iterator->second;
                if (value->retired || value->active_leases.load(std::memory_order_relaxed) != 0)
                    continue;
                bool work_pending = false;
                for (const auto& job : value->jobs->list()) {
                    if (job.status == app::JobStatus::Queued ||
                        job.status == app::JobStatus::Running) {
                        work_pending = true;
                        break;
                    }
                }
                if (work_pending ||
                    (!ignore_ttl && now - value->last_used < options.owner_resource_idle_ttl))
                    continue;
                if (candidate == resources.end() ||
                    value->last_used < candidate->second->last_used)
                    candidate = iterator;
            }
            if (candidate == resources.end())
                return false;
            candidate->second->retired = true;
            released.push_back(candidate->second);
            resources.erase(candidate);
            return true;
        };

        if (resources.size() >= options.max_owner_resources) {
            if (!evict_idle(false) && !evict_idle(true))
                throw Error(ErrorCode::QuotaExceeded, "hosted owner resource capacity is full");
        }

        auto created = std::make_shared<OwnerResources>();
        created->last_used = now;
        created->repository =
            std::make_unique<app::PostgresRepository>(options.postgres_connection, owner);
        created->jobs = std::make_unique<app::JobManager>(*created->repository, analyzer,
                                                          job_options);
        const auto [inserted, _] = resources.emplace(key, std::move(created));
        auto lease = std::make_shared<ScopeLease>(inserted->second);
        return ApiScope{inserted->second->repository.get(), inserted->second->jobs.get(),
                        std::move(lease)};
    }

    AccountExportResult export_account(const app::OwnerId& owner,
                                       std::string_view idempotency_key) const {
        if (owner.kind() != app::OwnerKind::Account)
            throw Error(ErrorCode::InvalidArgument, "account export requires an account owner");
        const auto result = identity->export_account(std::string(owner.value()),
                                                      std::string(idempotency_key));
        return AccountExportResult{result.request_id, std::move(result.data),
                                   result.completed_at_ms};
    }

    AccountDeletionResult delete_account(const app::OwnerId& owner,
                                         std::string_view idempotency_key) {
        if (owner.kind() != app::OwnerKind::Account)
            throw Error(ErrorCode::InvalidArgument, "account deletion requires an account owner");

        const std::string key = "account:" + std::string(owner.value());
        std::shared_ptr<OwnerResources> retired;
        {
            std::lock_guard lock(resources_mutex);
            const auto found = resources.find(key);
            if (found != resources.end()) {
                retired = found->second;
                retired->retired = true;
                for (const auto& job : retired->jobs->list()) {
                    if (job.status == app::JobStatus::Queued ||
                        job.status == app::JobStatus::Running)
                        static_cast<void>(retired->jobs->cancel(job.id));
                }
                resources.erase(found);
            }
        }
        // The shared scope lifetime keeps in-flight API requests and websocket observers safe
        // while cancellation drains. The database cascade can proceed without destroying the
        // owner JobManager out from underneath a request thread.
        const auto result = identity->delete_account(std::string(owner.value()),
                                                      std::string(idempotency_key));
        return AccountDeletionResult{result.request_id, result.receipt_token,
                                     result.completed_at_ms, result.backup_retention_until_ms};
    }

    HostedRuntimeOptions options;
    analysis::Analyzer& analyzer;
    app::JobManagerOptions job_options;
    std::unique_ptr<app::HostedIdentityStore> identity;
    std::unique_ptr<app::HostedBrowserObservationStore> browser_observations;
    std::unique_ptr<OidcTokenVerifier> verifier;
    std::mutex resources_mutex;
    std::map<std::string, std::shared_ptr<OwnerResources>> resources;
#else
    explicit Impl(HostedRuntimeOptions, analysis::Analyzer&, app::JobManagerOptions) {
        throw Error(ErrorCode::Unsupported,
                    "hosted runtime requires PostgreSQL and OpenSSL support");
    }
#endif
};

HostedRuntime::HostedRuntime(HostedRuntimeOptions options, analysis::Analyzer& analyzer,
                             app::JobManagerOptions job_options)
    : impl_(std::make_unique<Impl>(std::move(options), analyzer, job_options)) {}

HostedRuntime::~HostedRuntime() = default;

AuthConfig::TokenVerifier HostedRuntime::token_verifier() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    const OidcTokenVerifier* verifier = impl_->verifier.get();
    Impl* runtime = impl_.get();
    return [verifier, runtime](std::string_view token) {
        if (const auto guest = runtime->guest_owner(token))
            return guest;
        return verifier->verify(token);
    };
#else
    return {};
#endif
}

AuthConfig::ScopeResolver HostedRuntime::scope_resolver() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner) { return runtime->scope_for_owner(owner); };
#else
    return {};
#endif
}

AuthConfig::GuestSessionCreator HostedRuntime::guest_session_creator() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime] { return runtime->create_guest_session(); };
#else
    return {};
#endif
}

AuthConfig::GuestAnalysisReservation HostedRuntime::guest_analysis_reservation() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& guest, std::string_view game_id) {
        runtime->reserve_guest_analysis(guest, game_id);
    };
#else
    return {};
#endif
}

AuthConfig::BrowserObservationBegin HostedRuntime::browser_observation_begin() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner,
                     const analysis::BrowserObservationRunContext& context) {
        runtime->begin_browser_observation(owner, context);
    };
#else
    return {};
#endif
}

AuthConfig::BrowserObservationSubmit HostedRuntime::browser_observation_submit() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner, const analysis::BrowserObservationContext& context,
                     const analysis::BrowserEngineObservation& observation) {
        return runtime->submit_browser_observation(owner, context, observation);
    };
#else
    return {};
#endif
}

AuthConfig::BrowserObservationFinalize HostedRuntime::browser_observation_finalize() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner, std::string_view game_id,
                     std::string_view analysis_run_id) {
        return runtime->finalize_browser_observation(owner, game_id, analysis_run_id);
    };
#else
    return {};
#endif
}

AuthConfig::GuestClaimHandler HostedRuntime::guest_claim_handler() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](std::string_view token, const app::OwnerId& account,
                     std::string_view idempotency_key) {
        return runtime->claim_guest(token, account, idempotency_key);
    };
#else
    return {};
#endif
}

AuthConfig::FreshTokenVerifier HostedRuntime::fresh_token_verifier() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    const OidcTokenVerifier* verifier = impl_->verifier.get();
    return [verifier](std::string_view token) {
        return verifier->verify_fresh(token, std::chrono::minutes(5));
    };
#else
    return {};
#endif
}

AuthConfig::AccountExportHandler HostedRuntime::account_export_handler() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner, std::string_view idempotency_key) {
        return runtime->export_account(owner, idempotency_key);
    };
#else
    return {};
#endif
}

AuthConfig::AccountDeletionHandler HostedRuntime::account_deletion_handler() const {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
    Impl* runtime = impl_.get();
    return [runtime](const app::OwnerId& owner, std::string_view idempotency_key) {
        return runtime->delete_account(owner, idempotency_key);
    };
#else
    return {};
#endif
}

} // namespace pct::service
