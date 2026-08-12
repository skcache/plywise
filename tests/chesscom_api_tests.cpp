#include "test.hpp"

#include "pct/app/ingest_manager.hpp"
#include "pct/service/http_server.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace pct;

namespace {

class ApiIngestEngine final : public engine::AnalysisEngine {
  public:
    engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                   CancellationToken) override {
        chess::Board board = chess::Board::from_fen(request.fen);
        const auto moves = board.legal_moves();
        const std::string best = moves.empty() ? "(none)" : chess::uci(moves.front());
        return {{{1, request.depth, 0, std::nullopt, 1, 1, {best}}}, best, {}};
    }
};

struct ChessComApiFixture {
    std::filesystem::path path;
    storage::EventLog log;
    app::Repository repository;
    import::HttpTransport transport;
    import::ImportService importer;
    ApiIngestEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer;
    app::JobManager jobs;
    app::IngestManager ingest;
    service::Api api;

    explicit ChessComApiFixture(import::HttpTransport custom_transport = {})
        : path(std::filesystem::temp_directory_path() /
               ("pct-chesscom-api-" + std::to_string(::getpid()) + ".log")),
          log((std::filesystem::remove(path), path)), repository(log),
          transport(custom_transport ? std::move(custom_transport)
                                     : import::HttpTransport([](const import::HttpRequest& request) {
                                           if (request.url.ends_with("/archives"))
                                               return import::HttpResponse{200, {}, request.url,
                                                                            R"({"archives":[]})"};
                                           return import::HttpResponse{200, {}, request.url,
                                                                        "<html>no pgn</html>"};
                                       })),
          importer(transport), analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1}),
          jobs(repository, analyzer), ingest(importer, repository, jobs, transport),
          api(importer, repository, jobs, {}, {}, &ingest) {}
};

service::Request json_request(std::string method, std::string path, json::Value body) {
    return {std::move(method), std::move(path), {}, json::dump(std::move(body))};
}

int connect_loopback(std::uint16_t port) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string receive_available(int socket_fd) {
    std::string result;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pollfd descriptor{socket_fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0)
            continue;
        char buffer[4096];
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        result.append(buffer, static_cast<std::size_t>(count));
        if (result.find("\r\n\r\n") != std::string::npos)
            break;
    }
    return result;
}

bool wait_for_peer_close(int socket_fd) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        pollfd descriptor{socket_fd, POLLIN | POLLHUP, 0};
        if (poll(&descriptor, 1, 100) <= 0)
            continue;
        char buffer[4096];
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count == 0)
            return true;
        if (count < 0)
            return false;
    }
    return false;
}

} // namespace

TEST_CASE("Chess.com profile API persists only public profile fields") {
    ChessComApiFixture fixture;
    auto initial = fixture.api.handle({"GET", "/api/chesscom/profile", {}, {}});
    CHECK_EQ(initial.status, 200);
    CHECK(!json::parse(initial.body).at("connected").as_bool());

    auto forbidden = fixture.api.handle(json_request(
        "PUT", "/api/chesscom/profile",
        json::Value::Object{{"username", "Alice"}, {"token", "secret"}}));
    CHECK_EQ(forbidden.status, 400);
    CHECK_EQ(json::parse(forbidden.body).at("code").as_string(),
             "sensitive_fields_forbidden");
    CHECK(!fixture.repository.chesscom_profile().has_value());

    auto saved = fixture.api.handle(json_request(
        "PUT", "/api/chesscom/profile",
        json::Value::Object{{"username", "Alice-Player"},
                            {"time_controls", json::Value::Array{"rapid", "blitz"}}}));
    CHECK_EQ(saved.status, 200);
    CHECK_EQ(json::parse(saved.body).at("profile").at("normalized_username").as_string(),
             "alice-player");
}

