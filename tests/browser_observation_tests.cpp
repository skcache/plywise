#include "test.hpp"

#include "pct/analysis/browser_observation.hpp"

#include <string>

using namespace pct;

namespace {

constexpr std::string_view initial_fen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

analysis::BrowserEngineObservation valid_observation(std::string fen, std::string best_move,
                                                     std::size_t ply, std::size_t sequence,
                                                     std::vector<std::string> moves = {}) {
    if (moves.empty())
        moves.push_back(best_move);
    return analysis::BrowserEngineObservation{
        std::string(analysis::browser_observation_contract_version),
        "run-1",
        "game-1",
        ply,
        sequence,
        std::move(fen),
        "quick",
        std::string(analysis::browser_engine_name),
        std::string(analysis::browser_engine_version),
        std::string(analysis::browser_engine_source),
        std::string(analysis::browser_engine_asset_hash),
        10,
        12'345,
        120,
        1,
        std::move(best_move),
        {analysis::BrowserObservationLine{1, 34, std::nullopt, std::move(moves)}},
    };
}
analysis::BrowserObservationContext position_context(std::string fen) {
    return analysis::BrowserObservationContext{
        "game-1", "run-1", "quick", std::move(fen)};
}

} // namespace

TEST_CASE("browser observations validate the pinned engine and legal principal variation") {
    auto observation = valid_observation(std::string(initial_fen), "e2e4", 0, 0,
                                         {"e2e4", "e7e5"});
    const auto result = analysis::validate_browser_observation(
        position_context(std::string(initial_fen)), observation);
    CHECK(result.disposition == analysis::BrowserObservationDisposition::Accepted);
    CHECK_EQ(result.sequence, 0ULL);

    observation.engine_hash = "different-build";
    CHECK_THROWS(analysis::validate_browser_observation(
        position_context(std::string(initial_fen)), observation));
}

TEST_CASE("browser observation validation rejects forged positions and bounded payloads") {
    auto observation = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    auto context = position_context(std::string(initial_fen));

    observation.lines.front().moves = {"e2e5"};
    CHECK_THROWS(analysis::validate_browser_observation(context, observation));

    observation = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    observation.nodes = analysis::browser_observation_max_nodes + 1;
    CHECK_THROWS(analysis::validate_browser_observation(context, observation));

    observation = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    observation.lines.front().centipawns = 1;
    observation.lines.front().mate = 2;
    CHECK_THROWS(analysis::validate_browser_observation(context, observation));

    observation = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    context.canonical_fen = "8/8/8/8/8/8/8/8 w - - 0 1";
    CHECK_THROWS(analysis::validate_browser_observation(context, observation));
}

TEST_CASE("browser observation ledger binds owner and run and handles replay safely") {
    analysis::BrowserObservationLedger ledger;
    const analysis::BrowserObservationRunContext run{"game-1", "run-1", "quick"};
    ledger.begin("guest:one", run);
    ledger.begin("guest:one", run);
    CHECK_EQ(ledger.run_count(), 1ULL);

    const auto first = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    const auto first_context = position_context(std::string(initial_fen));
    const auto accepted = ledger.submit("guest:one", first_context, first);
    CHECK(accepted.disposition == analysis::BrowserObservationDisposition::Accepted);
    const auto duplicate = ledger.submit("guest:one", first_context, first);
    CHECK(duplicate.disposition == analysis::BrowserObservationDisposition::Duplicate);

    auto second = valid_observation(
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1", "e7e5", 1, 1);
    const auto second_context = position_context(second.fen);
    const auto second_receipt = ledger.submit("guest:one", second_context, second);
    CHECK(second_receipt.disposition == analysis::BrowserObservationDisposition::Accepted);
    CHECK_EQ(ledger.observation_count(), 2ULL);

    auto conflicting = first;
    conflicting.nodes += 1;
    CHECK_THROWS(ledger.submit("guest:one", first_context, conflicting));

    auto gap = valid_observation(second.fen, "e7e5", 1, 3);
    CHECK_THROWS(ledger.submit("guest:one", second_context, gap));
    CHECK_THROWS(ledger.submit("guest:missing", first_context, first));
    CHECK_THROWS(ledger.begin("guest:one", analysis::BrowserObservationRunContext{
                                   "game-1", "run-1", "balanced"}));

    ledger.begin("guest:two", run);
    CHECK_EQ(ledger.run_count(), 2ULL);
}
