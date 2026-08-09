#pragma once

#include "pct/app/job_manager.hpp"
#include "pct/app/ingest_manager.hpp"
#include "pct/app/repository.hpp"
#include "pct/import/import_service.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

struct AuthConfig {
    using TokenVerifier = std::function<std::optional<app::OwnerId>(std::string_view)>;
    using ScopeResolver = std::function<std::optional<ApiScope>(const app::OwnerId&)>;
    using GuestSessionCreator = std::function<std::optional<GuestSessionCredential>()>;
    using GuestClaimHandler = std::function<GuestClaimResult(
        std::string_view token, const app::OwnerId& account, std::string_view idempotency_key)>;

    // Hosted mode must provide a verifier. An empty verifier fails closed with 503.
    bool required{false};
    TokenVerifier verify;
    // When present, authenticated requests are routed to an owner-scoped repository and job
    // manager. Without it, the API retains the fixed local repository behavior.
    ScopeResolver resolve_scope;
    // These callbacks keep guest token generation and claim transactions behind the hosted
    // runtime. The API only validates the request shape and serializes typed results.
    GuestSessionCreator create_guest_session;
    GuestClaimHandler claim_guest;
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
    [[nodiscard]] bool has_scoped_authorization() const noexcept {
        return static_cast<bool>(auth_.resolve_scope);
    }

  private:
    struct Authentication {
        std::optional<app::OwnerId> owner;
        std::optional<Response> denial;
    };

    [[nodiscard]] Authentication authenticate(const Request& request) const;

    import::ImportService& importer_;
    app::IRepository& default_repository_;
    app::JobManager& default_jobs_;
    Diagnostics diagnostics_;
    AdvancedDrills advanced_drills_;
    app::IngestManager* ingest_{nullptr};
    ReadinessCheck readiness_;
    AuthConfig auth_;
};

struct ServerOptions {
    std::uint16_t port{8787};
    std::filesystem::path web_root{"web/dist"};
    std::string bind_address{"127.0.0.1"};
    std::vector<std::string> trusted_hosts;
    std::vector<std::string> allowed_origins;
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
        int fd{-1};
        std::optional<app::OwnerId> owner;
    };
    std::vector<WebSocketClient> websocket_clients_;
    std::mutex scoped_observers_mutex_;
    std::map<app::JobManager*, app::JobManager::ObserverId> scoped_observers_;
    std::mutex client_threads_mutex_;
    std::vector<std::thread> client_threads_;

    void handle_client(int client_fd);
    void handle_websocket(int client_fd, const Request& request);
    void subscribe_to_owner_events(app::JobManager& jobs, const app::OwnerId& owner);
    void remove_scoped_observers() noexcept;
    [[nodiscard]] Response static_file(std::string_view path) const;
    [[nodiscard]] bool host_allowed(std::string_view host) const;
    [[nodiscard]] bool origin_allowed(std::string_view origin) const;
    void broadcast_to_owner(const app::OwnerId& owner, std::string_view message);
};

} // namespace pct::service
