#include "test.hpp"

#include "pct/common/json.hpp"
#include "pct/import/chesscom_archive_client.hpp"
#include "pct/import/import_service.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace pct;
using namespace pct::import;

namespace {

constexpr std::string_view target_id = "171626462440";
constexpr std::string_view target_pgn = R"pgn([Event "Imported"]
[Site "https://www.chess.com/game/live/171626462440"]
[Date "2026.06.17"]
[White "Alex"]
[Black "Morgan"]
[Result "1-0"]

1. e4 e5 1-0)pgn";

std::string monthly_json(std::string_view id = target_id,
                         std::string_view time_class = "brand-new-class") {
    return json::dump(json::Value::Object{
        {"games",
         json::Value::Array{json::Value::Object{
             {"url", "https://www.chess.com/game/live/" + std::string(id)},
             {"pgn", std::string(target_pgn)},
             {"time_class", std::string(time_class)},
         }}},
    });
}

ChessComClientError capture_client_error(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const ChessComClientError& error) {
        return error;
    }
    throw std::runtime_error("expected ChessComClientError");
}

} // namespace

TEST_CASE("Chess.com exact live daily and analysis URL shapes") {
    const std::vector<std::string> urls = {
        "https://www.chess.com/game/live/171626462440",
        "https://chess.com/game/daily/171626462440",
        "https://www.chess.com/analysis/game/live/171626462440",
        "https://www.chess.com/analysis/game/live/171626462440/analysis?tab=analysis",
        "https://www.chess.com/analysis/game/daily/171626462440/analysis",
    };
    for (const std::string& url : urls)
        CHECK_EQ(ImportService::parse_chesscom_url(url).game_id, target_id);

    CHECK_THROWS(ImportService::parse_chesscom_url(
        "https://www.chess.com/game/live/171626462440/analysis"));
    CHECK_THROWS(ImportService::parse_chesscom_url(
        "https://www.chess.com/foo/game/live/171626462440"));
}

TEST_CASE("finished links fall back to the exact Chess.com callback move list") {
    const std::string callback = json::dump(json::Value::Object{
        {"game", json::Value::Object{
                      {"id", static_cast<double>(171626462440ULL)},
                      {"moveList", "mC0Kgv5Q"},
                      {"plyCount", 4},
                      {"pgnHeaders", json::Value::Object{
                                           {"Event", "Live Chess"},
                                           {"Site", "Chess.com"},
                                           {"Date", "2026.06.17"},
                                           {"White", "Alex"},
                                           {"Black", "Morgan"},
                                           {"Result", "1-0"},
                                       }},
                  }},
    });
    int requests = 0;
    ImportService service(HttpTransport{[&](const HttpRequest& request) {
        ++requests;
        if (request.url == "https://api.chess.com/pub/player/Alex/games/2026/06")
            return HttpResponse{200, {}, request.url, R"json({"games":[]})json"};
        if (request.url == "https://www.chess.com/callback/live/game/171626462440")
            return HttpResponse{200, {}, request.url, callback};
        return HttpResponse{200, {}, request.url, "<html>no game here</html>"};
    }});

    const ImportedGame imported = service.from_url(
        "https://www.chess.com/game/live/171626462440?player=Alex&year=2026&month=06");
    CHECK(imported.method == ImportMethod::PublicApi);
    CHECK_EQ(imported.game.tag("White"), "Alex");
    CHECK_EQ(imported.game.tag("Black"), "Morgan");
    CHECK_EQ(imported.game.plies.size(), 4ULL);
    CHECK_EQ(imported.game.plies[0].san, "e4");
    CHECK_EQ(imported.game.plies[1].san, "e5");
    CHECK_EQ(imported.game.plies[2].san, "Nf3");
    CHECK_EQ(imported.game.plies[3].san, "Nc6");
    CHECK_EQ(requests, 2);
}

