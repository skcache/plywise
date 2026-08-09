#include "pct/service/hosted_runtime.hpp"

#include "pct/common/error.hpp"

#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
#include "pct/app/hosted_identity.hpp"
#include "pct/app/postgres_repository.hpp"
#include "pct/service/oidc_token_verifier.hpp"

#include <curl/curl.h>
#endif

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace pct::service {
namespace {

#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)

constexpr std::size_t max_jwks_bytes = 512U * 1024U;

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
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(raw, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
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
    };

    Impl(HostedRuntimeOptions input, analysis::Analyzer& input_analyzer,
         app::JobManagerOptions input_job_options)
        : options(std::move(input)), analyzer(input_analyzer),
          job_options(input_job_options) {
        if (options.postgres_connection.empty() || options.oidc_issuer.empty() ||
            options.oidc_audience.empty() || options.oidc_provider.empty() ||
            !valid_jwks_url(options.oidc_jwks_url))
            throw Error(ErrorCode::InvalidArgument, "hosted runtime configuration is invalid");
        identity = std::make_unique<app::HostedIdentityStore>(options.postgres_connection);
        verifier = std::make_unique<OidcTokenVerifier>(OidcTokenVerifierOptions{
            options.oidc_issuer,
            options.oidc_audience,
            options.oidc_provider,
            60,
            [url = options.oidc_jwks_url] { return fetch_jwks(url); },
            [this](std::string_view provider,
                   std::string_view subject) { return resolve_account(provider, subject); },
        });
    }

    std::optional<app::OwnerId> resolve_account(std::string_view provider,
                                                 std::string_view subject) {
        if (provider != options.oidc_provider)
            return std::nullopt;
        try {
            return identity->ensure_account(std::string(provider), std::string(subject)).owner();
        } catch (...) {
            // Identity-store failures are deliberately indistinguishable from an invalid token
            // at this boundary; no provider or database details reach the HTTP response.
            return std::nullopt;
        }
    }

    std::optional<ApiScope> scope_for_owner(const app::OwnerId& owner) {
        if (owner.kind() != app::OwnerKind::Account)
            return std::nullopt;
        std::lock_guard lock(resources_mutex);
        const std::string key(owner.value());
        if (const auto existing = resources.find(key); existing != resources.end())
            return ApiScope{existing->second.repository.get(), existing->second.jobs.get()};

        OwnerResources created{
            std::make_unique<app::PostgresRepository>(options.postgres_connection, owner),
            nullptr,
        };
        created.jobs = std::make_unique<app::JobManager>(*created.repository, analyzer, job_options);
        const auto [inserted, _] = resources.emplace(key, std::move(created));
        return ApiScope{inserted->second.repository.get(), inserted->second.jobs.get()};
    }

    HostedRuntimeOptions options;
    analysis::Analyzer& analyzer;
    app::JobManagerOptions job_options;
    std::unique_ptr<app::HostedIdentityStore> identity;
    std::unique_ptr<OidcTokenVerifier> verifier;
    std::mutex resources_mutex;
    std::map<std::string, OwnerResources> resources;
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
    return [verifier](std::string_view token) { return verifier->verify(token); };
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

} // namespace pct::service
