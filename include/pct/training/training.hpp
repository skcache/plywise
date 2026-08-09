#pragma once

#include "pct/analysis/analyzer.hpp"
#include "pct/common/json.hpp"
#include "pct/engine/stockfish.hpp"

#include <cstdint>
#include <map>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pct::training {

inline constexpr std::string_view scheduler_version = "pct-sm2-1";
inline constexpr std::string_view profile_version = "profile-1";
inline constexpr std::string_view catalog_version = "2026.1";
inline constexpr std::string_view player_identity_contract_version = "player-identity-1";

enum class DrillState { New, Due, Upcoming, Mastered };

struct DrillAttempt {
    std::uint64_t id{0};
    std::int64_t attempted_at_ms{0};
    bool correct{false};
    std::string move;
    std::uint64_t response_time_ms{0};
    int hint_level{0};
    std::size_t retries{0};
};

struct Drill {
    std::string id;
    std::string source_game_id;
    std::size_t source_ply{0};
    std::string fen;
    std::string category;
    std::string phase;
    std::string explanation;
    std::string punishment;
    std::vector<std::string> solutions;
    int difficulty{1};
    int impact_cp{0};
    std::int64_t created_at_ms{0};
    std::vector<DrillAttempt> attempts;
    std::string played_move;
    std::string fen_after_mistake;
    std::string fen_after_punishment;
    int session_hint_level{0};
    std::int64_t session_started_at_ms{0};
    std::string changed_threat;
    std::vector<std::string> attacked_pieces;
    std::string opponent_response;
    std::string source_type{"personal_game"};
    std::string provenance;
    std::string corpus_version;
    std::vector<std::string> validation_evidence;
};

struct TacticalCorpusManifest {
    std::string id;
    std::string version;
    std::string source_url;
    std::string download_url;
    std::string license;
};

struct TacticalPuzzle {
    std::string id;
    std::string fen;
    std::string solution;
    std::vector<std::string> motifs;
    int rating{0};
};

struct Profile;

class TacticalCorpus {
  public:
    [[nodiscard]] static TacticalCorpus load(const std::filesystem::path& path);
    [[nodiscard]] const TacticalCorpusManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::vector<TacticalPuzzle>& puzzles() const noexcept { return puzzles_; }
    [[nodiscard]] std::vector<TacticalPuzzle> match(const Profile& profile,
                                                    std::size_t limit) const;

  private:
    TacticalCorpusManifest manifest_;
    std::vector<TacticalPuzzle> puzzles_;
};

using ValidationEngineFactory = std::function<std::unique_ptr<engine::AnalysisEngine>()>;

class AdvancedDrillGenerator {
  public:
    AdvancedDrillGenerator(TacticalCorpus corpus, ValidationEngineFactory verifier_factory);

    [[nodiscard]] std::vector<Drill> generate(const Profile& profile,
                                              const std::vector<Drill>& personal_drills,
                                              std::size_t limit = 5) const;

  private:
    TacticalCorpus corpus_;
    ValidationEngineFactory verifier_factory_;
};

struct Schedule {
    DrillState state{DrillState::New};
    std::int64_t last_review_ms{0};
    std::int64_t next_review_ms{0};
    std::size_t correct{0};
    std::size_t total{0};
    double success_rate{0.0};
    double retention{0.0};
    int interval_days{0};
    int priority{0};
};

struct Weakness {
    std::string category;
    std::size_t occurrences{0};
    std::size_t games{0};
    std::size_t attempts{0};
    std::size_t correct{0};
    std::size_t occurrences_7_days{0};
    std::size_t occurrences_30_days{0};
    double drill_accuracy{0.0};
    double average_loss_cp{0.0};
    double recurrence_rate{0.0};
    std::optional<double> repeated_interval_days;
    std::map<std::string, std::size_t> phases;
};

enum class PlayerIdentityDecision { Confirmed, Declined, Uncertain };

struct PlayerIdentity {
    std::string contract_version{std::string(player_identity_contract_version)};
    std::string game_id;
    std::string player_name;
    std::string source;
    PlayerIdentityDecision decision{PlayerIdentityDecision::Uncertain};
    std::int64_t decided_at_ms{0};
    bool operator==(const PlayerIdentity&) const = default;
};

struct Profile {
    struct RateMetric {
        std::size_t numerator{0};
        std::size_t denominator{0};
        std::optional<double> rate;
    };
    struct TrendPoint {
        std::int64_t day_start_ms{0};
        std::size_t games_analyzed{0};
        std::size_t mistakes{0};
        std::size_t drill_attempts{0};
        std::size_t drill_correct{0};
    };
    std::string player_name;
    std::optional<PlayerIdentity> player_identity;
    int latest_rating{0};
    std::size_t rating_observations{0};
    std::size_t games_imported{0};
    std::size_t games_analyzed{0};
    std::size_t games_shallow_analyzed{0};
    std::size_t total_mistakes{0};
    std::size_t total_positions{0};
    std::size_t drill_attempts{0};
    std::size_t drill_correct{0};
    std::size_t retention_reviews{0};
    std::size_t retained_reviews{0};
    double analysis_completion_rate{0.0};
    double drill_accuracy{0.0};
    double retention_rate{0.0};
    double average_centipawn_loss{0.0};
    std::size_t games_analyzed_7_days{0};
    std::size_t games_analyzed_30_days{0};
    std::vector<Weakness> weaknesses;
    struct OpeningPerformance {
        std::string eco;
        std::string name;
        std::size_t games{0};
        std::size_t mistakes{0};
        double average_centipawn_loss{0.0};
    };
    std::vector<OpeningPerformance> openings;
    std::vector<TrendPoint> activity_trend;
    RateMetric endgame_conversion;
    RateMetric king_safety_violations;
    RateMetric time_management_failures;
};

struct Resource {
    std::string id;
    std::string title;
    std::string kind;
    std::string locator;
    std::vector<std::string> categories;
    std::string phase;
    int minimum_rating{0};
    int maximum_rating{3000};
    std::string prerequisite;
    std::string opening;
};

struct Recommendation {
    Resource resource;
    std::string evidence;
    int priority{0};
    bool completed{false};
};

[[nodiscard]] Schedule schedule(const Drill& drill, std::int64_t now_ms,
                                std::size_t category_frequency = 1);
[[nodiscard]] int next_hint_level(const Drill& drill);
[[nodiscard]] int available_hint_level(const Drill& drill);
[[nodiscard]] std::vector<Drill> review_queue(std::vector<Drill> drills, std::int64_t now_ms);
[[nodiscard]] std::vector<Resource> default_catalog();
[[nodiscard]] std::vector<Recommendation>
recommend(const Profile& profile, const std::vector<Resource>& catalog,
          const std::map<std::string, std::int64_t>& completions,
          std::int64_t now_ms = 0);
[[nodiscard]] std::string_view name(DrillState state);
[[nodiscard]] std::string_view name(PlayerIdentityDecision decision);
[[nodiscard]] PlayerIdentityDecision player_identity_decision(std::string_view value);
void validate_player_identity(const PlayerIdentity& identity);
[[nodiscard]] PlayerIdentity player_identity_from_json(const json::Value& value);
[[nodiscard]] json::Value to_json(const PlayerIdentity& identity);
[[nodiscard]] json::Value to_json(const DrillAttempt& attempt);
[[nodiscard]] json::Value to_json(const Drill& drill, std::int64_t now_ms,
                                  std::size_t category_frequency = 1);
[[nodiscard]] json::Value to_json(const Profile& profile);
[[nodiscard]] json::Value to_json(const Recommendation& recommendation);

} // namespace pct::training