TEST_CASE("Chess.com URL and archive validation rejects authority and path attacks") {
    const std::vector<std::string> attacks = {
        "https://www.chess.com@evil.example/game/live/171626462440",
        "https://evil.example/game/live/171626462440",
        "https://www.chess.com:443/game/live/171626462440",
        "https://www.chess.com/game/live/%31%37",
        "https://www.chess.com/game/live/171626462440#https://evil.example",
        "https://www.chess.com\\@evil.example/game/live/171626462440",
    };
    for (const std::string& url : attacks)
        CHECK_THROWS(ImportService::parse_chesscom_url(url));

    CHECK_THROWS(ChessComArchiveClient::archive_index_url("a"));
    CHECK_THROWS(ChessComArchiveClient::monthly_archive_url("Alex", "2026", "13"));
    CHECK_THROWS(ChessComArchiveClient::validate_archive_url(
        "https://api.chess.com.evil/pub/player/Alex/games/2026/06", "Alex"));
    CHECK_THROWS(ChessComArchiveClient::validate_archive_url(
        "https://api.chess.com/pub/player/Morgan/games/2026/06", "Alex"));
    CHECK_THROWS(ChessComArchiveClient::validate_archive_url(
        "https://api.chess.com/pub/player/Alex/games/2026/06?next=evil", "Alex"));

    ChessComArchiveClient redirected([](const HttpRequest&) {
        return HttpResponse{200, {}, "https://evil.example/pub/player/Alex/games/2026/06",
                            R"json({"games":[]})json"};
    });
    const ChessComClientError redirect_error = capture_client_error([&] {
        static_cast<void>(redirected.fetch_month("Alex", "2026", "06"));
    });
    CHECK(redirect_error.failure() == ChessComFailure::InvalidResponse);
}

TEST_CASE("monthly archive structurally parses and preserves unknown time class") {
    ChessComArchiveClient client([](const HttpRequest& request) {
        CHECK_EQ(request.url, "https://api.chess.com/pub/player/Alex/games/2026/06");
        return HttpResponse{200, {{"etag", "v1"}}, request.url, monthly_json()};
    });
    const ChessComMonthlyArchive archive = client.fetch_month("Alex", "2026", "06");
    CHECK_EQ(archive.games.size(), 1ULL);
    CHECK_EQ(archive.games.front().url,
             "https://www.chess.com/game/live/171626462440");
    CHECK_EQ(archive.games.front().time_class, "brand-new-class");
    CHECK_EQ(archive.etag, "v1");
}

TEST_CASE("archive discovery validates URLs and finds exact target") {
    int requests = 0;
    ChessComArchiveClient client([&](const HttpRequest& request) {
        ++requests;
        if (request.url.ends_with("/archives")) {
            return HttpResponse{
                200,
                {},
                request.url,
                R"json({"archives":["https://api.chess.com/pub/player/Alex/games/2026/05","https://api.chess.com/pub/player/Alex/games/2026/06"]})json"};
        }
        CHECK(request.url.ends_with("/2026/06"));
        return HttpResponse{200, {}, request.url, monthly_json()};
    });
    const ChessComArchiveGame game = client.find_game("Alex", target_id);
    CHECK_EQ(game.url, "https://www.chess.com/game/live/171626462440");
    CHECK_EQ(requests, 2);
}

TEST_CASE("archive miss never returns an unrelated PGN") {
    ChessComArchiveClient client([](const HttpRequest& request) {
        return HttpResponse{200, {}, request.url, monthly_json("999")};
    });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.find_game("Alex", target_id, "2026", "06"));
    });
    CHECK(error.failure() == ChessComFailure::NotFound);
    CHECK(error.code() == ErrorCode::NotFound);

    CHECK_THROWS(ImportService::extract_pgn(monthly_json("999"), target_id));
}

TEST_CASE("malformed and schema-invalid archive JSON are typed invalid responses") {
    for (const std::string& body : {std::string("{not-json"),
                                    std::string(R"json({"games":{}})json"),
                                    std::string(R"json({"games":[{"url":7,"pgn":"x"}]})json")}) {
        ChessComArchiveClient client([&](const HttpRequest& request) {
            return HttpResponse{200, {}, request.url, body};
        });
        const ChessComClientError error = capture_client_error([&] {
            static_cast<void>(client.fetch_month("Alex", "2026", "06"));
        });
        CHECK(error.failure() == ChessComFailure::InvalidResponse);
        CHECK(error.code() == ErrorCode::ParseError);
    }
}

