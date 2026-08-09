#include "test.hpp"

#include "pct/app/repository.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <unistd.h>

using namespace pct;

namespace {

constexpr std::string_view pgn = R"pgn([White "Alex"]
[Black "Morgan"]
[Result "1-0"]
1. e4 e5 2. Nf3 Nc6 1-0)pgn";

std::filesystem::path repository_path() {
    return std::filesystem::temp_directory_path() /
           ("pct-repository-" + std::to_string(::getpid()) + ".log");
}

json::Value read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::stringstream contents;
    contents << input.rdbuf();
    return json::parse(contents.str());
}

void remove_repository_files(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    for (const std::string name : {"games.idx", "positions.idx", "mistakes.idx", "drills.idx",
                                   "profile.idx", "resources.idx", "ratings.idx", "snapshots.idx"})
        std::filesystem::remove(path.parent_path() / name);
    std::filesystem::remove_all(path.parent_path() / "snapshots");
}

} // namespace

static_assert(std::is_base_of_v<app::IRepository, app::EventLogRepository>);
static_assert(std::is_base_of_v<app::IGameRepository, app::EventLogRepository>);
static_assert(std::is_base_of_v<app::IAnalysisRepository, app::EventLogRepository>);

TEST_CASE("repository owner ids keep account and guest scopes explicit") {
    const auto local = app::OwnerId::local();
    const auto guest = app::OwnerId::guest("guest_opaque_01");
    const auto account = app::OwnerId::account("account_opaque_01");

    CHECK(local.kind() == app::OwnerKind::Local);
    CHECK(guest.kind() == app::OwnerKind::Guest);
    CHECK(account.kind() == app::OwnerKind::Account);
    CHECK_EQ(local.value(), "local");
    CHECK_EQ(guest.value(), "guest_opaque_01");
    CHECK_EQ(account.value(), "account_opaque_01");
    CHECK(!(guest == account));
    CHECK_THROWS(app::OwnerId::guest(""));
    CHECK_THROWS(app::OwnerId::account(""));
}

TEST_CASE("event log adapter serves owner-scoped repository interfaces") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};

    {
        storage::EventLog log(path);
        app::EventLogRepository local(log);
        app::IGameRepository& games = local;
        app::IAnalysisRepository& analyses = local;

        CHECK(games.owner() == app::OwnerId::local());
        CHECK(games.add(imported) == app::AddResult::Added);
        analysis::GameAnalysis completed;
        completed.game_id = parsed.identity;
        completed.accuracy = 82.0;
        analyses.save_analysis(completed);
        CHECK(games.get(parsed.identity)->analysis.has_value());
    }
    remove_repository_files(path);
}

TEST_CASE("repository deduplicates imports and replays completed analysis") {
    const auto path = repository_path();
    std::filesystem::remove(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
        CHECK(repository.add(imported) == app::AddResult::Duplicate);
        analysis::GameAnalysis completed;
        completed.game_id = parsed.identity;
        completed.accuracy = 87.5;
        completed.white_accuracy = 91.0;
        completed.black_accuracy = 84.0;
        completed.accuracy_sample_size = 4;
        repository.save_analysis(completed);
        CHECK(repository.get(parsed.identity)->analysis.has_value());
        CHECK(std::filesystem::exists(path.parent_path() / "games.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "positions.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "mistakes.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "drills.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "profile.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "resources.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "ratings.idx"));
        CHECK(std::filesystem::exists(path.parent_path() / "snapshots.idx"));
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK_EQ(repository.size(), 1ULL);
        CHECK_EQ(repository.list().front().imported.game.identity, parsed.identity);
        const auto restored = repository.get(parsed.identity);
        CHECK(restored.has_value());
        CHECK(restored->analysis.has_value());
        CHECK_EQ(restored->analysis->accuracy, 87.5);
        CHECK_EQ(restored->analysis->accuracy_sample_size, 4ULL);
        CHECK_EQ(restored->imported.game.plies.size(), 4ULL);
    }
    remove_repository_files(path);
}

