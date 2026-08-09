#include "pct/training/training.hpp"

#include "pct/common/error.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pct::training {
namespace {
constexpr std::int64_t day_ms = 24LL * 60LL * 60LL * 1000LL;

int interval_after(const Drill& drill) {
    int interval = 0;
    std::size_t streak = 0;
    for (const DrillAttempt& attempt : drill.attempts) {
        if (!attempt.correct || attempt.hint_level >= 3) {
            interval = 0;
            streak = 0;
            continue;
        }
        ++streak;
        if (streak == 1)
            interval = 1;
        else if (streak == 2)
            interval = 3;
        else
            interval = std::max(interval + 1, static_cast<int>(std::lround(interval * 2.2)));
    }
    return interval;
}
} // namespace

Schedule schedule(const Drill& drill, std::int64_t now_ms, std::size_t category_frequency) {
    Schedule result;
    result.total = drill.attempts.size();
    for (const DrillAttempt& attempt : drill.attempts) {
        if (attempt.correct)
            ++result.correct;
        result.last_review_ms = std::max(result.last_review_ms, attempt.attempted_at_ms);
    }
    result.success_rate = result.total == 0 ? 0.0 : static_cast<double>(result.correct) /
                                                          static_cast<double>(result.total);
    result.interval_days = interval_after(drill);
    result.next_review_ms = result.total == 0 ? drill.created_at_ms
                                              : result.last_review_ms + result.interval_days * day_ms;
    const bool last_correct = !drill.attempts.empty() && drill.attempts.back().correct &&
                              drill.attempts.back().hint_level < 3;
    result.retention = result.total == 0 ? 0.0 : (last_correct ? result.success_rate : 0.0);
    if (result.total == 0)
        result.state = DrillState::New;
    else if (result.interval_days >= 21 && result.success_rate >= 0.8 && now_ms < result.next_review_ms)
        result.state = DrillState::Mastered;
    else if (now_ms >= result.next_review_ms)
        result.state = DrillState::Due;
    else
        result.state = DrillState::Upcoming;
    const int overdue_days = now_ms > result.next_review_ms
                                 ? static_cast<int>((now_ms - result.next_review_ms) / day_ms)
                                 : 0;
    result.priority = (last_correct ? 0 : 400) + static_cast<int>(category_frequency) * 30 +
                      std::min(300, drill.impact_cp / 2) + std::min(200, overdue_days * 10) -
                      (result.state == DrillState::Mastered ? 300 : 0);
    return result;
}

int next_hint_level(const Drill& drill) {
    if (drill.attempts.empty() || drill.attempts.back().correct)
        return drill.session_hint_level;
    std::size_t failures = 0;
    for (auto it = drill.attempts.rbegin(); it != drill.attempts.rend() && !it->correct; ++it)
        ++failures;
    return std::max(drill.session_hint_level,
                    std::min(3, static_cast<int>(failures)));
}

int available_hint_level(const Drill& drill) {
    if (drill.session_started_at_ms == 0)
        return 0;
    std::size_t failures = 0;
    for (const DrillAttempt& attempt : drill.attempts) {
        if (attempt.attempted_at_ms >= drill.session_started_at_ms && !attempt.correct)
            ++failures;
    }
    return std::min(3, static_cast<int>(failures));
}

std::vector<Drill> review_queue(std::vector<Drill> drills, std::int64_t now_ms) {
    std::map<std::string, std::vector<Drill>> grouped;
    for (auto& drill : drills)
        grouped[drill.category].push_back(std::move(drill));
    std::map<std::string, std::size_t> frequency;
    for (const auto& [category, values] : grouped)
        frequency[category] = values.size();
    for (auto& [category, values] : grouped) {
        std::stable_sort(values.begin(), values.end(), [&](const auto& left, const auto& right) {
            return schedule(left, now_ms, frequency[category]).priority >
                   schedule(right, now_ms, frequency[category]).priority;
        });
    }
    std::vector<std::string> categories;
    for (const auto& [category, _] : grouped)
        categories.push_back(category);
    std::stable_sort(categories.begin(), categories.end(), [&](const auto& left, const auto& right) {
        return schedule(grouped[left].front(), now_ms, frequency[left]).priority >
               schedule(grouped[right].front(), now_ms, frequency[right]).priority;
    });
    std::vector<Drill> result;
    std::size_t round = 0;
    while (result.size() < drills.size()) {
        for (const auto& category : categories)
            if (round < grouped[category].size())
                result.push_back(std::move(grouped[category][round]));
        ++round;
    }
    return result;
}