TEST_CASE("304 is a typed result and sends conditional headers") {
    ChessComArchiveClient client([](const HttpRequest& request) {
        CHECK_EQ(request.headers.at("If-None-Match"), "etag-1");
        CHECK_EQ(request.headers.at("If-Modified-Since"), "Wed, 17 Jun 2026 00:00:00 GMT");
        return HttpResponse{304, {{"etag", "etag-1"}}, request.url, {}};
    });
    const ChessComMonthlyArchive result = client.fetch_month(
        "Alex", "2026", "06", {"etag-1", "Wed, 17 Jun 2026 00:00:00 GMT"});
    CHECK(result.not_modified);
    CHECK(result.games.empty());
}

TEST_CASE("404 and 410 have distinct typed failures") {
    for (const long status : {404L, 410L}) {
        ChessComArchiveClient client([=](const HttpRequest& request) {
            return HttpResponse{status, {}, request.url, {}};
        });
        const ChessComClientError error = capture_client_error([&] {
            static_cast<void>(client.fetch_month("Alex", "2026", "06"));
        });
        CHECK(error.failure() ==
              (status == 404 ? ChessComFailure::NotFound : ChessComFailure::Gone));
        CHECK_EQ(error.status(), status);
    }
}

TEST_CASE("429 Retry-After retries serially and is bounded") {
    int requests = 0;
    std::vector<std::chrono::milliseconds> sleeps;
    ChessComArchiveClient client(
        [&](const HttpRequest& request) {
            ++requests;
            if (requests < 3)
                return HttpResponse{429, {{"retry-after", "2"}}, request.url, {}};
            return HttpResponse{200, {}, request.url, monthly_json()};
        },
        [&](std::chrono::milliseconds duration, CancellationToken) {
            sleeps.push_back(duration);
        });
    CHECK_EQ(client.fetch_month("Alex", "2026", "06").games.size(), 1ULL);
    CHECK_EQ(requests, 3);
    CHECK_EQ(sleeps.size(), 2ULL);
    CHECK_EQ(sleeps.front().count(), 2000);

    requests = 0;
    ChessComArchiveClient bounded(
        [&](const HttpRequest& request) {
            ++requests;
            return HttpResponse{503, {}, request.url, {}};
        },
        [](std::chrono::milliseconds, CancellationToken) {});
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(bounded.fetch_month("Alex", "2026", "06"));
    });
    CHECK(error.failure() == ChessComFailure::ServerError);
    CHECK_EQ(requests, 3);
}

TEST_CASE("cancellation is observed before transport") {
    CancellationSource source;
    source.request_stop();
    int requests = 0;
    ChessComArchiveClient client([&](const HttpRequest&) {
        ++requests;
        return HttpResponse{};
    });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.fetch_month("Alex", "2026", "06", {}, source.get_token()));
    });
    CHECK(error.failure() == ChessComFailure::Cancelled);
    CHECK_EQ(requests, 0);
}

TEST_CASE("timeouts are typed and use the bounded retry policy") {
    int requests = 0;
    int sleeps = 0;
    ChessComArchiveClient client(
        [&](const HttpRequest&) -> HttpResponse {
            ++requests;
            throw ChessComClientError(ChessComFailure::Timeout, ErrorCode::Timeout,
                                      "simulated timeout");
        },
        [&](std::chrono::milliseconds, CancellationToken) { ++sleeps; });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.fetch_month("Alex", "2026", "06"));
    });
    CHECK(error.failure() == ChessComFailure::Timeout);
    CHECK_EQ(requests, 3);
    CHECK_EQ(sleeps, 2);
}

TEST_CASE("injected transport cannot bypass response body bounds") {
    ChessComArchiveClient client([](const HttpRequest& request) {
        return HttpResponse{200, {}, request.url,
                            std::string(chesscom_max_body_size + 1U, 'x')};
    });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.fetch_month("Alex", "2026", "06"));
    });
    CHECK(error.failure() == ChessComFailure::BodyTooLarge);
}

TEST_CASE("public fallback failure does not replace archive failure") {
    int requests = 0;
    ImportService service(
        [&](const HttpRequest& request) {
            ++requests;
            if (request.url.find("api.chess.com") != std::string::npos)
                return HttpResponse{503, {}, request.url, {}};
            return HttpResponse{200, {}, request.url, "<html>no game here</html>"};
        },
        [](std::chrono::milliseconds, CancellationToken) {});
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(service.from_url(
            "https://www.chess.com/game/live/171626462440?player=Alex&year=2026&month=06"));
    });
    CHECK(error.failure() == ChessComFailure::ServerError);
    CHECK_EQ(requests, 5);
}