TEST_CASE("repository persists variation trees and deletion tombstones") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    std::string variation_id;
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
        const auto created = repository.create_variation(parsed.identity, 0, "after");
        variation_id = created.id;
        const auto first = repository.extend_variation(variation_id, 0, "e7e5");
        CHECK_EQ(first.nodes.size(), 2ULL);
        const auto reset = repository.reset_variation(variation_id);
        CHECK_EQ(reset.current_node_id, 0ULL);
        const auto branched = repository.extend_variation(variation_id, 0, "c7c5");
        CHECK_EQ(branched.nodes.at(0).children.size(), 2ULL);
        CHECK(std::filesystem::exists(repository.create_snapshot()));
        CHECK(repository.compact_storage() > 0);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto restored = repository.variation(variation_id);
        CHECK(restored.has_value());
        CHECK_EQ(restored->game_id, parsed.identity);
        CHECK_EQ(restored->nodes.size(), 3ULL);
        CHECK_EQ(restored->nodes.at(0).children.size(), 2ULL);
        CHECK(repository.delete_variation(variation_id));
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(!repository.variation(variation_id).has_value());
    }
    remove_repository_files(path);
}

TEST_CASE("repository round trips the Phase 2.1 per-ply classification contract") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
        analysis::GameAnalysis completed;
        completed.game_id = parsed.identity;
        analysis::MoveAssessment move;
        move.ply = 0;
        move.move_number = 1;
        move.side = "white";
        move.san = "e4";
        move.played_san = "e4";
        move.played_uci = "e2e4";
        move.fen_before = parsed.plies[0].fen_before;
        move.fen_after = parsed.plies[0].fen_after;
        move.best_uci = "e2e4";
        move.best_san = "e4";
        move.evaluation_before = 20;
        move.evaluation_after = 18;
        move.evaluation_after_best = 20;
        move.expected_points_before = 0.512;
        move.expected_points_after = 0.511;
        move.expected_points_loss = 0.001;
        move.quality = analysis::MoveQuality::Book;
        move.classification_state = analysis::ClassificationState::Final;
        move.classification_reasons = {"recognized local theory"};
        move.tactical_tags = {"development"};
        move.principal_variation = {"e2e4", "e7e5"};
        move.acceptable_alternatives = {"d2d4", "e2e4"};
        move.book_source = "local-opening-book";
        move.book_version = "2026.1";
        move.depth = 18;
        move.nodes = 12345;
        move.time_ms = 42;
        move.multipv = 3;
        move.engine_version = "stockfish-test";
        completed.requested_engine_profile = "balanced";
        completed.actual_engine_profile = "balanced";
        completed.engine_name = "Stockfish";
        completed.engine_source = "browser";
        completed.engine_hash = "sha256:test-build";
        completed.moves.push_back(move);
        repository.save_analysis(completed);
        const auto attempt = repository.record_review_attempt(parsed.identity, 0, "d2d4");
        CHECK(attempt.accepted);
        CHECK_EQ(attempt.uci, "d2d4");
        CHECK(std::filesystem::exists(repository.create_snapshot()));
        CHECK(repository.compact_storage() > 0);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto restored = repository.get(parsed.identity);
        CHECK(restored.has_value());
        CHECK(restored->analysis.has_value());
        const auto& move = restored->analysis->moves.front();
        CHECK_EQ(move.played_uci, "e2e4");
        CHECK_EQ(move.best_san, "e4");
        CHECK(move.quality == analysis::MoveQuality::Book);
        CHECK(move.classification_state == analysis::ClassificationState::Final);
        CHECK_EQ(move.classification_reasons.front(), "recognized local theory");
        CHECK_EQ(move.principal_variation.size(), 2ULL);
        CHECK_EQ(move.acceptable_alternatives.size(), 2ULL);
        CHECK_EQ(move.nodes, 12345ULL);
        CHECK_EQ(move.engine_version, "stockfish-test");
        CHECK_EQ(move.classification_model_version,
                 std::string(analysis::classification_model_version));
        CHECK_EQ(restored->analysis->requested_engine_profile, "balanced");
        CHECK_EQ(restored->analysis->actual_engine_profile, "balanced");
        CHECK_EQ(restored->analysis->engine_name, "Stockfish");
        CHECK_EQ(restored->analysis->engine_source, "browser");
        CHECK_EQ(restored->analysis->engine_hash, "sha256:test-build");
        const auto attempts = repository.review_attempts(parsed.identity);
        CHECK_EQ(attempts.size(), 1ULL);
        CHECK(attempts.front().accepted);
        CHECK_EQ(attempts.front().uci, "d2d4");
    }
    remove_repository_files(path);
}