std::vector<Resource> default_catalog() {
    const std::vector<Resource> fallback{
        {"lichess-hanging", "Lichess: Practice hanging pieces", "interactive",
         "https://lichess.org/practice", {"Hanging piece", "Hanging queen", "Ignored attack"},
         "any", 0, 1800, "none", "any"},
        {"chesstactics-forcing", "Forcing moves: checks, captures, threats", "lesson",
         "Local lesson 1", {"Missed check", "Missed free capture", "One-move tactical loss"},
         "middlegame", 0, 1600, "legal move basics", "any"},
        {"opening-principles", "Opening principles: develop, control, castle", "lesson",
         "Local lesson 2", {"Delayed development", "Premature queen development", "Uncastled king"},
         "opening", 0, 1800, "none", "any"},
        {"king-safety", "King safety and back-rank patterns", "lesson", "Local lesson 3",
         {"Unsafe king", "Back-rank weakness", "Failed response to mate threat"}, "any", 600,
         2200, "forcing moves", "any"},
        {"endgame-pawns", "Passed pawns and active kings", "lesson", "Local lesson 4",
         {"Ignored passed pawn", "Incorrect king activity", "Promotion-race error"}, "endgame",
         600, 2200, "basic checkmates", "any"},
    };
    std::filesystem::path catalog_path = PCT_RESOURCE_CATALOG_PATH;
    if (const char* override_path = std::getenv("PCT_RESOURCE_CATALOG");
        override_path != nullptr && *override_path != '\0')
        catalog_path = override_path;
    else if (std::filesystem::exists("resources/catalog.json"))
        catalog_path = "resources/catalog.json";
    std::ifstream input(catalog_path);
    if (!input)
        return fallback;
    try {
        std::stringstream contents;
        contents << input.rdbuf();
        const json::Value document = json::parse(contents.str());
        if (document.at("version").as_string() != catalog_version)
            return fallback;
        std::vector<Resource> catalog;
        for (const auto& item : document.at("resources").as_array()) {
            Resource resource;
            resource.id = item.at("id").as_string();
            resource.title = item.at("title").as_string();
            resource.kind = item.at("kind").as_string();
            resource.locator = item.at("locator").as_string();
            for (const auto& category : item.at("categories").as_array())
                resource.categories.push_back(category.as_string());
            resource.phase = item.at("phase").as_string();
            resource.minimum_rating = item.at("minimum_rating").as_int();
            resource.maximum_rating = item.at("maximum_rating").as_int();
            resource.prerequisite = item.at("prerequisite").as_string();
            resource.opening = item.get("opening", "any").as_string();
            catalog.push_back(std::move(resource));
        }
        return catalog.empty() ? fallback : catalog;
    } catch (const std::exception&) {
        return fallback;
    }
}

