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

struct AuthConfig {
    using TokenVerifier = std::function<std::optional<app::OwnerId>(std::string_view)>;

    // Hosted mode must provide a verifier. An empty verifier fails closed with 503.
    bool required{false};
    TokenVerifier verify;
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
        : importer_(importer), repository_(repository), jobs_(jobs),
          diagnostics_(std::move(diagnostics)), advanced_drills_(std::move(advanced_drills)),
          ingest_(ingest), readiness_(std::move(readiness)), auth_(std::move(auth)) {}

    [[nodiscard]] Response handle(const Request& request);
    [[nodiscard]] std::optional<Response> authorize(const Request& request) const;

  private:
    import::ImportService& importer_;
    app::IRepository& repository_;
    app::JobManager& jobs_;
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
    std::vector<int> websocket_clients_;
    std::mutex client_threads_mutex_;
    std::vector<std::thread> client_threads_;

    void handle_client(int client_fd);
    void handle_websocket(int client_fd, const Request& request);
    [[nodiscard]] Response static_file(std::string_view path) const;
    [[nodiscard]] bool host_allowed(std::string_view host) const;
    [[nodiscard]] bool origin_allowed(std::string_view origin) const;
};

} // namespace pct::service