TEST_CASE("hosted Chess.com profile routes stay inside the authenticated owner scope") {
    const auto suffix = std::to_string(::getpid());
    const auto global_path = std::filesystem::temp_directory_path() /
                             ("pct-chesscom-scope-global-" + suffix + ".log");
    const auto scoped_path = std::filesystem::temp_directory_path() /
                             ("pct-chesscom-scope-owner-" + suffix + ".log");
    storage::EventLog global_log((std::filesystem::remove(global_path), global_path));
    storage::EventLog scoped_log((std::filesystem::remove(scoped_path), scoped_path));
    app::Repository global_repository(global_log);
    app::Repository scoped_repository(scoped_log);
    import::ImportService importer;
    ApiIngestEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    app::JobManager global_jobs(global_repository, analyzer);
    app::JobManager scoped_jobs(scoped_repository, analyzer);
    app::IngestManager global_ingest(importer, global_repository, global_jobs);
    app::IngestManager scoped_ingest(importer, scoped_repository, scoped_jobs);

    service::AuthConfig auth;
    auth.required = true;
    auth.allow_shared_ingest = false;
    auth.verify = [](std::string_view token) -> std::optional<app::OwnerId> {
        return token == "owner-token" ? std::optional<app::OwnerId>(app::OwnerId::local())
                                       : std::nullopt;
    };
    auth.resolve_scope = [&](const app::OwnerId& owner) -> std::optional<service::ApiScope> {
        if (owner != scoped_repository.owner())
            return std::nullopt;
        return service::ApiScope{&scoped_repository, &scoped_jobs, {}, &scoped_ingest};
    };
    service::Api api(importer, global_repository, global_jobs, {}, {}, &global_ingest, {}, auth);

    auto put_profile = [&](std::string username) {
        auto request = json_request(
            "PUT", "/api/chesscom/profile",
            json::Value::Object{{"username", std::move(username)}});
        request.headers.emplace("authorization", "Bearer owner-token");
        return api.handle(std::move(request));
    };
    auto get_profile = [&] {
        service::Request request{"GET", "/api/chesscom/profile",
                                 {{"authorization", "Bearer owner-token"}}, {}};
        return api.handle(std::move(request));
    };

    CHECK_EQ(put_profile("Scoped-Player").status, 200);
    CHECK_EQ(json::parse(get_profile().body).at("profile").at("normalized_username").as_string(),
             "scoped-player");
    CHECK(!global_repository.chesscom_profile().has_value());
    CHECK(scoped_repository.chesscom_profile().has_value());
}

TEST_CASE("hosted Chess.com URL resolutions use the authenticated owner ingest manager") {
    const auto suffix = std::to_string(::getpid());
    const auto global_path = std::filesystem::temp_directory_path() /
                             ("pct-chesscom-resolution-global-" + suffix + ".log");
    const auto scoped_path = std::filesystem::temp_directory_path() /
                             ("pct-chesscom-resolution-owner-" + suffix + ".log");
    storage::EventLog global_log((std::filesystem::remove(global_path), global_path));
    storage::EventLog scoped_log((std::filesystem::remove(scoped_path), scoped_path));
    app::Repository global_repository(global_log);
    app::Repository scoped_repository(scoped_log);
    const import::HttpTransport transport([](const import::HttpRequest& request) {
        return import::HttpResponse{200, {}, request.url, "<html>no pgn</html>"};
    });
    import::ImportService importer(transport);
    ApiIngestEngine engine;
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1});
    app::JobManager global_jobs(global_repository, analyzer);
    app::JobManager scoped_jobs(scoped_repository, analyzer);
    app::IngestManager global_ingest(importer, global_repository, global_jobs, transport);
    app::IngestManager scoped_ingest(importer, scoped_repository, scoped_jobs, transport);

    service::AuthConfig auth;
    auth.required = true;
    auth.verify = [](std::string_view token) -> std::optional<app::OwnerId> {
        return token == "owner-token" ? std::optional<app::OwnerId>(app::OwnerId::local())
                                       : std::nullopt;
    };
    auth.resolve_scope = [&](const app::OwnerId& owner) -> std::optional<service::ApiScope> {
        if (owner != scoped_repository.owner())
            return std::nullopt;
        return service::ApiScope{&scoped_repository, &scoped_jobs, {}, &scoped_ingest};
    };
    auth.hosted_imports_per_window = 1;
    auth.hosted_global_imports_per_window = 1;
    service::Api api(importer, global_repository, global_jobs, {}, {}, &global_ingest, {}, auth);

    service::Request request = json_request(
        "POST", "/api/import",
        json::Value::Object{{"url", "https://www.chess.com/game/live/171626462440"}});
    request.headers.emplace("authorization", "Bearer owner-token");
    const auto queued = api.handle(std::move(request));
    CHECK_EQ(queued.status, 202);
    const auto body = json::parse(queued.body);
    CHECK_EQ(body.at("status").as_string(), "resolving");
    const std::string resolution_id = body.at("resolution_id").as_string();
    CHECK(scoped_ingest.resolution(resolution_id).has_value());
    CHECK(!global_ingest.resolution(resolution_id).has_value());

    service::Request status_request{
        "GET", "/api/import/resolutions/" + resolution_id,
        {{"authorization", "Bearer owner-token"}}, {}};
    const auto status = api.handle(std::move(status_request));
    CHECK_EQ(status.status, 200);
    CHECK_EQ(json::parse(status.body).at("id").as_string(), resolution_id);

    service::Request limited_resolution = json_request(
        "POST", "/api/import/resolve",
        json::Value::Object{{"url", "https://www.chess.com/game/live/171626462441"}});
    limited_resolution.headers.emplace("authorization", "Bearer owner-token");
    const auto resolution_limited = api.handle(std::move(limited_resolution));
    CHECK_EQ(resolution_limited.status, 429);
    CHECK_EQ(json::parse(resolution_limited.body).at("code").as_string(),
             "quota_exceeded");

    service::Request limited_sync = json_request(
        "POST", "/api/chesscom/sync", json::Value::Object{{"days", 30}});
    limited_sync.headers.emplace("authorization", "Bearer owner-token");
    const auto sync_limited = api.handle(std::move(limited_sync));
    CHECK_EQ(sync_limited.status, 429);
    CHECK_EQ(json::parse(sync_limited.body).at("code").as_string(), "quota_exceeded");
}

