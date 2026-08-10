#include "pct/analysis/analyzer.hpp"
#include "pct/app/ingest_manager.hpp"
#include "pct/app/job_manager.hpp"
#include "pct/app/repository.hpp"
#include "pct/common/log.hpp"
#include "pct/engine/stockfish.hpp"
#include "pct/engine/pool.hpp"
#include "pct/import/import_service.hpp"
#include "pct/service/http_server.hpp"
#include "pct/service/hosted_runtime.hpp"
#include "pct/storage/event_log.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    std::filesystem::path data_dir{"data"};
    std::filesystem::path web_root{"web/dist"};
    std::string stockfish{"stockfish"};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8787};
    std::size_t workers{std::min<std::size_t>(2, std::max(1U, std::thread::hardware_concurrency()))};
    std::size_t max_pending{256};
    std::size_t retry_limit{1};
    std::filesystem::path tactical_corpus{"resources/tactical-corpus.json"};
    bool tactical_corpus_enabled{true};
    std::string chesscom_username;
    std::vector<std::string> trusted_hosts;
    std::vector<std::string> allowed_origins;
    bool require_auth{false};
    bool require_auth_explicit{false};
    // Only the local/container smoke harness may opt into an unauthenticated remote bind. The
    // default remains fail-closed so a production config typo cannot expose the API.
    bool allow_insecure_remote{false};
    std::string postgres_connection;
    std::string oidc_issuer;
    std::string oidc_audience{"authenticated"};
    std::string oidc_provider{"supabase"};
    std::string oidc_jwks_url;
};

std::optional<std::string> environment(std::string_view name) {
    if (const char* value = std::getenv(std::string(name).c_str());
        value != nullptr && value[0] != '\0') {
        return value;
    }
    return std::nullopt;
}

std::vector<std::string> comma_separated(std::string_view value) {
    std::vector<std::string> result;
    while (!value.empty()) {
        const std::size_t separator = value.find(',');
        std::string entry(value.substr(0, separator));
        const std::size_t first = entry.find_first_not_of(" \t");
        const std::size_t last = entry.find_last_not_of(" \t");
        if (first != std::string::npos)
            result.push_back(entry.substr(first, last - first + 1));
        if (separator == std::string_view::npos)
            break;
        value.remove_prefix(separator + 1);
    }
    return result;
}

unsigned long bounded_number(std::string_view value, std::string_view name,
                             unsigned long minimum, unsigned long maximum) {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(std::string(value), &consumed);
    if (consumed != value.size() || parsed < minimum || parsed > maximum) {
        throw std::runtime_error(std::string(name) + " must be between " +
                                 std::to_string(minimum) + " and " +
                                 std::to_string(maximum));
    }
    return parsed;
}

bool boolean_option(std::string_view value, std::string_view name) {
    if (value == "1" || value == "true")
        return true;
    if (value == "0" || value == "false")
        return false;
    throw std::runtime_error(std::string(name) + " must be true or false");
}

