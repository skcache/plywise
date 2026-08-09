#include "test.hpp"

#include "pct/app/repository.hpp"
#include "pct/training/training.hpp"

#include <array>
#include <filesystem>
#include <numeric>
#include <unistd.h>

using namespace pct;

namespace {
constexpr std::string_view training_pgn = R"pgn([White "Alex"]
[Black "Morgan"]
[WhiteElo "1200"]
[BlackElo "1250"]
[Result "1-0"]

1. e4 e5 2. Nf3 Nc6 1-0)pgn";

std::filesystem::path training_path() {
    return std::filesystem::temp_directory_path() /
           ("pct-training-" + std::to_string(::getpid())) / "events.log";
}

analysis::GameAnalysis sample_analysis(const chess::Game& game) {
    analysis::GameAnalysis result;
    result.game_id = game.identity;
    result.moves.push_back(analysis::MoveAssessment{0, "e4", game.plies[0].fen_before,
                                                    game.plies[0].fen_after, 20, -180, 200, 0,
                                                    analysis::MoveQuality::Mistake,
                                                    analysis::GamePhase::Opening, "e7e5"});
    analysis::Mistake mistake;
    mistake.rank = 1;
    mistake.ply = 0;
    mistake.san = "a3";
    mistake.fen = game.plies[0].fen_before;
    mistake.loss = 200;
    mistake.phase = analysis::GamePhase::Opening;
    mistake.category = "Delayed development";
    mistake.explanation = "Develop a center pawn or piece before a flank pawn.";
    mistake.punishment = "e7e5";
    mistake.better_moves = {"e2e4"};
    mistake.engine_details.lines.push_back(
        engine::PrincipalVariation{1, 18, 20, std::nullopt, 1000, 5, {"e2e4"}});
    result.mistakes.push_back(std::move(mistake));
    return result;
}
} // namespace

TEST_CASE("spaced repetition schedule is deterministic and reacts to failure hints and success") {
    training::Drill drill{"d", "g", 0, chess::Board::initial().to_fen(), "Hanging piece",
                          "opening", "explain", "e7e5", {"e2e4"}, 2, 250, 1000, {}};
    CHECK(training::schedule(drill, 1000).state == training::DrillState::New);
    drill.attempts.push_back({1, 2000, false, "d2d4", 9000, 0, 0});
    CHECK_EQ(training::next_hint_level(drill), 1);
    CHECK(training::schedule(drill, 2000).state == training::DrillState::Due);
    drill.attempts.push_back({2, 3000, true, "e2e4", 4000, 1, 1});
    const auto scheduled = training::schedule(drill, 3000);
    CHECK(scheduled.state == training::DrillState::Upcoming);
    CHECK_EQ(scheduled.interval_days, 1);
    CHECK_EQ(scheduled.total, 2ULL);
}

TEST_CASE("review queue interleaves categories to prevent weakness starvation") {
    std::vector<training::Drill> drills;
    for (int index = 0; index < 3; ++index)
        drills.push_back({"a" + std::to_string(index), "g", 0, chess::Board::initial().to_fen(),
                          "A", "opening", {}, {}, {"e2e4"}, 1, 300, 0, {}});
    drills.push_back({"b", "g", 0, chess::Board::initial().to_fen(), "B", "opening", {}, {},
                      {"e2e4"}, 1, 100, 0, {}});
    const auto queue = training::review_queue(std::move(drills), 1000);
    CHECK_EQ(queue.size(), 4ULL);
    CHECK(queue[0].category != queue[1].category);
}

TEST_CASE("scheduler covers reveal overdue mastered and deterministic replay") {
    constexpr std::int64_t day = 24LL * 60LL * 60LL * 1000LL;
    training::Drill revealed{"reveal", "g", 0, chess::Board::initial().to_fen(),
                             "Fork", "middlegame", {}, {}, {"e2e4"}, 3, 300, 0, {}};
    revealed.attempts.push_back({1, 1000, true, "e2e4", 1000, 3, 0});
    CHECK_EQ(training::schedule(revealed, 1000).interval_days, 0);
    CHECK(training::schedule(revealed, 1000).state == training::DrillState::Due);

    training::Drill practiced{"master", "g", 0, chess::Board::initial().to_fen(),
                              "Pin", "middlegame", {}, {}, {"e2e4"}, 3, 300, 0, {}};
    std::int64_t reviewed = 1000;
    for (std::uint64_t id = 1; id <= 5; ++id) {
        practiced.attempts.push_back({id, reviewed, true, "e2e4", 1000, 0, 0});
        reviewed += day;
    }
    const auto mastered = training::schedule(practiced, reviewed);
    CHECK(mastered.interval_days >= 21);
    CHECK(mastered.state == training::DrillState::Mastered);
    const auto replayed = training::schedule(practiced, reviewed);
    CHECK_EQ(replayed.next_review_ms, mastered.next_review_ms);
    CHECK_EQ(replayed.priority, mastered.priority);
    CHECK(training::schedule(practiced, mastered.next_review_ms + day).state ==
          training::DrillState::Due);
}