TEST_CASE("Chess.com sync archive and resolution API contracts are bounded and structured") {
    ChessComApiFixture fixture;
    fixture.ingest.configure_profile("Alice");
    const auto sync = fixture.api.handle(json_request(
        "POST", "/api/chesscom/sync", json::Value::Object{{"days", 30}}));
    CHECK_EQ(sync.status, 202);
    CHECK_EQ(json::parse(sync.body).at("max_months").as_size(), 2ULL);

    std::vector<app::ChessComArchiveEntry> entries;
    for (int id = 0; id < 205; ++id) {
        const std::string game_id = std::to_string(9000 + id);
        entries.push_back({game_id, "https://www.chess.com/game/live/" + game_id,
                           "[Event \"A\"]\n\n1. e4 e5 *", "alice", "2026-07",
                           "rapid", id, 1, "https://api.chess.com/archive"});
    }
    static_cast<void>(fixture.repository.index_chesscom_archive_chunk(std::move(entries)));
    const auto archive = fixture.api.handle(
        {"GET", "/api/chesscom/archive?limit=1000&username=Alice", {}, {}});
    CHECK_EQ(archive.status, 200);
    CHECK_EQ(json::parse(archive.body).at("entries").as_array().size(),
             app::chesscom_archive_search_limit);

    const auto resolution = fixture.api.handle(json_request(
        "POST", "/api/import/resolve",
        json::Value::Object{{"url", "https://www.chess.com/game/live/171626462440"}}));
    CHECK_EQ(resolution.status, 202);
    CHECK(json::parse(resolution.body).as_object().contains("id"));
}

TEST_CASE("legacy manual PGN import remains compatible while remote URL returns resolution id") {
    ChessComApiFixture fixture;
    const auto manual = fixture.api.handle(json_request(
        "POST", "/api/import",
        json::Value::Object{{"pgn", "[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"}}));
    CHECK_EQ(manual.status, 202);
    const auto manual_body = json::parse(manual.body);
    CHECK(manual_body.as_object().contains("game_id"));
    CHECK(!manual_body.as_object().contains("job"));
    CHECK(fixture.jobs.list().empty());

    const auto remote = fixture.api.handle(json_request(
        "POST", "/api/import",
        json::Value::Object{{"url", "https://www.chess.com/game/live/171626462440"}}));
    CHECK_EQ(remote.status, 202);
    CHECK(json::parse(remote.body).as_object().contains("resolution_id"));
}