std::vector<Recommendation> recommend(const Profile& profile, const std::vector<Resource>& catalog,
                                      const std::map<std::string, std::int64_t>& completions,
                                      std::int64_t now_ms) {
    std::vector<Recommendation> result;
    if (now_ms == 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    for (const Resource& resource : catalog) {
        if (profile.latest_rating > 0 &&
            (profile.latest_rating < resource.minimum_rating ||
             profile.latest_rating > resource.maximum_rating))
            continue;
        const Weakness* best = nullptr;
        for (const Weakness& weakness : profile.weaknesses) {
            if (std::find(resource.categories.begin(), resource.categories.end(), weakness.category) !=
                    resource.categories.end() &&
                (!best || weakness.occurrences > best->occurrences)) {
                best = &weakness;
            }
        }
        if (!best)
            continue;
        const bool complete = completions.contains(resource.id);
        int completion_penalty = 0;
        int days_since_study = -1;
        if (complete) {
            const auto studied_at = completions.at(resource.id);
            days_since_study = now_ms > studied_at
                                   ? static_cast<int>((now_ms - studied_at) / day_ms)
                                   : 0;
            completion_penalty = std::max(0, 500 - days_since_study * 5);
        }
        const int phase_bonus = resource.phase == "any" || best->phases.contains(resource.phase)
                                    ? 100
                                    : 0;
        const bool opening_relevant =
            std::any_of(profile.openings.begin(), profile.openings.end(),
                        [&](const auto& opening) {
                            return opening.name == resource.opening && opening.games >= 2;
                        });
        const int repertoire_bonus =
            resource.opening == "any" ? 0 : opening_relevant ? 150 : -150;
        const int recurrence_score =
            static_cast<int>(std::lround(best->recurrence_rate * 300.0));
        const int severity_score = static_cast<int>(best->average_loss_cp / 2.0);
        const int drill_score = best->drill_accuracy < 0.6 ? 200 : 0;
        const int priority = static_cast<int>(best->occurrences * 100) + recurrence_score +
                             severity_score + drill_score + phase_bonus + repertoire_bonus -
                             completion_penalty;
        std::string evidence =
            best->category + " occurred " + std::to_string(best->occurrences) + " times across " +
            std::to_string(best->games) + " games (" +
            std::to_string(static_cast<int>(std::lround(best->recurrence_rate * 100.0))) +
            "% recurrence); average loss is " +
            std::to_string(static_cast<int>(std::lround(best->average_loss_cp))) +
            " cp and drill accuracy is " +
            std::to_string(static_cast<int>(best->drill_accuracy * 100.0)) + "%";
        evidence += phase_bonus > 0 ? "; phase matches" : "; phase does not match";
        if (resource.opening != "any")
            evidence += opening_relevant ? "; repertoire match: " + resource.opening
                                         : "; repertoire not yet established: " + resource.opening;
        if (complete)
            evidence += "; studied " + std::to_string(days_since_study) + " days ago";
        evidence += ".";
        result.push_back({resource,
                          std::move(evidence),
                          priority, complete});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.priority > right.priority;
    });
    return result;
}

std::string_view name(DrillState state) {
    switch (state) {
    case DrillState::New: return "new";
    case DrillState::Due: return "due";
    case DrillState::Upcoming: return "upcoming";
    case DrillState::Mastered: return "mastered";
    }
    return "new";
}

json::Value to_json(const DrillAttempt& attempt) {
    return json::Value::Object{{"id", static_cast<double>(attempt.id)},
                               {"attempted_at_ms", static_cast<double>(attempt.attempted_at_ms)},
                               {"correct", attempt.correct}, {"move", attempt.move},
                               {"response_time_ms", static_cast<double>(attempt.response_time_ms)},
                               {"hint_level", attempt.hint_level}, {"retries", attempt.retries}};
}

json::Value to_json(const Drill& drill, std::int64_t now_ms, std::size_t category_frequency) {
    json::Value::Array attempts;
    for (const auto& attempt : drill.attempts)
        attempts.push_back(to_json(attempt));
    json::Value::Array solutions;
    for (const auto& solution : drill.solutions)
        solutions.emplace_back(solution);
    json::Value::Array validation_evidence;
    for (const auto& evidence : drill.validation_evidence)
        validation_evidence.emplace_back(evidence);
    const Schedule scheduled = schedule(drill, now_ms, category_frequency);
    return json::Value::Object{
        {"id", drill.id}, {"source_game_id", drill.source_game_id}, {"source_ply", drill.source_ply},
        {"fen", drill.fen}, {"category", drill.category}, {"phase", drill.phase},
        {"explanation", drill.explanation}, {"punishment", drill.punishment},
        {"solutions", std::move(solutions)}, {"difficulty", drill.difficulty},
        {"impact_cp", drill.impact_cp}, {"created_at_ms", static_cast<double>(drill.created_at_ms)},
        {"attempts", std::move(attempts)}, {"hint_level", drill.session_hint_level},
        {"available_hint_level", available_hint_level(drill)},
        {"played_move", drill.played_move},
        {"fen_after_mistake", drill.fen_after_mistake},
        {"fen_after_punishment", drill.fen_after_punishment},
        {"session_hint_level", drill.session_hint_level},
        {"session_started_at_ms", static_cast<double>(drill.session_started_at_ms)},
        {"changed_threat", drill.changed_threat},
        {"attacked_pieces", [&] {
             json::Value::Array pieces;
             for (const auto& piece : drill.attacked_pieces)
                 pieces.emplace_back(piece);
             return json::Value(std::move(pieces));
         }()},
        {"opponent_response", drill.opponent_response},
        {"source_type", drill.source_type},
        {"provenance", drill.provenance},
        {"corpus_version", drill.corpus_version},
        {"validation_evidence", std::move(validation_evidence)},
        {"scheduler_version", std::string(scheduler_version)},
        {"schedule", json::Value::Object{{"state", std::string(name(scheduled.state))},
                                           {"last_review_ms", static_cast<double>(scheduled.last_review_ms)},
                                           {"next_review_ms", static_cast<double>(scheduled.next_review_ms)},
                                           {"correct", scheduled.correct}, {"total", scheduled.total},
                                           {"success_rate", scheduled.success_rate},
                                           {"retention", scheduled.retention},
                                           {"interval_days", scheduled.interval_days},
                                           {"priority", scheduled.priority}}}};
}

