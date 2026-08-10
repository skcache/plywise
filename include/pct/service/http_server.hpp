#pragma once

#include "pct/analysis/browser_observation.hpp"
#include "pct/app/job_manager.hpp"
#include "pct/app/ingest_manager.hpp"
#include "pct/app/repository.hpp"
#include "pct/import/import_service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pct::service {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct Response {
    int status{200};
    std::map<std::string, std::string> headers;
    std::string body;
};

struct Readiness {
    bool storage_ready{true};
    bool engine_ready{true};
    bool local_only{true};
    std::string engine;
};

// A request scope keeps the repository and job queue selected by the authenticated owner
// together. Hosted callers must return objects that remain alive for the duration of the request;
// the API never stores these pointers between requests.
struct ApiScope {
    app::IRepository* repository{nullptr};
    app::JobManager* jobs{nullptr};
    // Hosted scopes pin their owner resources while request or websocket code uses these
    // pointers. Local and test scopes leave the lifetime empty.
    std::shared_ptr<void> lifetime;
};

struct GuestSessionCredential {
    std::string guest_id;
    std::string token;
    std::int64_t expires_at_ms{0};
};

struct GuestClaimResult {
    std::string guest_id;
    std::string account_id;
    std::size_t transferred_games{0};
    bool already_claimed{false};
};

struct AccountExportResult {
    std::string request_id;
    json::Value data;
    std::int64_t completed_at_ms{0};
};

struct AccountDeletionResult {
    std::string request_id;
    std::string receipt_token;
    std::int64_t completed_at_ms{0};
    std::int64_t backup_retention_until_ms{0};
};

struct WebSocketTicketCredential {
    std::string ticket;
    std::int64_t expires_at_ms{0};
};

struct AuthConfig {
    using TokenVerifier = std::function<std::optional<app::OwnerId>(std::string_view)>;
    using ScopeResolver = std::function<std::optional<ApiScope>(const app::OwnerId&)>;
    using GuestSessionCreator = std::function<std::optional<GuestSessionCredential>()>;
    using GuestAnalysisReservation =
        std::function<void(const app::OwnerId& guest, std::string_view game_id)>;
    using BrowserObservationBegin =
        std::function<void(const app::OwnerId& owner,
                           const analysis::BrowserObservationRunContext& context)>;
    using BrowserObservationSubmit = std::function<analysis::BrowserObservationReceipt(
        const app::OwnerId& owner, const analysis::BrowserObservationContext& context,
        const analysis::BrowserEngineObservation& observation)>;
    using BrowserObservationFinalize = std::function<analysis::BrowserObservationBundle(
        const app::OwnerId& owner, std::string_view game_id, std::string_view analysis_run_id)>;
    using GuestClaimHandler = std::function<GuestClaimResult(
        std::string_view token, const app::OwnerId& account, std::string_view idempotency_key)>;
    using FreshTokenVerifier = std::function<std::optional<app::OwnerId>(std::string_view)>;
    using AccountExportHandler = std::function<AccountExportResult(
        const app::OwnerId& account, std::string_view idempotency_key)>;
    using AccountDeletionHandler = std::function<AccountDeletionResult(
        const app::OwnerId& account, std::string_view idempotency_key)>;
    using WebSocketTicketIssuer =
        std::function<std::optional<WebSocketTicketCredential>(const app::OwnerId& owner)>;
    using WebSocketTicketVerifier =
        std::function<std::optional<app::OwnerId>(std::string_view ticket)>;