TEST_CASE("curl transport returns 304 and follows only explicit redirect statuses") {
    int hops = 0;
    const HttpRequest request{"https://api.chess.com/pub/player/Alex/games/2026/06", {},
                              chesscom_max_body_size, CancellationToken{}};
    const HttpResponse not_modified = curl_http_get_with_hops(
        request, [&](const HttpRequest&, std::string_view current) {
            ++hops;
            return HttpResponse{304, {}, std::string(current), {}};
        });
    CHECK_EQ(not_modified.status, 304L);
    CHECK_EQ(hops, 1);

    hops = 0;
    const HttpResponse use_proxy = curl_http_get_with_hops(
        request, [&](const HttpRequest&, std::string_view current) {
            ++hops;
            return HttpResponse{305, {{"location", request.url}}, std::string(current), {}};
        });
    CHECK_EQ(use_proxy.status, 305L);
    CHECK_EQ(hops, 1);

    for (const long status : {301L, 302L, 303L, 307L, 308L}) {
        hops = 0;
        const HttpResponse redirected = curl_http_get_with_hops(
            request, [&](const HttpRequest&, std::string_view current) {
                ++hops;
                if (hops == 1)
                    return HttpResponse{status, {{"location", request.url}},
                                        std::string(current), {}};
                return HttpResponse{200, {}, std::string(current),
                                    R"json({"games":[]})json"};
            });
        CHECK_EQ(redirected.status, 200L);
        CHECK_EQ(hops, 2);
    }
}

TEST_CASE("PubAPI effective URLs and redirects cannot swap account month or path") {
    const std::vector<std::string> swaps = {
        "https://api.chess.com/pub/player/Morgan/games/2026/06",
        "https://api.chess.com/pub/player/Alex/games/2026/05",
        "https://api.chess.com/pub/player/Alex/stats",
    };
    for (const std::string& effective : swaps) {
        ChessComArchiveClient client([&](const HttpRequest&) {
            return HttpResponse{200, {}, effective, R"json({"games":[]})json"};
        });
        const ChessComClientError error = capture_client_error([&] {
            static_cast<void>(client.fetch_month("Alex", "2026", "06"));
        });
        CHECK(error.failure() == ChessComFailure::InvalidResponse);
    }

    int hops = 0;
    const HttpRequest request{"https://api.chess.com/pub/player/Alex/games/2026/06", {},
                              chesscom_max_body_size, CancellationToken{}};
    const ChessComClientError redirect_error = capture_client_error([&] {
        static_cast<void>(curl_http_get_with_hops(
            request, [&](const HttpRequest&, std::string_view current) {
                ++hops;
                return HttpResponse{
                    302,
                    {{"location", "https://api.chess.com/pub/player/Alex/games/2026/05"}},
                    std::string(current), {}};
            }));
    });
    CHECK(redirect_error.failure() == ChessComFailure::InvalidResponse);
    CHECK_EQ(hops, 1);
}

TEST_CASE("archive discovery scans at most four unique newest months") {
    std::vector<std::string> requests;
    ChessComArchiveClient client([&](const HttpRequest& request) {
        requests.push_back(request.url);
        if (request.url.ends_with("/archives")) {
            return HttpResponse{
                200,
                {},
                request.url,
                R"json({"archives":["https://api.chess.com/pub/player/Alex/games/2026/01","https://api.chess.com/pub/player/Alex/games/2026/02","https://api.chess.com/pub/player/Alex/games/2026/03","https://api.chess.com/pub/player/Alex/games/2026/04","https://api.chess.com/pub/player/Alex/games/2026/05","https://api.chess.com/pub/player/Alex/games/2026/06","https://api.chess.com/pub/player/Alex/games/2026/06"]})json"};
        }
        return HttpResponse{200, {}, request.url, R"json({"games":[]})json"};
    });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.find_game("Alex", target_id));
    });
    CHECK(error.failure() == ChessComFailure::NotFound);
    CHECK_EQ(requests.size(), 5ULL);
    CHECK(requests[1].ends_with("/2026/06"));
    CHECK(requests[2].ends_with("/2026/05"));
    CHECK(requests[3].ends_with("/2026/04"));
    CHECK(requests[4].ends_with("/2026/03"));
}

