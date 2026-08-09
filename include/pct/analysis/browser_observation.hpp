#pragma once

#include "pct/chess/board.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pct::analysis {

inline constexpr std::string_view browser_observation_contract_version =
    "browser-observation-v1";
inline constexpr std::string_view browser_engine_name = "Stockfish";
inline constexpr std::string_view browser_engine_version =
    "stockfish-18.0.8-lite-single";
inline constexpr std::string_view browser_engine_source = "browser";
inline constexpr std::string_view browser_engine_asset_hash =
    "js:5243fd9b276cab7dfe3ad1d43ab9ead73568fac76468c614242977a210c4a391;"
    "wasm:a8fbc05ec6920b56d7485826dcb02c5ffd2826bcbf751cf973046f237a9096f1";

inline constexpr std::size_t browser_observation_max_fen_length = 128;
inline constexpr std::size_t browser_observation_max_id_length = 128;
inline constexpr std::size_t browser_observation_max_owner_key_length = 512;
inline constexpr std::size_t browser_observation_max_pv_length = 32;
inline constexpr std::size_t browser_observation_max_sequence = 4096;
inline constexpr std::size_t browser_observation_max_ply = 20000;
inline constexpr std::size_t browser_observation_max_lines = 1;
inline constexpr std::uint64_t browser_observation_max_nodes = 100'000'000;

struct BrowserObservationLine {
    int rank{1};
    std::optional<int> centipawns;
    std::optional<int> mate;
    std::vector<std::string> moves;

    bool operator==(const BrowserObservationLine&) const = default;
};

struct BrowserEngineObservation {
    std::string contract_version;
    std::string analysis_run_id;
    std::string game_id;
    std::size_t ply{0};
    std::size_t sequence{0};
    std::string fen;
    std::string profile;
    std::string engine_name;
    std::string engine_version;
    std::string engine_source;
    std::string engine_hash;
    int depth{0};
    std::uint64_t nodes{0};
    std::uint64_t time_ms{0};
    int multipv{1};
    std::string best_move;
    std::vector<BrowserObservationLine> lines;

    bool operator==(const BrowserEngineObservation&) const = default;
};

struct BrowserObservationContext {
    std::string game_id;
    std::string analysis_run_id;
    std::string profile;
    std::string canonical_fen;
};

struct BrowserObservationRunContext {
    std::string game_id;
    std::string analysis_run_id;
    std::string profile;
};

enum class BrowserObservationDisposition { Accepted, Duplicate };

struct BrowserObservationReceipt {
    BrowserObservationDisposition disposition{BrowserObservationDisposition::Accepted};
    std::size_t sequence{0};
};

[[nodiscard]] BrowserObservationReceipt
validate_browser_observation(const BrowserObservationContext& context,
                             const BrowserEngineObservation& observation);

// The ledger is deliberately a staging guard, not a review repository. It prevents duplicate and
// replayed submissions from being accepted twice during one process lifetime. A future hosted
// repository must persist the same run/sequence invariants transactionally before classification.
class BrowserObservationLedger final {
  public:
    explicit BrowserObservationLedger(std::size_t max_runs = 256,
                                      std::size_t max_observations_per_run = 512);

    // Register the run before accepting any browser observations. The run binds
    // ownership, game, and profile; each submitted position still supplies its own canonical FEN.
    void begin(std::string_view owner_key, const BrowserObservationRunContext& context);

    [[nodiscard]] BrowserObservationReceipt submit(
        std::string_view owner_key, const BrowserObservationContext& context,
        const BrowserEngineObservation& observation);

    [[nodiscard]] std::size_t run_count() const;
    [[nodiscard]] std::size_t observation_count() const;

  private:
    struct RunState {
        BrowserObservationRunContext context;
        std::size_t next_sequence{0};
        std::map<std::pair<std::size_t, std::size_t>, BrowserEngineObservation> observations;
    };

    std::size_t max_runs_;
    std::size_t max_observations_per_run_;
    mutable std::mutex mutex_;
    std::map<std::string, RunState> runs_;
};

} // namespace pct::analysis