TEST_CASE("drills attempts profiles and recommendations survive event replay") {
    const auto path = training_path();
    std::filesystem::remove_all(path.parent_path());
    const auto game = chess::parse_pgn(training_pgn);
    const import::ImportedGame imported{game, {}, std::string(training_pgn),
                                        import::ImportMethod::ManualPgn};
    std::string drill_id;
    std::string profile_before_restart;
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK(repository.add(imported) == app::AddResult::Added);
        repository.save_player_identity(training::PlayerIdentity{
            std::string(training::player_identity_contract_version), game.identity, "Alex", "pgn",
            training::PlayerIdentityDecision::Confirmed, 0});
        repository.save_analysis(sample_analysis(game));
        const auto drills = repository.drills(10'000);
        CHECK_EQ(drills.size(), 1ULL);
        drill_id = drills.front().id;
        CHECK_EQ(drills.front().played_move, "e2e4");
        CHECK(drills.front().fen_after_mistake != drills.front().fen);
        CHECK(drills.front().fen_after_punishment != drills.front().fen_after_mistake);
        CHECK_THROWS(repository.record_attempt(drill_id, "e2e5", 1000, 0, 20'000));
        const auto failed = repository.record_attempt(drill_id, "d2d4", 8000, 0, 20'000);
        CHECK(!failed.correct);
        CHECK_EQ(training::next_hint_level(*repository.drill(drill_id)), 1);
        const auto correct = repository.record_attempt(drill_id, "e2e4", 4000, 1, 30'000);
        CHECK(correct.correct);
        const auto profile = repository.profile();
        CHECK_EQ(profile.games_imported, 1ULL);
        CHECK_EQ(profile.games_analyzed, 1ULL);
        CHECK_EQ(profile.games_analyzed_7_days, 1ULL);
        CHECK_EQ(profile.games_analyzed_30_days, 1ULL);
        CHECK_EQ(profile.player_name, "Alex");
        CHECK_EQ(profile.latest_rating, 1200);
        CHECK_EQ(profile.rating_observations, 1ULL);
        CHECK_EQ(profile.total_mistakes, 1ULL);
        CHECK_EQ(profile.drill_attempts, 2ULL);
        CHECK_EQ(profile.retention_reviews, 1ULL);
        CHECK_EQ(profile.retained_reviews, 1ULL);
        CHECK_EQ(profile.retention_rate, 1.0);
        profile_before_restart = json::dump(training::to_json(profile));
        const auto recommendations = repository.recommendations();
        CHECK(std::any_of(recommendations.begin(), recommendations.end(), [](const auto& value) {
            return value.resource.id == "opening-principles";
        }));
        const auto snapshot = repository.create_snapshot();
        CHECK(std::filesystem::exists(snapshot));
        repository.complete_resource("opening-principles", 40'000);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto restored = repository.drill(drill_id);
        CHECK(restored.has_value());
        CHECK_EQ(restored->attempts.size(), 2ULL);
        CHECK(!restored->fen_after_punishment.empty());
        CHECK_EQ(repository.profile().drill_correct, 1ULL);
        CHECK_EQ(json::dump(training::to_json(repository.profile())), profile_before_restart);
        const auto recommendations = repository.recommendations();
        const auto opening_principles =
            std::find_if(recommendations.begin(), recommendations.end(), [](const auto& value) {
                return value.resource.id == "opening-principles";
            });
        CHECK(opening_principles != recommendations.end());
        CHECK(opening_principles->completed);
        CHECK(std::filesystem::exists(path.parent_path() / "drills.idx"));
    }
    std::filesystem::remove_all(path.parent_path());
}

TEST_CASE("drill solutions require legal engine-verified principal variation moves") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-drill-verification-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const auto game = chess::parse_pgn(training_pgn);
    storage::EventLog log(path);
    app::Repository repository(log);
    static_cast<void>(repository.add(import::ImportedGame{
        game, {}, std::string(training_pgn), import::ImportMethod::ManualPgn}));
    auto unverified = sample_analysis(game);
    unverified.mistakes.front().better_moves = {"e2e5"};
    unverified.mistakes.front().engine_details.lines.front().moves = {"e2e5"};
    repository.save_analysis(unverified);
    CHECK(repository.drills(0).empty());

    repository.save_analysis(sample_analysis(game));
    CHECK_EQ(repository.drills(0).size(), 1ULL);
    CHECK_EQ(repository.drills(0).front().solutions.front(), "e2e4");
    std::filesystem::remove_all(directory);
}

TEST_CASE("guided lesson stores the changed threat attacked piece and engine response") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-guided-lesson-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const std::string pgn =
        "[White \"Learner\"]\n[Black \"Coach\"]\n[Result \"0-1\"]\n\n"
        "1. e4 e5 2. Qh5 Nc6 3. Qxe5+ Nxe5 0-1";
    const auto game = chess::parse_pgn(pgn);
    storage::EventLog log(path);
    app::Repository repository(log);
    static_cast<void>(repository.add(
        import::ImportedGame{game, {}, pgn, import::ImportMethod::ManualPgn}));
    analysis::GameAnalysis completed;
    completed.game_id = game.identity;
    analysis::Mistake mistake;
    mistake.ply = 4;
    mistake.fen = game.plies[4].fen_before;
    mistake.category = "Hanging queen";
    mistake.phase = analysis::GamePhase::Opening;
    mistake.explanation = "The queen can be captured.";
    mistake.punishment = "c6e5";
    mistake.better_moves = {"h5f3"};
    mistake.engine_details.lines.push_back(
        engine::PrincipalVariation{1, 18, 200, std::nullopt, 1000, 5, {"h5f3"}});
    completed.mistakes.push_back(std::move(mistake));
    repository.save_analysis(completed);
    const auto drill = repository.drills(0).front();
    CHECK_EQ(drill.played_move, "h5e5");
    CHECK_EQ(drill.opponent_response, "c6e5");
    CHECK(drill.changed_threat.find("captures your queen") != std::string::npos);
    CHECK(std::find(drill.attacked_pieces.begin(), drill.attacked_pieces.end(),
                    "queen on e5") != drill.attacked_pieces.end());
    {
        storage::EventLog replay_log(path);
        app::Repository replayed(replay_log);
        const auto restored = replayed.drills(0).front();
        CHECK_EQ(restored.changed_threat, drill.changed_threat);
        CHECK(restored.attacked_pieces == drill.attacked_pieces);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("repository creates a bounded-startup snapshot every ten completed analyses") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-periodic-snapshot-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    const std::array<std::string_view, 10> moves{
        "e4 e5", "d4 d5", "c4 c5", "Nf3 Nf6", "g3 g6",
        "b3 b6", "f4 f5", "Nc3 Nc6", "a3 a6", "h3 h6"};
    for (int index = 0; index < 10; ++index) {
        const std::string pgn = "[Event \"" + std::to_string(index) +
                                "\"]\n[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. " +
                                std::string(moves[static_cast<std::size_t>(index)]) + " 1-0";
        const auto game = chess::parse_pgn(pgn);
        const import::ImportedGame imported{game, {}, pgn, import::ImportMethod::ManualPgn};
        CHECK(repository.add(imported) == app::AddResult::Added);
        analysis::GameAnalysis completed;
        completed.game_id = game.identity;
        repository.save_analysis(completed);
    }
    std::size_t snapshots = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory / "snapshots"))
        if (entry.path().extension() == ".json")
            ++snapshots;
    CHECK_EQ(snapshots, 1ULL);
    std::filesystem::remove_all(directory);
}

TEST_CASE("drill session hint state and measured response time survive restart") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-drill-session-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const auto game = chess::parse_pgn(training_pgn);
    const import::ImportedGame imported{game, {}, std::string(training_pgn),
                                        import::ImportMethod::ManualPgn};
    std::string drill_id;
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        static_cast<void>(repository.add(imported));
        repository.save_analysis(sample_analysis(game));
        drill_id = repository.drills(1000).front().id;
        CHECK_EQ(repository.begin_drill_session(drill_id, 1000).session_started_at_ms, 1000LL);
        CHECK_THROWS(repository.advance_hint(drill_id, 1200));
        const auto first_failure = repository.record_attempt(drill_id, "d2d4", 0, 0, 2000);
        CHECK(!first_failure.correct);
        CHECK_EQ(training::available_hint_level(*repository.drill(drill_id)), 1);
        CHECK_EQ(repository.advance_hint(drill_id, 2200).session_hint_level, 1);
        CHECK_THROWS(repository.advance_hint(drill_id, 2400));
        const auto second_failure = repository.record_attempt(drill_id, "d2d4", 0, 0, 3000);
        CHECK(!second_failure.correct);
        CHECK_EQ(repository.advance_hint(drill_id, 3200).session_hint_level, 2);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto resumed = repository.drill(drill_id);
        CHECK_EQ(resumed->session_hint_level, 2);
        CHECK_EQ(resumed->session_started_at_ms, 1000LL);
        CHECK_EQ(training::available_hint_level(*resumed), 2);
        const auto attempt = repository.record_attempt(drill_id, "e2e4", 999999, 3, 6000);
        CHECK(attempt.correct);
        CHECK_EQ(attempt.hint_level, 2);
        CHECK_EQ(attempt.response_time_ms, 5000ULL);
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto reset = repository.drill(drill_id);
        CHECK_EQ(reset->session_hint_level, 0);
        CHECK_EQ(reset->session_started_at_ms, 0LL);
        CHECK_EQ(reset->attempts.size(), 3ULL);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("snapshot plus tail restores shallow state without marking it complete") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-shallow-snapshot-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const auto game = chess::parse_pgn(training_pgn);
    const import::ImportedGame imported{game, {}, std::string(training_pgn),
                                        import::ImportMethod::ManualPgn};
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        static_cast<void>(repository.add(imported));
        auto shallow = sample_analysis(game);
        shallow.mistakes.clear();
        repository.save_shallow_analysis(shallow);
        static_cast<void>(repository.create_snapshot());
        repository.record_job_state(game.identity, "queued");
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        const auto restored = repository.get(game.identity);
        CHECK(restored.has_value());
        CHECK(restored->shallow_analysis.has_value());
        CHECK(!restored->analysis.has_value());
        CHECK_EQ(repository.profile().games_shallow_analyzed, 1ULL);
        CHECK_EQ(repository.recoverable_analysis_jobs().size(), 1ULL);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("snapshot plus drill-attempt tail reproduces the complete profile") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-profile-tail-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    const auto game = chess::parse_pgn(training_pgn);
    const import::ImportedGame imported{game, {}, std::string(training_pgn),
                                        import::ImportMethod::ManualPgn};
    std::string expected;
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        static_cast<void>(repository.add(imported));
        repository.save_analysis(sample_analysis(game));
        static_cast<void>(repository.create_snapshot());
        const std::string drill_id = repository.drills(1000).front().id;
        static_cast<void>(repository.begin_drill_session(drill_id, 1000));
        static_cast<void>(repository.record_attempt(drill_id, "d2d4", 0, 0, 2000));
        static_cast<void>(repository.advance_hint(drill_id, 2500));
        static_cast<void>(repository.record_attempt(drill_id, "e2e4", 0, 0, 4000));
        expected = json::dump(training::to_json(repository.profile()));
    }
    {
        storage::EventLog log(path);
        app::Repository repository(log);
        CHECK_EQ(json::dump(training::to_json(repository.profile())), expected);
        CHECK_EQ(repository.profile().drill_attempts, 2ULL);
        CHECK_EQ(repository.profile().retained_reviews, 1ULL);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("multiple games produce explainable recurrence and profile denominators") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-profile-denominators-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    const std::array<std::string, 2> pgns{
        "[White \"Alex\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0",
        "[White \"Alex\"]\n[Black \"C\"]\n[Result \"0-1\"]\n\n1. d4 d5 0-1"};
    for (const auto& pgn : pgns) {
        const auto game = chess::parse_pgn(pgn);
        static_cast<void>(repository.add(
            import::ImportedGame{game, {}, pgn, import::ImportMethod::ManualPgn}));
        if (!repository.player_identity())
            repository.save_player_identity(training::PlayerIdentity{
                std::string(training::player_identity_contract_version), game.identity, "Alex", "pgn",
                training::PlayerIdentityDecision::Confirmed, 0});
        auto completed = sample_analysis(game);
        completed.game_id = game.identity;
        completed.moves.front().fen_before = game.plies.front().fen_before;
        completed.moves.front().fen_after = game.plies.front().fen_after;
        completed.mistakes.front().fen = game.plies.front().fen_before;
        completed.mistakes.front().better_moves = {chess::uci(game.plies.front().move)};
        completed.mistakes.front().engine_details.lines.front().moves =
            completed.mistakes.front().better_moves;
        repository.save_analysis(completed);
    }
    const auto profile = repository.profile();
    CHECK_EQ(profile.games_imported, 2ULL);
    CHECK_EQ(profile.games_analyzed, 2ULL);
    CHECK_EQ(profile.total_mistakes, 2ULL);
    CHECK_EQ(profile.analysis_completion_rate, 1.0);
    CHECK_EQ(profile.total_positions, 2ULL);
    CHECK_EQ(profile.king_safety_violations.denominator, 2ULL);
    CHECK_EQ(profile.time_management_failures.denominator, 0ULL);
    CHECK_EQ(profile.activity_trend.size(), 14ULL);
    const auto trend_games = std::accumulate(
        profile.activity_trend.begin(), profile.activity_trend.end(), std::size_t{0},
        [](std::size_t total, const auto& point) { return total + point.games_analyzed; });
    const auto trend_mistakes = std::accumulate(
        profile.activity_trend.begin(), profile.activity_trend.end(), std::size_t{0},
        [](std::size_t total, const auto& point) { return total + point.mistakes; });
    CHECK_EQ(trend_games, 2ULL);
    CHECK_EQ(trend_mistakes, 2ULL);
    const auto weakness = std::find_if(profile.weaknesses.begin(), profile.weaknesses.end(),
                                       [](const auto& value) {
                                           return value.category == "Delayed development";
                                       });
    CHECK(weakness != profile.weaknesses.end());
    CHECK_EQ(weakness->occurrences, 2ULL);
    CHECK_EQ(weakness->games, 2ULL);
    CHECK_EQ(weakness->recurrence_rate, 1.0);
    CHECK(weakness->repeated_interval_days.has_value());
    std::filesystem::remove_all(directory);
}

TEST_CASE("five eligible games expose every statistically gated dashboard rate") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pct-profile-rates-" + std::to_string(::getpid()));
    const auto path = directory / "events.log";
    std::filesystem::remove_all(directory);
    storage::EventLog log(path);
    app::Repository repository(log);
    const std::array<std::string_view, 5> moves{"e4 e5", "d4 d5", "c4 c5", "Nf3 Nf6",
                                                "g3 g6"};
    for (std::size_t index = 0; index < moves.size(); ++index) {
        const std::string result = index < 3 ? "1-0" : "0-1";
        const std::string pgn =
            "[Event \"Rate " + std::to_string(index) + "\"]\n[White \"Alex\"]\n"
            "[Black \"Opponent " + std::to_string(index) + "\"]\n[Result \"" + result +
            "\"]\n[TimeControl \"600+0\"]\n\n1. " +
            std::string(moves[index].substr(0, moves[index].find(' '))) +
            " {[%clk 0:09:59]} " +
            std::string(moves[index].substr(moves[index].find(' ') + 1)) + " " + result;
        const auto game = chess::parse_pgn(pgn);
        static_cast<void>(repository.add(
            import::ImportedGame{game, {}, pgn, import::ImportMethod::ManualPgn}));
        if (!repository.player_identity())
            repository.save_player_identity(training::PlayerIdentity{
                std::string(training::player_identity_contract_version), game.identity, "Alex", "pgn",
                training::PlayerIdentityDecision::Confirmed, 0});
        auto completed = sample_analysis(game);
        completed.game_id = game.identity;
        completed.moves.front().phase = analysis::GamePhase::Endgame;
        completed.moves.front().fen_before = game.plies.front().fen_before;
        completed.moves.front().fen_after = game.plies.front().fen_after;
        auto& mistake = completed.mistakes.front();
        mistake.fen = game.plies.front().fen_before;
        mistake.phase = analysis::GamePhase::Endgame;
        mistake.category = index < 2 ? "Instant-move blunder"
                                    : index < 4 ? "Open king position"
                                                : "Delayed development";
        mistake.better_moves = {chess::uci(game.plies.front().move)};
        mistake.engine_details.lines.front().moves = mistake.better_moves;
        mistake.punishment = chess::uci(game.plies[1].move);
        repository.save_analysis(completed);
    }
    const auto profile = repository.profile();
    CHECK_EQ(profile.games_imported, 5ULL);
    CHECK_EQ(profile.games_analyzed, 5ULL);
    CHECK_EQ(profile.games_analyzed_7_days, 5ULL);
    CHECK_EQ(profile.games_analyzed_30_days, 5ULL);
    CHECK_EQ(profile.total_positions, 5ULL);
    CHECK_EQ(profile.total_mistakes, 5ULL);
    CHECK_EQ(profile.endgame_conversion.denominator, 5ULL);
    CHECK_EQ(profile.endgame_conversion.numerator, 3ULL);
    CHECK_EQ(*profile.endgame_conversion.rate, 0.6);
    CHECK_EQ(profile.king_safety_violations.denominator, 5ULL);
    CHECK_EQ(profile.king_safety_violations.numerator, 2ULL);
    CHECK_EQ(*profile.king_safety_violations.rate, 0.4);
    CHECK_EQ(profile.time_management_failures.denominator, 5ULL);
    CHECK_EQ(profile.time_management_failures.numerator, 2ULL);
    CHECK_EQ(*profile.time_management_failures.rate, 0.4);
    CHECK_EQ(profile.analysis_completion_rate, 1.0);
    std::filesystem::remove_all(directory);
}