TEST_CASE("hosted scoped import resolves a client-rendered game through the callback endpoint") {
    const std::string game_id = "171626462440";
    const std::string callback = json::dump(json::Value::Object{
        {"game", json::Value::Object{
                      {"id", static_cast<double>(171626462440ULL)},
                      {"moveList", "mC0Kgv5Q"},
                      {"plyCount", 4},
                      {"pgnHeaders", json::Value::Object{
                                           {"Event", "Live Chess"},
                                           {"White", "Alice"},
                                           {"Black", "Bob"},
                                           {"Result", "1-0"},
                                       }},
                  }},
    });
    ChessComApiFixture fixture([&](const import::HttpRequest& request) {
        if (request.url == "https://www.chess.com/game/live/" + game_id)
            return import::HttpResponse{200, {}, request.url, "<html>no pgn</html>"};
        if (request.url == "https://www.chess.com/callback/live/game/" + game_id)
            return import::HttpResponse{200, {}, request.url, callback};
        throw std::runtime_error("unexpected Chess.com request: " + request.url);
    });
    service::AuthConfig auth;
    auth.required = true;
    auth.verify = [](std::string_view token) -> std::optional<app::OwnerId> {
        return token == "test-token" ? std::optional<app::OwnerId>(app::OwnerId::local())
                                      : std::nullopt;
    };
    auth.resolve_scope = [&](const app::OwnerId& owner) -> std::optional<service::ApiScope> {
        if (owner != fixture.repository.owner())
            return std::nullopt;
        return service::ApiScope{&fixture.repository, &fixture.jobs, {}};
    };
    auth.allow_shared_ingest = false;
    service::Api hosted_api(fixture.importer, fixture.repository, fixture.jobs, {}, {},
                            &fixture.ingest, {}, std::move(auth));
    service::Request request = json_request(
        "POST", "/api/import",
        json::Value::Object{{"url", "https://www.chess.com/game/live/" + game_id}});
    request.headers.emplace("authorization", "Bearer test-token");

    const auto imported = hosted_api.handle(request);
    CHECK_EQ(imported.status, 202);
    const auto body = json::parse(imported.body);
    CHECK_EQ(body.at("status").as_string(), "imported");
    CHECK(!body.at("game_id").as_string().empty());
}

TEST_CASE("cached URL import accepts current Chess.com generic Site PGN") {
    ChessComApiFixture fixture;
    const std::string id = "171626817794";
    const std::string pgn =
        "[Event \"Live Chess\"]\n[Site \"Chess.com\"]\n[Date \"2026.07.15\"]\n"
        "[White \"Hikaru\"]\n[Black \"Opponent\"]\n[Result \"1-0\"]\n\n"
        "1. e4 e5 1-0";
    static_cast<void>(fixture.repository.index_chesscom_archive_chunk({
        {id, "https://www.chess.com/game/live/" + id, pgn, "hikaru", "2026-07",
         "rapid", 1, 2, "https://api.chess.com/pub/player/hikaru/games/2026/07"}}));

    const auto imported = fixture.api.handle(json_request(
        "POST", "/api/import",
        json::Value::Object{{"url", "https://www.chess.com/game/live/" + id}}));
    CHECK_EQ(imported.status, 202);
    const auto imported_body = json::parse(imported.body);
    CHECK_EQ(imported_body.at("status").as_string(), "imported");
    CHECK(!imported_body.as_object().contains("job"));
    CHECK(fixture.jobs.list().empty());
}