TEST_CASE("selected archive PGN Site and Link must both agree with target") {
    const std::string mismatched_pgn = R"pgn([Event "Imported"]
[Site "https://www.chess.com/game/live/171626462440"]
[Link "https://www.chess.com/game/live/999"]
[Date "2026.06.17"]
[White "Alex"]
[Black "Morgan"]
[Result "1-0"]

1. e4 e5 1-0)pgn";
    const std::string body = json::dump(json::Value::Object{
        {"games",
         json::Value::Array{json::Value::Object{
             {"url", "https://www.chess.com/game/live/171626462440"},
             {"pgn", mismatched_pgn},
         }}},
    });
    ChessComArchiveClient client([&](const HttpRequest& request) {
        return HttpResponse{200, {}, request.url, body};
    });
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(client.find_game("Alex", target_id, "2026", "06"));
    });
    CHECK(error.failure() == ChessComFailure::InvalidResponse);
}

TEST_CASE("public game page player discovery accepts current Open Graph title") {
    const auto players = ImportService::extract_players(
        "<meta property=\"og:title\" content=\"Chess: superking116 vs CartaaaaZ\" />");
    CHECK_EQ(players.white, "superking116");
    CHECK_EQ(players.black, "CartaaaaZ");
    CHECK_THROWS(ImportService::extract_players("<title>Chess.com</title>"));
}

TEST_CASE("exact archive URL accepts Chess.com's generic PGN Site tag") {
    const std::string current_archive_pgn = R"pgn([Event "Live Chess"]
[Site "Chess.com"]
[Date "2026.07.15"]
[White "Alex"]
[Black "Morgan"]
[Result "1-0"]

1. e4 e5 1-0)pgn";
    const std::string body = json::dump(json::Value::Object{
        {"games",
         json::Value::Array{json::Value::Object{
             {"url", "https://www.chess.com/game/live/171626462440"},
             {"pgn", current_archive_pgn},
         }}},
    });
    ChessComArchiveClient client([&](const HttpRequest& request) {
        return HttpResponse{200, {}, request.url, body};
    });

    const ChessComArchiveGame game =
        client.find_game("Alex", target_id, "2026", "07");
    CHECK_EQ(game.url, "https://www.chess.com/game/live/171626462440");
    CHECK_EQ(game.pgn, current_archive_pgn);
}

TEST_CASE("cancellation after archive failure is never hidden by archive failure") {
    CancellationSource source;
    int api_requests = 0;
    int page_requests = 0;
    ImportService service(
        [&](const HttpRequest& request) {
            if (request.url.find("api.chess.com") != std::string::npos) {
                ++api_requests;
                if (api_requests == 3)
                    source.request_stop();
                return HttpResponse{503, {}, request.url, {}};
            }
            ++page_requests;
            return HttpResponse{200, {}, request.url, "<html>unused</html>"};
        },
        [](std::chrono::milliseconds, CancellationToken) {});
    const ChessComClientError error = capture_client_error([&] {
        static_cast<void>(service.from_url(
            "https://www.chess.com/game/live/171626462440?player=Alex&year=2026&month=06",
            source.get_token()));
    });
    CHECK(error.failure() == ChessComFailure::Cancelled);
    CHECK_EQ(api_requests, 3);
    CHECK_EQ(page_requests, 0);
}

TEST_CASE("unexpected public fallback exception is not hidden by archive failure") {
    ImportService service(
        [](const HttpRequest& request) -> HttpResponse {
            if (request.url.find("api.chess.com") != std::string::npos)
                return HttpResponse{503, {}, request.url, {}};
            throw std::runtime_error("unexpected fallback failure");
        },
        [](std::chrono::milliseconds, CancellationToken) {});
    try {
        static_cast<void>(service.from_url(
            "https://www.chess.com/game/live/171626462440?player=Alex&year=2026&month=06"));
        CHECK(false);
    } catch (const std::runtime_error& error) {
        CHECK_EQ(std::string(error.what()), "unexpected fallback failure");
    }
}