TEST_CASE("repository migrates legacy tactical quality labels without rejecting stored JSON") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
    }
    {
        storage::EventLog log(path);
        json::Value legacy_move{json::Value::Object{
            {"ply", 0},
            {"san", "e4"},
            {"fen_before", parsed.plies[0].fen_before},
            {"fen_after", parsed.plies[0].fen_after},
            {"evaluation_before", 12},
            {"evaluation_after", 10},
            {"loss", 2},
            {"material_delta", 0},
            {"quality", "capture"},
            {"phase", "opening"},
            {"best_response", "e7e5"},
        }};
        json::Value::Array moves;
        moves.push_back(std::move(legacy_move));
        const json::Value analysis_json{json::Value::Object{
            {"game_id", parsed.identity},
            {"moves", std::move(moves)},
            {"mistakes", json::Value::Array{}},
        }};
        static_cast<void>(log.append(
            storage::EventType::AnalysisCompleted,
            json::dump(json::Value::Object{{"game_id", parsed.identity},
                                           {"analysis", analysis_json}})));
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto restored = repository.get(parsed.identity);
        CHECK(restored.has_value());
        CHECK(restored->analysis.has_value());
        const auto& move = restored->analysis->moves.front();
        CHECK_EQ(analysis::name(move.quality), "good");
        CHECK(move.classification_state == analysis::ClassificationState::Final);
        CHECK(std::find(move.tactical_tags.begin(), move.tactical_tags.end(), "capture") !=
              move.tactical_tags.end());
        CHECK_EQ(move.played_san, "e4");
        CHECK_EQ(move.classification_model_version, "legacy-fixed-cp-thresholds");
        CHECK_EQ(move.expected_points_model_version, "legacy-derived-on-read");
        CHECK(!move.classification_reasons.empty());
    }
    remove_repository_files(path);
}

TEST_CASE("repository rebuilds deleted and corrupted derived indexes from the event log") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
        static_cast<void>(repository.create_snapshot());
    }

    const auto directory = path.parent_path();
    std::filesystem::remove(directory / "profile.idx");
    {
        std::ofstream output(directory / "resources.idx", std::ios::trunc);
        output << "not json";
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK_EQ(repository.size(), 1ULL);
    }

    CHECK_EQ(read_json(directory / "profile.idx").at("version").as_int(), 1);
    CHECK_EQ(read_json(directory / "profile.idx").at("profile").at("games_imported").as_int(), 1);
    CHECK_EQ(read_json(directory / "resources.idx").at("catalog_version").as_string(),
             std::string(training::catalog_version));
    CHECK_EQ(read_json(directory / "ratings.idx").at("ratings").as_array().size(), 0ULL);
    const auto snapshots = read_json(directory / "snapshots.idx").at("snapshots").as_array();
    CHECK_EQ(snapshots.size(), 1ULL);
    CHECK(snapshots.front().at("valid").as_bool());
    remove_repository_files(path);
}

TEST_CASE("repository ignores interrupted and corrupted snapshots without losing events") {
    const auto path = repository_path();
    remove_repository_files(path);
    const chess::Game parsed = chess::parse_pgn(pgn);
    const import::ImportedGame imported{
        parsed, {}, std::string(pgn), import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
    }
    const auto snapshots = path.parent_path() / "snapshots";
    std::filesystem::create_directories(snapshots);
    {
        std::ofstream partial(snapshots / "projection-999.json.tmp");
        partial << "{\"games\":[";
    }
    {
        std::ofstream corrupt(snapshots / "projection-998.json");
        corrupt << "not a snapshot";
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK_EQ(repository.size(), 1ULL);
        CHECK(repository.get(parsed.identity).has_value());
    }
    const auto indexed = read_json(path.parent_path() / "snapshots.idx").at("snapshots").as_array();
    CHECK_EQ(indexed.size(), 1ULL);
    CHECK(!indexed.front().at("valid").as_bool());
    remove_repository_files(path);
}
