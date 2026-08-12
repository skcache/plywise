#pragma once

#include "pct/chess/board.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pct::chess {

struct Ply {
    Move move;
    std::string san;
    std::string fen_before;
    std::string fen_after;
    std::optional<std::int64_t> clock_ms;
    std::optional<std::int64_t> elapsed_ms;
};

struct Game {
    std::map<std::string, std::string> tags;
    std::string source_pgn;
    std::vector<Ply> plies;
    std::string identity;

    [[nodiscard]] std::string tag(std::string_view key, std::string_view fallback = {}) const;
};

[[nodiscard]] Game parse_pgn(std::string_view pgn);
[[nodiscard]] std::string normalized_game_identity(const Game& game);
// Stable, annotation-free PGN for globally shared chess truth. Exact imported
// PGN and provenance remain owner-local in hosted persistence.
[[nodiscard]] std::string canonical_pgn(const Game& game);

} // namespace pct::chess
