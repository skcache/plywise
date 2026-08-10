#include "test.hpp"

#include "pct/app/ingest_manager.hpp"
#include "pct/service/http_server.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
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

    ChessComApiFixture()
        : path(std::filesystem::temp_directory_path() /
               ("pct-chesscom-api-" + std::to_string(::getpid()) + ".log")),
          log((std::filesystem::remove(path), path)), repository(log),
          transport([](const import::HttpRequest& request) {
              if (request.url.ends_with("/archives"))
                  return import::HttpResponse{200, {}, request.url, R"({"archives":[]})"};
              return import::HttpResponse{200, {}, request.url, "<html>no pgn</html>"};
          }), importer(transport), analyzer(engine, cache, analysis::AnalyzerOptions{2, 3, 80, 2, 1}),
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
    service::HttpServer server(
        fixture.api, fixture.jobs,
        service::ServerOptions{0, {}, "127.0.0.1", {"api.plywise.test"},
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