Options environment_options() {
    Options options;
    if (const auto value = environment("PCT_DATA_DIR"))
        options.data_dir = *value;
    if (const auto value = environment("PCT_WEB_ROOT"))
        options.web_root = *value;
    if (const auto value = environment("PCT_STOCKFISH"))
        options.stockfish = *value;
    if (const auto value = environment("PCT_BIND_ADDRESS"))
        options.bind_address = *value;
    if (const auto value = environment("PCT_PORT")) {
        options.port = static_cast<std::uint16_t>(
            bounded_number(*value, "PCT_PORT", 1, 65535));
    }
    if (const auto value = environment("PCT_WORKERS"))
        options.workers = bounded_number(*value, "PCT_WORKERS", 1, 16);
    if (const auto value = environment("PCT_MAX_PENDING"))
        options.max_pending = bounded_number(*value, "PCT_MAX_PENDING", 1, 10000);
    if (const auto value = environment("PCT_RETRY_LIMIT"))
        options.retry_limit = bounded_number(*value, "PCT_RETRY_LIMIT", 0, 10);
    if (const auto value = environment("PCT_TACTICAL_CORPUS"))
        options.tactical_corpus = *value;
    if (const auto value = environment("PCT_CHESSCOM_USERNAME"))
        options.chesscom_username = *value;
    if (const auto value = environment("PCT_TRUSTED_HOSTS"))
        options.trusted_hosts = comma_separated(*value);
    if (const auto value = environment("PCT_ALLOWED_ORIGINS"))
        options.allowed_origins = comma_separated(*value);
    if (const auto value = environment("PCT_REQUIRE_AUTH")) {
        options.require_auth = boolean_option(*value, "PCT_REQUIRE_AUTH");
        options.require_auth_explicit = true;
    }
    if (const auto value = environment("PCT_ALLOW_INSECURE_REMOTE"))
        options.allow_insecure_remote = boolean_option(*value, "PCT_ALLOW_INSECURE_REMOTE");
    if (const auto value = environment("PCT_POSTGRES_URL"))
        options.postgres_connection = *value;
    if (const auto value = environment("PCT_OIDC_ISSUER"))
        options.oidc_issuer = *value;
    if (const auto value = environment("PCT_OIDC_AUDIENCE"))
        options.oidc_audience = *value;
    if (const auto value = environment("PCT_OIDC_PROVIDER"))
        options.oidc_provider = *value;
    if (const auto value = environment("PCT_OIDC_JWKS_URL"))
        options.oidc_jwks_url = *value;
    if (const auto value = environment("PCT_SUPABASE_URL")) {
        std::string base = *value;
        while (base.ends_with('/'))
            base.pop_back();
        if (options.oidc_issuer.empty())
            options.oidc_issuer = base + "/auth/v1";
        if (options.oidc_jwks_url.empty())
            options.oidc_jwks_url = options.oidc_issuer + "/.well-known/jwks.json";
    }
    return options;
}