    // Hosted mode must provide a verifier. An empty verifier fails closed with 503.
    bool required{false};
    TokenVerifier verify;
    // When present, authenticated requests are routed to an owner-scoped repository and job
    // manager. Without it, the API retains the fixed local repository behavior.
    ScopeResolver resolve_scope;
    // These callbacks keep guest token generation and claim transactions behind the hosted
    // runtime. The API only validates the request shape and serializes typed results.
    GuestSessionCreator create_guest_session;
    // Hosted mode atomically consumes the guest's one free analysis before work is queued.
    GuestAnalysisReservation reserve_guest_analysis;
    // Hosted mode persists browser observations so an interrupted run can resume after restart.
    BrowserObservationBegin begin_browser_observation;
    BrowserObservationSubmit submit_browser_observation;
    BrowserObservationFinalize finalize_browser_observation;
    GuestClaimHandler claim_guest;
    FreshTokenVerifier verify_fresh;
    AccountExportHandler export_account;
    AccountDeletionHandler delete_account;
    // Hosted WebSockets use a short-lived, single-use ticket instead of putting the full
    // account bearer token in Sec-WebSocket-Protocol. Local mode can leave these unset.
    WebSocketTicketIssuer issue_websocket_ticket;
    WebSocketTicketVerifier verify_websocket_ticket;
    // Guest review is retained for local compatibility tests, but hosted deployments disable it
    // explicitly so every public product flow starts behind account authentication.
    bool allow_guest_access{true};
    // The current ingest manager is process-scoped. Hosted accounts must not share it until an
    // owner-scoped persistent adapter is wired, so hosted main disables the async profile/sync
    // routes while direct single-game imports remain available through the scoped repository.
    bool allow_shared_ingest{false};
    // Hosted limits keep the free service predictable for a small private alpha. They are only
    // applied when resolve_scope is configured; local mode remains unlimited for compatibility.
    std::size_t hosted_imports_per_window{100};
    std::size_t hosted_analysis_starts_per_window{30};
    // Process-wide ceilings keep a burst of newly-created accounts from multiplying work
    // without bound. They apply only when resolve_scope is configured.
    std::size_t hosted_global_imports_per_window{200};
    std::size_t hosted_global_analysis_starts_per_window{30};
    std::chrono::seconds hosted_quota_window{std::chrono::minutes(1)};
    std::size_t hosted_game_storage_limit{app::hosted_game_storage_limit};
};

class Api {
  public:
    using Diagnostics = std::function<json::Value()>;
    using AdvancedDrills = std::function<std::vector<training::Drill>()>;
    using ReadinessCheck = std::function<Readiness()>;

    Api(import::ImportService& importer, app::IRepository& repository, app::JobManager& jobs,
        Diagnostics diagnostics = {}, AdvancedDrills advanced_drills = {},
        app::IngestManager* ingest = nullptr, ReadinessCheck readiness = {},
        AuthConfig auth = {})
        : importer_(importer), default_repository_(repository), default_jobs_(jobs),
          diagnostics_(std::move(diagnostics)), advanced_drills_(std::move(advanced_drills)),
          ingest_(ingest), readiness_(std::move(readiness)), auth_(std::move(auth)) {}

    [[nodiscard]] Response handle(const Request& request);
    [[nodiscard]] std::optional<Response> authorize(
        const Request& request, std::optional<app::OwnerId>* authenticated_owner = nullptr) const;
    [[nodiscard]] std::optional<ApiScope> scope_for_owner(const app::OwnerId& owner) const;
    [[nodiscard]] bool has_websocket_ticket_verifier() const noexcept {
        return static_cast<bool>(auth_.verify_websocket_ticket);
    }
    [[nodiscard]] bool has_scoped_authorization() const noexcept {
        return static_cast<bool>(auth_.resolve_scope);
    }

  private:
    enum class QuotaKind { Import, AnalysisStart };

    struct OwnerQuotaWindow {
        std::deque<std::chrono::steady_clock::time_point> imports;
        std::deque<std::chrono::steady_clock::time_point> analysis_starts;
        std::chrono::steady_clock::time_point last_used{};
    };

    struct Authentication {
        std::optional<app::OwnerId> owner;
        std::optional<Response> denial;
    };

    [[nodiscard]] Authentication authenticate(const Request& request) const;
    [[nodiscard]] bool consume_hosted_quota(const app::OwnerId& owner, QuotaKind kind,
                                             std::size_t amount = 1);