TEST_CASE("editable resource catalog covers media books modules and repertoire relevance") {
    const auto catalog = training::default_catalog();
    const auto has_kind = [&](std::string_view kind) {
        return std::any_of(catalog.begin(), catalog.end(),
                           [&](const auto& resource) { return resource.kind == kind; });
    };
    CHECK(has_kind("interactive"));
    CHECK(has_kind("video_timestamp"));
    CHECK(has_kind("free_video"));
    CHECK(has_kind("book_chapter"));
    CHECK(has_kind("exercise"));
    CHECK(has_kind("tactical_theme"));
    CHECK(has_kind("endgame_module"));
    CHECK(has_kind("repertoire_lesson"));
    training::Profile profile;
    profile.latest_rating = 1200;
    training::Weakness weakness;
    weakness.category = "Delayed development";
    weakness.occurrences = 3;
    weakness.games = 2;
    weakness.phases["opening"] = 3;
    profile.weaknesses.push_back(weakness);
    profile.openings.push_back({"C50", "Italian Game", 2, 2, 90.0});
    const auto recommendations = training::recommend(profile, catalog, {});
    const auto repertoire =
        std::find_if(recommendations.begin(), recommendations.end(), [](const auto& value) {
            return value.resource.id == "italian-repertoire";
        });
    CHECK(repertoire != recommendations.end());
    CHECK(repertoire->evidence.find("Delayed development") != std::string::npos);
    CHECK(repertoire->evidence.find("recurrence") != std::string::npos);
    CHECK(repertoire->evidence.find("average loss") != std::string::npos);
    CHECK(repertoire->evidence.find("repertoire match") != std::string::npos);
}

