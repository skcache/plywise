#pragma once

#include "pct/chess/pgn.hpp"
#include "pct/import/chesscom_archive_client.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace pct::import {

enum class ImportMethod { PublicApi, PublicPage, ManualPgn };

struct ImportedGame {
    chess::Game game;
    std::string source_url;
    std::string pgn;
    ImportMethod method{ImportMethod::ManualPgn};
};

struct ChessComUrl {
    std::string canonical;
    std::string game_id;
    std::string player;
    std::string year;
    std::string month;
    std::string game_type;
};

struct ChessComGamePlayers {
    std::string white;
    std::string black;
};

using HttpGet = std::function<std::string(const std::string&)>;

class ImportService {
  public:
    explicit ImportService(HttpGet get = {});
    explicit ImportService(HttpTransport transport, RetrySleeper sleeper = {});

    [[nodiscard]] ImportedGame from_url(std::string_view url,
                                        CancellationToken cancellation = {}) const;
    [[nodiscard]] ImportedGame from_pgn(std::string_view pgn,
                                        std::string_view source_url = {}) const;
    [[nodiscard]] ChessComGamePlayers
    discover_players(std::string_view url, CancellationToken cancellation = {}) const;

    [[nodiscard]] static ChessComUrl parse_chesscom_url(std::string_view url);
    [[nodiscard]] static std::string extract_pgn(std::string_view response,
                                                 std::string_view target_game_id = {});
    [[nodiscard]] static ChessComGamePlayers extract_players(std::string_view response);

  private:
    HttpTransport transport_;
    RetrySleeper sleeper_;
};

[[nodiscard]] std::string curl_get(const std::string& url);

} // namespace pct::import