std::string_view name(PlayerIdentityDecision decision) {
    switch (decision) {
    case PlayerIdentityDecision::Confirmed: return "confirmed";
    case PlayerIdentityDecision::Declined: return "declined";
    case PlayerIdentityDecision::Uncertain: return "uncertain";
    }
    return "uncertain";
}

PlayerIdentityDecision player_identity_decision(std::string_view value) {
    if (value == "confirmed")
        return PlayerIdentityDecision::Confirmed;
    if (value == "declined")
        return PlayerIdentityDecision::Declined;
    if (value == "uncertain")
        return PlayerIdentityDecision::Uncertain;
    throw Error(ErrorCode::InvalidArgument, "player identity decision is invalid");
}

void validate_player_identity(const PlayerIdentity& identity) {
    if (identity.contract_version != player_identity_contract_version)
        throw Error(ErrorCode::InvalidArgument, "player identity contract version is unsupported");
    if (identity.game_id.empty() || identity.game_id.size() > 256)
        throw Error(ErrorCode::InvalidArgument, "player identity game id is invalid");
    if (identity.player_name.size() > 128)
        throw Error(ErrorCode::InvalidArgument, "player identity name is too long");
    if (identity.source.empty() || identity.source.size() > 32)
        throw Error(ErrorCode::InvalidArgument, "player identity source is invalid");
    if (identity.source != "pgn" && identity.source != "public_page" &&
        identity.source != "public_api" && identity.source != "profile_archive" &&
        identity.source != "manual")
        throw Error(ErrorCode::InvalidArgument, "player identity source is invalid");
    if (identity.decided_at_ms < 0)
        throw Error(ErrorCode::InvalidArgument, "player identity decision time is invalid");
    if (identity.decision == PlayerIdentityDecision::Confirmed && identity.player_name.empty())
        throw Error(ErrorCode::InvalidArgument, "confirmed player identity requires a name");
}

PlayerIdentity player_identity_from_json(const json::Value& value) {
    PlayerIdentity identity;
    identity.contract_version = value.get("contract_version", "").as_string();
    identity.game_id = value.at("game_id").as_string();
    identity.player_name = value.get("player_name", "").as_string();
    identity.source = value.at("source").as_string();
    identity.decision = player_identity_decision(value.at("decision").as_string());
    identity.decided_at_ms = static_cast<std::int64_t>(value.get("decided_at_ms", 0).as_number());
    validate_player_identity(identity);
    return identity;
}

json::Value to_json(const PlayerIdentity& identity) {
    return json::Value::Object{
        {"contract_version", identity.contract_version},
        {"game_id", identity.game_id},
        {"player_name", identity.player_name},
        {"source", identity.source},
        {"decision", std::string(name(identity.decision))},
        {"decided_at_ms", static_cast<double>(identity.decided_at_ms)},
    };
}