TEST_CASE("resource ranking explains severity drills repertoire and study recency") {
    constexpr std::int64_t day = 24LL * 60LL * 60LL * 1000LL;
    constexpr std::int64_t now = 1000LL * day;
    training::Profile profile;
    profile.games_analyzed = 4;
    training::Weakness weakness;
    weakness.category = "Fork";
    weakness.occurrences = 3;
    weakness.games = 3;
    weakness.recurrence_rate = 0.75;
    weakness.average_loss_cp = 240;
    weakness.drill_accuracy = 0.4;
    weakness.phases["middlegame"] = 3;
    profile.weaknesses.push_back(weakness);
    profile.openings.push_back({"C50", "Italian Game", 3, 2, 80.0});
    const training::Resource general{"general", "General", "exercise", "local", {"Fork"},
                                     "middlegame", 0, 3000, "none", "any"};
    const training::Resource italian{"italian", "Italian", "repertoire_lesson", "local",
                                     {"Fork"}, "middlegame", 0, 3000, "none",
                                     "Italian Game"};
    const auto uncompleted = training::recommend(profile, {general, italian}, {}, now);
    CHECK_EQ(uncompleted.front().resource.id, "italian");
    CHECK(uncompleted.front().evidence.find("75% recurrence") != std::string::npos);
    CHECK(uncompleted.front().evidence.find("240 cp") != std::string::npos);

    const auto recent = training::recommend(profile, {general}, {{"general", now - day}}, now);
    const auto old = training::recommend(profile, {general}, {{"general", now - 100 * day}}, now);
    CHECK(recent.front().completed);
    CHECK(old.front().priority > recent.front().priority);
    CHECK(recent.front().evidence.find("studied 1 days ago") != std::string::npos);
    CHECK(uncompleted.back().priority > recent.front().priority);
}