Options parse_options(int argc, char** argv) {
    Options options = environment_options();
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto value = [&]() -> std::string {
            if (++index >= argc)
                throw std::runtime_error("missing value for " + std::string(argument));
            return argv[index];
        };
        if (argument == "--data-dir")
            options.data_dir = value();
        else if (argument == "--web-root")
            options.web_root = value();
        else if (argument == "--stockfish")
            options.stockfish = value();
        else if (argument == "--bind-address")
            options.bind_address = value();
        else if (argument == "--port") {
            const unsigned long port = bounded_number(value(), "port", 1, 65535);
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--workers") {
            options.workers = bounded_number(value(), "workers", 1, 16);
        } else if (argument == "--max-pending") {
            options.max_pending = bounded_number(value(), "max-pending", 1, 10000);
        } else if (argument == "--retry-limit") {
            options.retry_limit = bounded_number(value(), "retry-limit", 0, 10);
        } else if (argument == "--tactical-corpus") {
            options.tactical_corpus = value();
        } else if (argument == "--no-tactical-corpus") {
            options.tactical_corpus_enabled = false;
        } else if (argument == "--chesscom-username") {
            options.chesscom_username = value();
        } else if (argument == "--trusted-host") {
            options.trusted_hosts.push_back(value());
        } else if (argument == "--allowed-origin") {
            options.allowed_origins.push_back(value());
        } else if (argument == "--require-auth") {
            options.require_auth = true;
            options.require_auth_explicit = true;
        } else if (argument == "--help") {
            std::cout << "usage: personal-chess-tutor [--data-dir path] [--web-root path] "
                         "[--stockfish path] [--bind-address IPv4] [--port number] "
                         "[--workers 1-16] "
                         "[--max-pending count] [--retry-limit count] "
                         "[--chesscom-username public-name] "
                         "[--trusted-host hostname] [--allowed-origin https-origin] "
                         "[--require-auth] "
                         "[--tactical-corpus path | --no-tactical-corpus]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + std::string(argument));
        }
    }
    if (!options.require_auth_explicit && options.bind_address != "127.0.0.1")
        options.require_auth = true;
    if (options.bind_address != "127.0.0.1" && !options.require_auth &&
        !options.allow_insecure_remote)
        throw std::runtime_error(
            "remote HTTP binds require PCT_REQUIRE_AUTH=true; use 127.0.0.1 for "
            "unauthenticated development");
    if (options.bind_address != "127.0.0.1" && !options.require_auth &&
        options.allow_insecure_remote)
        std::cerr << "warning: unauthenticated remote HTTP bind explicitly enabled\n";
    if (options.bind_address != "127.0.0.1" && options.require_auth &&
        (options.postgres_connection.empty() || options.oidc_issuer.empty() ||
         options.oidc_jwks_url.empty()))
        throw std::runtime_error(
            "authenticated remote HTTP binds require PCT_POSTGRES_URL and "
            "PCT_OIDC_ISSUER/PCT_OIDC_JWKS_URL (or PCT_SUPABASE_URL)");
    if (options.bind_address != "127.0.0.1" && options.require_auth &&
        (options.trusted_hosts.empty() || options.allowed_origins.empty()))
        throw std::runtime_error(
            "authenticated remote HTTP binds require PCT_TRUSTED_HOSTS and "
            "PCT_ALLOWED_ORIGINS");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        stop_requested = 0;
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);

        std::filesystem::create_directories(options.data_dir);
        pct::storage::EventLog event_log(options.data_dir / "events.log");
        if (event_log.replay().truncated_tail) {
            if (event_log.recover_trailing_record()) {
                pct::log(pct::LogLevel::Warning, "storage", "recovered a partial trailing record");
            }
        }
        pct::app::Repository repository(event_log);
        pct::import::ImportService importer;
        bool engine_ready = false;
        std::string engine_identity = options.stockfish;
        try {
            engine_identity = pct::engine::Stockfish::resolve_executable(options.stockfish);
            pct::engine::Stockfish probe(
                pct::engine::StockfishOptions{engine_identity, 16, 1});
            probe.start();
            probe.stop();
            engine_ready = true;
        } catch (const std::exception& error) {
            pct::log(pct::LogLevel::Warning, "stockfish",
                     "engine readiness probe failed: " + std::string(error.what()));
        }
        pct::engine::EnginePool engines(
            [&](std::size_t) {
                return std::make_unique<pct::engine::Stockfish>(
                    pct::engine::StockfishOptions{options.stockfish, 128, 1});
            },
            pct::engine::EnginePoolOptions{options.workers, options.max_pending,
                                           options.retry_limit});
        pct::analysis::AnalysisCache cache;
        pct::analysis::Analyzer analyzer(engines, cache);
        pct::app::JobManager jobs(
            repository, analyzer,
            pct::app::JobManagerOptions{options.workers, options.max_pending,
                                        options.retry_limit});
        pct::app::IngestManager ingest(importer, repository, jobs, {}, {}, {},
                                       options.chesscom_username);
        std::unique_ptr<pct::service::HostedRuntime> hosted_runtime;
        pct::service::AuthConfig auth{options.require_auth, {}};
        const bool hosted_storage_configured = !options.postgres_connection.empty();
        const bool hosted_identity_configured = !options.oidc_issuer.empty() ||
                                                !options.oidc_jwks_url.empty();
        if (hosted_storage_configured || hosted_identity_configured) {
#if defined(PCT_HAS_OIDC) && defined(PCT_HAS_POSTGRES)
            if (!hosted_storage_configured || !hosted_identity_configured ||
                options.oidc_issuer.empty() || options.oidc_jwks_url.empty())
                throw std::runtime_error(
                    "hosted runtime requires PCT_POSTGRES_URL, PCT_OIDC_ISSUER, and "
                    "PCT_OIDC_JWKS_URL (or PCT_SUPABASE_URL)");
            hosted_runtime = std::make_unique<pct::service::HostedRuntime>(
                pct::service::HostedRuntimeOptions{
                    options.postgres_connection,
                    options.oidc_issuer,
                    options.oidc_audience,
                    options.oidc_provider,
                    options.oidc_jwks_url,
                },
                analyzer,
                pct::app::JobManagerOptions{options.workers, options.max_pending,
                                            options.retry_limit});
            auth.required = true;
            auth.allow_guest_access = false;
            auth.allow_shared_ingest = false;
            auth.verify = hosted_runtime->token_verifier();
            auth.resolve_scope = hosted_runtime->scope_resolver();
            auth.create_guest_session = hosted_runtime->guest_session_creator();
            auth.reserve_guest_analysis = hosted_runtime->guest_analysis_reservation();
            auth.begin_browser_observation = hosted_runtime->browser_observation_begin();
            auth.submit_browser_observation = hosted_runtime->browser_observation_submit();
            auth.finalize_browser_observation = hosted_runtime->browser_observation_finalize();
            auth.claim_guest = hosted_runtime->guest_claim_handler();
            auth.verify_fresh = hosted_runtime->fresh_token_verifier();
            auth.export_account = hosted_runtime->account_export_handler();
            auth.delete_account = hosted_runtime->account_deletion_handler();
#else
            throw std::runtime_error(
                "hosted runtime requires a build with PostgreSQL and OpenSSL support");
#endif
        }
        std::unique_ptr<pct::training::AdvancedDrillGenerator> advanced_drills;
        if (options.tactical_corpus_enabled && std::filesystem::exists(options.tactical_corpus)) {
            advanced_drills = std::make_unique<pct::training::AdvancedDrillGenerator>(
                pct::training::TacticalCorpus::load(options.tactical_corpus),
                [&] {
                    return std::make_unique<pct::engine::Stockfish>(
                        pct::engine::StockfishOptions{options.stockfish, 64, 1});
                });
        }
        pct::service::Api api(importer, repository, jobs, [&] {
            const auto stats = engines.stats();
            const auto count = [](std::uint64_t value) {
                return static_cast<std::size_t>(value);
            };
            return pct::json::Value::Object{
                {"engine_workers", engines.worker_count()},
                {"engine_submitted", count(stats.submitted)},
                {"engine_completed", count(stats.completed)},
                {"engine_failed", count(stats.failed)},
                {"engine_retried", count(stats.retried)},
                {"engine_rejected", count(stats.rejected)},
                {"engine_active", count(stats.active)},
                {"queued_interactive", count(stats.queued_interactive)},
                {"queued_current_game", count(stats.queued_current_game)},
                {"queued_historical", count(stats.queued_historical)},
                {"maximum_queue_latency_ms", count(stats.maximum_queue_latency_ms)},
            };
        }, [&] {
            if (!advanced_drills)
                return std::vector<pct::training::Drill>{};
            return advanced_drills->generate(repository.profile(), repository.drills(0), 5);
        }, &ingest, [=] {
            return pct::service::Readiness{
                true, engine_ready, options.bind_address == "127.0.0.1", engine_identity};
        }, auth);
        pct::service::HttpServer server(
            api, jobs,
            pct::service::ServerOptions{options.port, options.web_root, options.bind_address,
                                        options.trusted_hosts, options.allowed_origins},
            &ingest);
        if (!std::filesystem::exists(options.web_root / "index.html")) {
            const bool local_only = options.bind_address == "127.0.0.1";
            pct::log(local_only ? pct::LogLevel::Warning : pct::LogLevel::Info, "http",
                     local_only
                         ? "frontend build is missing; run npm run build --prefix web"
                         : "frontend build is not mounted; serving the API only");
        }
        std::jthread signal_watcher([&](std::stop_token token) {
            while (!token.stop_requested() && stop_requested == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (stop_requested != 0) {
                pct::log(pct::LogLevel::Info, "runtime",
                         "shutdown requested; stopping HTTP admission");
                server.stop();
            }
        });
        server.run();
        signal_watcher.request_stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