TEST_CASE("HTTP and WebSocket lifecycle enforce configured browser authorities") {
    ChessComApiFixture fixture;
    const std::filesystem::path static_root =
        std::filesystem::temp_directory_path() /
        ("pct-static-root-" + std::to_string(::getpid()));
    const std::filesystem::path outside_file =
        std::filesystem::temp_directory_path() /
        ("pct-static-outside-" + std::to_string(::getpid()) + ".txt");
    std::filesystem::remove_all(static_root);
    std::filesystem::create_directories(static_root);
    {
        std::ofstream(static_root / "index.html") << "static index";
        std::ofstream(outside_file) << "outside sentinel";
    }
    const std::filesystem::path outside_link = static_root / "outside-link";
    std::filesystem::create_symlink(outside_file, outside_link);
    service::HttpServer server(
        fixture.api, fixture.jobs,
        service::ServerOptions{0, static_root, "127.0.0.1", {"api.plywise.test"},
                               {"https://app.plywise.test"}},
                               &fixture.ingest);
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    for (int attempt = 0; attempt < 200 && server.bound_port() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const std::uint16_t port = server.bound_port();
    const int rejected_http_origin = port == 0 ? -1 : connect_loopback(port);
    std::string rejected_http_origin_response;
    if (rejected_http_origin >= 0) {
        const std::string request =
            "POST /api/import HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nOrigin: https://evil.example\r\nContent-Type: text/plain\r\n"
            "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
        static_cast<void>(send(rejected_http_origin, request.data(), request.size(), 0));
        rejected_http_origin_response = receive_available(rejected_http_origin);
        close(rejected_http_origin);
    }

    const int rejected_http_host = port == 0 ? -1 : connect_loopback(port);
    std::string rejected_http_host_response;
    if (rejected_http_host >= 0) {
        const std::string request =
            "GET /api/health HTTP/1.1\r\nHost: evil.example\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(rejected_http_host, request.data(), request.size(), 0));
        rejected_http_host_response = receive_available(rejected_http_host);
        close(rejected_http_host);
    }

    const int duplicate_content_length = port == 0 ? -1 : connect_loopback(port);
    std::string duplicate_content_length_response;
    if (duplicate_content_length >= 0) {
        const std::string request =
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: 0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(duplicate_content_length, request.data(), request.size(), 0));
        duplicate_content_length_response = receive_available(duplicate_content_length);
        close(duplicate_content_length);
    }

    const int transfer_encoding = port == 0 ? -1 : connect_loopback(port);
    std::string transfer_encoding_response;
    if (transfer_encoding >= 0) {
        const std::string request =
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(transfer_encoding, request.data(), request.size(), 0));
        transfer_encoding_response = receive_available(transfer_encoding);
        close(transfer_encoding);
    }

    const int accepted_http = port == 0 ? -1 : connect_loopback(port);
    std::string accepted_http_response;
    if (accepted_http >= 0) {
        const std::string request =
            "GET /api/health HTTP/1.1\r\nHost: localhost:" + std::to_string(port) +
            "\r\nOrigin: http://localhost:" + std::to_string(port) +
            "\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(accepted_http, request.data(), request.size(), 0));
        accepted_http_response = receive_available(accepted_http);
        close(accepted_http);
    }

    const int rejected_absolute_static = port == 0 ? -1 : connect_loopback(port);
    std::string rejected_absolute_static_response;
    if (rejected_absolute_static >= 0) {
        const std::string request =
            "GET /" + outside_file.generic_string() +
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(rejected_absolute_static, request.data(), request.size(), 0));
        rejected_absolute_static_response = receive_available(rejected_absolute_static);
        close(rejected_absolute_static);
    }

    const int accepted_static = port == 0 ? -1 : connect_loopback(port);
    std::string accepted_static_response;
    if (accepted_static >= 0) {
        const std::string request =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(accepted_static, request.data(), request.size(), 0));
        accepted_static_response = receive_available(accepted_static);
        close(accepted_static);
    }

    const int rejected_symlink_static = port == 0 ? -1 : connect_loopback(port);
    std::string rejected_symlink_static_response;
    if (rejected_symlink_static >= 0) {
        constexpr std::string_view request =
            "GET /outside-link HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(rejected_symlink_static, request.data(), request.size(), 0));
        rejected_symlink_static_response = receive_available(rejected_symlink_static);
        close(rejected_symlink_static);
    }

    const int accepted_preflight = port == 0 ? -1 : connect_loopback(port);
    std::string accepted_preflight_response;
    if (accepted_preflight >= 0) {
        const std::string request =
            "OPTIONS /api/games HTTP/1.1\r\nHost: api.plywise.test\r\n"
            "Origin: https://app.plywise.test\r\n"
            "Access-Control-Request-Method: PUT\r\nConnection: close\r\n\r\n";
        static_cast<void>(send(accepted_preflight, request.data(), request.size(), 0));
        accepted_preflight_response = receive_available(accepted_preflight);
        close(accepted_preflight);
    }

    const int rejected = port == 0 ? -1 : connect_loopback(port);
    std::string rejected_response;
    if (rejected >= 0) {
        const std::string request =
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nOrigin: https://evil.example\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        static_cast<void>(send(rejected, request.data(), request.size(), 0));
        rejected_response = receive_available(rejected);
        close(rejected);
    }

    const int accepted = port == 0 ? -1 : connect_loopback(port);
    std::string accepted_response;
    if (accepted >= 0) {
        const std::string request =
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nOrigin: http://127.0.0.1:" + std::to_string(port) +
            "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        static_cast<void>(send(accepted, request.data(), request.size(), 0));
        accepted_response = receive_available(accepted);
        for (int attempt = 0; attempt < 20 &&
                               accepted_response.find("jobs_snapshot") == std::string::npos;
             ++attempt)
            accepted_response += receive_available(accepted);
        server.broadcast(R"({"type":"broadcast-smoke"})");
    }
    const std::string broadcast_response = accepted >= 0 ? receive_available(accepted) : "";

    server.stop();
    server.stop();
    const bool peer_closed = accepted >= 0 && wait_for_peer_close(accepted);
    if (accepted >= 0)
        close(accepted);
    server_thread.join();
    std::filesystem::remove_all(static_root);
    std::filesystem::remove(outside_file);

    CHECK(port != 0);
    CHECK(rejected_http_origin >= 0);
    CHECK(rejected_http_origin_response.starts_with("HTTP/1.1 403 Forbidden"));
    CHECK(rejected_http_host >= 0);
    CHECK(rejected_http_host_response.starts_with("HTTP/1.1 403 Forbidden"));
    CHECK(duplicate_content_length >= 0);
    CHECK(duplicate_content_length_response.starts_with("HTTP/1.1 400 Bad Request"));
    CHECK(transfer_encoding >= 0);
    CHECK(transfer_encoding_response.starts_with("HTTP/1.1 400 Bad Request"));
    CHECK(accepted_http >= 0);
    CHECK(accepted_http_response.starts_with("HTTP/1.1 200 OK"));
    CHECK(rejected_absolute_static >= 0);
    CHECK(rejected_absolute_static_response.starts_with("HTTP/1.1 400 Bad Request"));
    CHECK(accepted_static >= 0);
    CHECK(accepted_static_response.starts_with("HTTP/1.1 200 OK"));
    CHECK(rejected_symlink_static >= 0);
    CHECK(rejected_symlink_static_response.starts_with("HTTP/1.1 400 Bad Request"));
    CHECK(accepted_preflight >= 0);
    CHECK(accepted_preflight_response.starts_with("HTTP/1.1 204 No Content"));
    CHECK(accepted_preflight_response.find(
              "Access-Control-Allow-Origin: https://app.plywise.test") != std::string::npos);
    CHECK(accepted_preflight_response.find(
              "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS") !=
          std::string::npos);
    CHECK(rejected >= 0);
    CHECK(rejected_response.starts_with("HTTP/1.1 403 Forbidden"));
    CHECK(accepted >= 0);
    CHECK(accepted_response.starts_with("HTTP/1.1 101 Switching Protocols"));
    CHECK(broadcast_response.find("broadcast-smoke") != std::string::npos);
    CHECK(peer_closed);
    CHECK(!server_error);
    CHECK(service::HttpServer::valid_websocket_origin("http://localhost:8787"));
    CHECK(!service::HttpServer::valid_websocket_origin("http://localhost.evil:8787"));
}

