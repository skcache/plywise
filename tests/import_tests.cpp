#include "test.hpp"

#include "pct/import/import_service.hpp"

using namespace pct::import;

namespace {

constexpr std::string_view sample_pgn = R"pgn([Event "Imported"]
[Site "https://www.chess.com/game/live/123"]
[Date "2026.06.17"]
[White "Alex"]
[Black "Morgan"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0)pgn";

} // namespace

TEST_CASE("Chess.com URL validation and normalization") {
    const auto url = ImportService::parse_chesscom_url(
        "https://chess.com/game/live/123?player=Alex&year=2026&month=06");
    CHECK_EQ(url.game_id, "123");
    CHECK_EQ(url.canonical, "https://www.chess.com/game/live/123");
    CHECK_EQ(url.player, "Alex");
    CHECK_THROWS(ImportService::parse_chesscom_url("http://chess.com/game/live/123"));
    CHECK_THROWS(ImportService::parse_chesscom_url("https://evil.example/game/live/123"));
}

TEST_CASE("manual PGN import reconstructs the game") {
    ImportService service;
    const ImportedGame imported = service.from_pgn(sample_pgn);
    CHECK_EQ(imported.game.plies.size(), 4ULL);
    CHECK(imported.method == ImportMethod::ManualPgn);
}

TEST_CASE("page import extracts JSON-escaped PGN") {
    const std::string escaped =
        "<script>{\"pgn\":\"[Event \\\"Imported\\\"]\\n[Site "
        "\\\"https://www.chess.com/game/live/123\\\"]\\n[Date \\\"2026.06.17\\\"]\\n[White "
        "\\\"Alex\\\"]\\n[Black \\\"Morgan\\\"]\\n[Result \\\"1-0\\\"]\\n\\n1. e4 e5 2. Nf3 Nc6 "
        "1-0\"}</script>";
    ImportService service([&](const std::string& url) {
        CHECK_EQ(url, "https://www.chess.com/game/live/123");
        return escaped;
    });
    const ImportedGame imported = service.from_url("https://www.chess.com/game/live/123");
    CHECK_EQ(imported.game.tag("White"), "Alex");
    CHECK(imported.method == ImportMethod::PublicPage);
}

TEST_CASE("archive import selects the matching game URL") {
    std::string response = "{\"games\":[{\"url\":\"https://www.chess.com/game/live/123\",\"pgn\":";
    response +=
        "\"[Event \\\"Imported\\\"]\\n[Site \\\"https://www.chess.com/game/live/123\\\"]\\n[Date "
        "\\\"2026.06.17\\\"]\\n[White \\\"Alex\\\"]\\n[Black \\\"Morgan\\\"]\\n[Result "
        "\\\"1-0\\\"]\\n\\n1. e4 e5 2. Nf3 Nc6 1-0\"}]}";
    ImportService service([&](const std::string& url) {
        CHECK(url.find("api.chess.com/pub/player/Alex/games/2026/06") != std::string::npos);
        return response;
    });
    const ImportedGame imported =
        service.from_url("https://chess.com/game/live/123?player=Alex&year=2026&month=06");
    CHECK(imported.method == ImportMethod::PublicApi);
}

TEST_CASE("plain game links discover players before the page fallback") {
    const std::string archive =
        R"json({"archives":["https://api.chess.com/pub/player/Alex/games/2026/06"]})json";
    const std::string month =
        R"json({"games":[{"url":"https://www.chess.com/game/live/123","pgn":"[Event \"Imported\"]\n[Site \"Chess.com\"]\n[Date \"2026.06.17\"]\n[White \"Alex\"]\n[Black \"Morgan\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 1-0"}]})json";
    ImportService service([&](const HttpRequest& request) {
        if (request.url == "https://www.chess.com/game/live/123")
            return HttpResponse{200, {}, request.url,
                                "<title>Chess: Alex vs Morgan - Chess.com</title>"};
        if (request.url == "https://api.chess.com/pub/player/Alex/games/archives")
            return HttpResponse{200, {}, request.url, archive};
        if (request.url == "https://api.chess.com/pub/player/Alex/games/2026/06")
            return HttpResponse{200, {}, request.url, month};
        throw std::runtime_error("unexpected Chess.com request: " + request.url);
    });

    const ImportedGame imported = service.from_url("https://www.chess.com/game/live/123");
    CHECK(imported.method == ImportMethod::PublicApi);
    CHECK_EQ(imported.game.tag("White"), "Alex");
    CHECK_EQ(imported.game.tag("Black"), "Morgan");
}
