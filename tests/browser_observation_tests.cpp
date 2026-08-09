#include "test.hpp"

#include "pct/analysis/browser_observation.hpp"

#include <map>
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

TEST_CASE("browser observations round trip through the hosted staging JSON contract") {
    auto original = valid_observation(std::string(initial_fen), "e2e4", 0, 0,
                                      {"e2e4", "e7e5"});
    original.lines.front().centipawns.reset();
    original.lines.front().mate = 3;
    const auto encoded = analysis::browser_observation_to_json(original);
    const auto restored = analysis::browser_observation_from_json(encoded);
    CHECK(restored == original);
}

TEST_CASE("browser observations preserve terminal positions without inventing a move") {
    const auto game = chess::parse_pgn(R"pgn(
[White "A"]
[Black "B"]
[Result "0-1"]

1. f3 e5 2. g4 Qh4# 0-1
)pgn");
    auto observation = valid_observation(game.plies.back().fen_after, "0000", 3, 0);
    observation.lines.front().moves.clear();
    observation.lines.front().centipawns.reset();
    observation.lines.front().mate = 1;
    const auto result = analysis::validate_browser_observation(
        position_context(observation.fen), observation);
    CHECK(result.disposition == analysis::BrowserObservationDisposition::Accepted);
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

TEST_CASE("browser review assembly shares the server classification contract") {
    const chess::Game game = chess::parse_pgn(R"pgn(
[White "A"]
[Black "B"]
[Result "1-0"]

1. e4 e5 1-0
)pgn");
    const auto observation_for = [&](std::size_t ply, std::size_t sequence, bool after,
                                     std::string best_move) {
        const auto& canonical = game.plies[ply];
        const std::string fen = after ? canonical.fen_after : canonical.fen_before;
        return analysis::BrowserEngineObservation{
            std::string(analysis::browser_observation_contract_version),
            "browser-run-1",
            game.identity,
            ply,
            sequence,
            fen,
            "quick",
            std::string(analysis::browser_engine_name),
            std::string(analysis::browser_engine_version),
            std::string(analysis::browser_engine_source),
            std::string(analysis::browser_engine_asset_hash),
            10,
            1'000,
            100,
            1,
            best_move,
            {analysis::BrowserObservationLine{1, 24, std::nullopt, {best_move}}},
        };
    };
    std::vector<analysis::BrowserEngineObservation> observations{
        observation_for(0, 0, false, "e2e4"),
        observation_for(0, 1, true, "e7e5"),
        observation_for(1, 2, false, "e7e5"),
        observation_for(1, 3, true, "g1f3"),
    };

    std::map<std::string, engine::AnalysisResult> server_results;
    for (const auto& observation : observations) {
        const auto& line = observation.lines.front();
        server_results.insert_or_assign(
            observation.fen,
            engine::AnalysisResult{
                {engine::PrincipalVariation{observation.multipv, observation.depth,
                                            line.centipawns, line.mate, observation.nodes,
                                            observation.time_ms, line.moves}},
                observation.best_move,
                {},
            });
    }
    class ObservationEngine final : public engine::AnalysisEngine {
      public:
        explicit ObservationEngine(std::map<std::string, engine::AnalysisResult> results)
            : results_(std::move(results)) {}

        engine::AnalysisResult analyze(const engine::AnalysisRequest& request,
                                       CancellationToken) override {
            const auto found = results_.find(request.fen);
            CHECK(found != results_.end());
            return found->second;
        }

      private:
        std::map<std::string, engine::AnalysisResult> results_;
    } observation_engine(std::move(server_results));
    analysis::AnalysisCache cache;
    analysis::Analyzer analyzer(observation_engine, cache);
    const auto server = analyzer.analyze_shallow(game);
    const auto browser = analysis::assemble_browser_review(game, observations);

    CHECK_EQ(browser.moves.size(), server.moves.size());
    CHECK_EQ(browser.opening, server.opening);
    CHECK_EQ(browser.eco, server.eco);
    CHECK_EQ(browser.accuracy_sample_size, game.plies.size());
    CHECK_EQ(browser.requested_engine_profile, "quick");
    CHECK_EQ(browser.actual_engine_profile, "quick");
    CHECK_EQ(browser.engine_name, std::string(analysis::browser_engine_name));
    CHECK_EQ(browser.engine_source, std::string(analysis::browser_engine_source));
    CHECK_EQ(browser.engine_hash, std::string(analysis::browser_engine_asset_hash));
    for (std::size_t index = 0; index < browser.moves.size(); ++index) {
        CHECK_EQ(browser.moves[index].played_uci, server.moves[index].played_uci);
        CHECK_EQ(browser.moves[index].best_uci, server.moves[index].best_uci);
        CHECK_EQ(browser.moves[index].fen_before, server.moves[index].fen_before);
        CHECK_EQ(browser.moves[index].fen_after, server.moves[index].fen_after);
        CHECK(browser.moves[index].quality == server.moves[index].quality);
        CHECK(browser.moves[index].tactical_tags == server.moves[index].tactical_tags);
        CHECK(browser.moves[index].classification_state == analysis::ClassificationState::Final);
        CHECK_EQ(browser.moves[index].engine_version,
                 std::string(analysis::browser_engine_version));
    }
}

TEST_CASE("browser observation finalization requires a complete before-and-after run") {
    analysis::BrowserObservationLedger ledger;
    ledger.begin("guest:one", analysis::BrowserObservationRunContext{
                              "game-1", "run-1", "quick", 2});
    const auto first = valid_observation(std::string(initial_fen), "e2e4", 0, 0);
    static_cast<void>(ledger.submit("guest:one", position_context(std::string(initial_fen)), first));
    CHECK_THROWS(ledger.finalize("guest:one", "game-1", "run-1"));

    const auto second_fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1";
    auto second = valid_observation(second_fen, "e7e5", 1, 1);
    static_cast<void>(ledger.submit("guest:one", position_context(second_fen), second));
    const auto bundle = ledger.finalize("guest:one", "game-1", "run-1");
    CHECK_EQ(bundle.observations.size(), 2ULL);
    CHECK_EQ(bundle.context.expected_observations, 2ULL);
    const auto retry = ledger.finalize("guest:one", "game-1", "run-1");
    CHECK_EQ(retry.observations.size(), 2ULL);
}