    import::ImportService& importer_;
    app::IRepository& default_repository_;
    app::JobManager& default_jobs_;
    Diagnostics diagnostics_;
    AdvancedDrills advanced_drills_;
    app::IngestManager* ingest_{nullptr};
    ReadinessCheck readiness_;
    AuthConfig auth_;
    analysis::BrowserObservationLedger browser_observations_;
    std::mutex quota_mutex_;
    std::map<std::string, OwnerQuotaWindow> quota_windows_;
    OwnerQuotaWindow global_quota_window_;
};

struct ServerOptions {
    std::uint16_t port{8787};
    std::filesystem::path web_root{"web/dist"};
    std::string bind_address{"127.0.0.1"};
    std::vector<std::string> trusted_hosts;
    std::vector<std::string> allowed_origins;
    // The C++ server closes each HTTP connection after one request, so this is a request rate
    // limiter even for WebSocket handshakes and malformed/auth-failing requests. The global cap
    // is a second guard when a reverse proxy collapses all clients onto one backend peer.
    std::size_t requests_per_peer_window{600};
    std::size_t requests_per_window{2400};
    std::chrono::seconds request_rate_window{std::chrono::minutes(1)};
    std::size_t max_rate_limit_peers{4096};
};

class HttpServer {
  public:
    HttpServer(Api& api, app::JobManager& jobs, ServerOptions options = {},
               app::IngestManager* ingest = nullptr);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void run();
    void stop() noexcept;
    void broadcast(std::string_view message);
    [[nodiscard]] std::uint16_t bound_port() const noexcept {
        return bound_port_.load(std::memory_order_acquire);
    }
    [[nodiscard]] static bool valid_websocket_origin(std::string_view origin);

  private:
    Api& api_;
    app::JobManager& jobs_;
    app::IngestManager* ingest_{nullptr};
    ServerOptions options_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    std::atomic<int> listen_fd_{-1};
    std::mutex clients_mutex_;
    struct WebSocketClient {
        explicit WebSocketClient(int input_fd, std::optional<app::OwnerId> input_owner)
            : fd(input_fd), owner(std::move(input_owner)) {}

        int fd{-1};
        std::optional<app::OwnerId> owner;
        std::mutex io_mutex;
        bool retired{false};
    };
    using WebSocketClientPtr = std::shared_ptr<WebSocketClient>;
    // Membership is short-lived under clients_mutex_; socket I/O is serialized per client so a
    // stalled peer cannot block broadcasts or admission of other peers.
    std::vector<WebSocketClientPtr> websocket_clients_;
    std::mutex scoped_observers_mutex_;
    struct ScopedObserver {
        app::JobManager::ObserverId observer_id{0};
        std::shared_ptr<void> lifetime;
    };
    std::map<app::JobManager*, ScopedObserver> scoped_observers_;
    std::mutex client_threads_mutex_;
    std::condition_variable client_threads_cv_;
    std::size_t active_client_threads_{0};
    static constexpr std::size_t max_active_client_threads_{128};

    struct RateLimitWindow {
        std::deque<std::chrono::steady_clock::time_point> requests;
    };
    std::mutex rate_limit_mutex_;
    std::map<std::string, RateLimitWindow> rate_limit_peers_;
    RateLimitWindow rate_limit_global_;

    void handle_client(int client_fd);
    [[nodiscard]] bool consume_request_rate_limit(std::string_view peer);
    void handle_websocket(int client_fd, const Request& request);
    [[nodiscard]] static bool send_websocket_frame(const WebSocketClientPtr& client,
                                                    std::string_view frame);
    static void retire_websocket_client(const WebSocketClientPtr& client) noexcept;
    static void close_websocket_client(const WebSocketClientPtr& client) noexcept;
    void remove_websocket_client(const WebSocketClientPtr& client) noexcept;
    void subscribe_to_owner_events(app::JobManager& jobs, const app::OwnerId& owner,
                                    std::shared_ptr<void> lifetime = {});
    void remove_scoped_observers() noexcept;
    [[nodiscard]] Response static_file(std::string_view path) const;
    [[nodiscard]] bool host_allowed(std::string_view host) const;
    [[nodiscard]] bool origin_allowed(std::string_view origin) const;
    void broadcast_to_owner(const app::OwnerId& owner, std::string_view message);
};

} // namespace pct::service