TEST_CASE("hosted authority configuration rejects wildcards and public plaintext origins") {
    ChessComApiFixture fixture;
    CHECK_THROWS(service::HttpServer(
        fixture.api, fixture.jobs,
        service::ServerOptions{0, {}, "127.0.0.1", {"*.plywise.test"}, {}}));
    CHECK_THROWS(service::HttpServer(
        fixture.api, fixture.jobs,
        service::ServerOptions{0, {}, "127.0.0.1", {}, {"http://app.plywise.test"}}));
}

TEST_CASE("HTTP rate limits every connection before request parsing") {
    ChessComApiFixture fixture;
    service::ServerOptions options;
    options.port = 0;
    options.requests_per_peer_window = 2;
    options.requests_per_window = 2;
    options.request_rate_window = std::chrono::seconds(30);
    options.max_rate_limit_peers = 8;
    service::HttpServer server(fixture.api, fixture.jobs, options);
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    for (int attempt = 0; attempt < 200 && server.bound_port() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto request = [&] {
        const int socket_fd = connect_loopback(server.bound_port());
        if (socket_fd < 0)
            return std::string{};
        const std::string encoded = "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                    "Connection: close\r\n\r\n";
        static_cast<void>(send(socket_fd, encoded.data(), encoded.size(), 0));
        const std::string response = receive_available(socket_fd);
        close(socket_fd);
        return response;
    };
    const std::string first = request();
    const std::string second = request();
    const std::string third = request();

    server.stop();
    server_thread.join();

    CHECK(first.starts_with("HTTP/1.1 200 OK"));
    CHECK(second.starts_with("HTTP/1.1 200 OK"));
    CHECK(third.starts_with("HTTP/1.1 429 Too Many Requests"));
    CHECK(third.find("Retry-After: 30") != std::string::npos);
    CHECK(!server_error);
}