json::Value to_json(const Profile& profile) {
    json::Value::Array weaknesses;
    for (const auto& weakness : profile.weaknesses) {
        json::Value::Object phases;
        for (const auto& [phase, count] : weakness.phases)
            phases.emplace(phase, count);
        weaknesses.emplace_back(json::Value::Object{
            {"category", weakness.category}, {"occurrences", weakness.occurrences},
            {"games", weakness.games}, {"attempts", weakness.attempts},
            {"occurrences_7_days", weakness.occurrences_7_days},
            {"occurrences_30_days", weakness.occurrences_30_days},
            {"correct", weakness.correct}, {"drill_accuracy", weakness.drill_accuracy},
            {"average_loss_cp", weakness.average_loss_cp},
            {"recurrence_rate", weakness.recurrence_rate},
            {"repeated_interval_days", weakness.repeated_interval_days
                                                   ? json::Value(*weakness.repeated_interval_days)
                                                   : json::Value{}},
            {"phases", std::move(phases)}});
    }
    json::Value::Array openings;
    for (const auto& opening : profile.openings)
        openings.emplace_back(json::Value::Object{
            {"eco", opening.eco}, {"name", opening.name}, {"games", opening.games},
            {"mistakes", opening.mistakes},
            {"average_centipawn_loss", opening.average_centipawn_loss}});
    json::Value::Array activity_trend;
    for (const auto& point : profile.activity_trend)
        activity_trend.emplace_back(json::Value::Object{
            {"day_start_ms", static_cast<double>(point.day_start_ms)},
            {"games_analyzed", point.games_analyzed},
            {"mistakes", point.mistakes},
            {"drill_attempts", point.drill_attempts},
            {"drill_correct", point.drill_correct},
        });
    const auto metric_json = [](const Profile::RateMetric& metric) {
        return json::Value::Object{
            {"numerator", metric.numerator}, {"denominator", metric.denominator},
            {"rate", metric.rate ? json::Value(*metric.rate) : json::Value{}},
            {"statistically_meaningful", metric.denominator >= 5}};
    };
    return json::Value::Object{
        {"projection_version", std::string(profile_version)},
        {"player_name", profile.player_name}, {"latest_rating", profile.latest_rating},
        {"player_identity", profile.player_identity ? to_json(*profile.player_identity)
                                                     : json::Value{}},
        {"rating_observations", profile.rating_observations},
        {"games_imported", profile.games_imported},
        {"games_analyzed", profile.games_analyzed},
        {"games_shallow_analyzed", profile.games_shallow_analyzed},
        {"total_mistakes", profile.total_mistakes},
        {"games_analyzed_7_days", profile.games_analyzed_7_days},
        {"games_analyzed_30_days", profile.games_analyzed_30_days},
        {"total_positions", profile.total_positions}, {"drill_attempts", profile.drill_attempts},
        {"drill_correct", profile.drill_correct},
        {"retention_reviews", profile.retention_reviews},
        {"retained_reviews", profile.retained_reviews},
        {"analysis_completion_rate", profile.analysis_completion_rate},
        {"drill_accuracy", profile.drill_accuracy},
        {"retention_rate", profile.retention_rate},
        {"average_centipawn_loss", profile.average_centipawn_loss},
        {"weaknesses", std::move(weaknesses)},
        {"openings", std::move(openings)},
        {"activity_trend", std::move(activity_trend)},
        {"endgame_conversion", metric_json(profile.endgame_conversion)},
        {"king_safety_violations", metric_json(profile.king_safety_violations)},
        {"time_management_failures", metric_json(profile.time_management_failures)}};
}

json::Value to_json(const Recommendation& recommendation) {
    json::Value::Array categories;
    for (const auto& category : recommendation.resource.categories)
        categories.emplace_back(category);
    return json::Value::Object{{"id", recommendation.resource.id},
                               {"title", recommendation.resource.title},
                               {"kind", recommendation.resource.kind},
                               {"locator", recommendation.resource.locator},
                               {"categories", std::move(categories)},
                               {"phase", recommendation.resource.phase},
                               {"prerequisite", recommendation.resource.prerequisite},
                               {"opening", recommendation.resource.opening},
                               {"catalog_version", std::string(catalog_version)},
                               {"evidence", recommendation.evidence},
                               {"priority", recommendation.priority},
                               {"completed", recommendation.completed}};
}

} // namespace pct::training
