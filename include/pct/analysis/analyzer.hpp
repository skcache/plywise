#pragma once

#include "pct/chess/pgn.hpp"
#include "pct/engine/stockfish.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pct::analysis {

inline constexpr std::string_view opening_book_version = "2026.1";
inline constexpr std::string_view classification_model_version =
    "tutor-classification-model-1";
inline constexpr std::string_view expected_points_model_version =
    "tutor-logistic-unrated-1500-v1";
inline constexpr std::string_view accuracy_model_version =
    "tutor-expected-points-squared-v1";

enum class AnalysisStage { Parsing, ShallowScan, DeepAnalysis, Complete };
enum class GamePhase { Opening, Middlegame, Endgame };
enum class MoveQuality {
    Brilliant,
    Great,
    Best,
    Excellent,
    Good,
    Book,
    Inaccuracy,
    Mistake,
    Miss,
    Blunder,
};
enum class ClassificationState { Pending, Provisional, Final };

struct Progress {
    AnalysisStage stage{AnalysisStage::Parsing};
    std::size_t complete{0};
    std::size_t total{0};
    std::string message;
};

struct MoveAssessment {
    // The original fields remain first to preserve source compatibility with existing aggregate
    // initializers. New code should prefer the explicit contract fields below.
    std::size_t ply{0};
    std::string san;
    std::string fen_before;
    std::string fen_after;
    int evaluation_before{0};
    int evaluation_after{0};
    int loss{0};
    int material_delta{0};
    MoveQuality quality{MoveQuality::Good};
    GamePhase phase{GamePhase::Middlegame};
    std::string best_response;

    std::size_t move_number{1};
    std::string side{"white"};
    std::string played_uci;
    std::string played_san;
    std::string best_uci;
    std::string best_san;
    int evaluation_after_best{0};
    double expected_points_before{0.5};
    double expected_points_after{0.5};
    double expected_points_loss{0.0};
    ClassificationState classification_state{ClassificationState::Pending};
    std::vector<std::string> classification_reasons;
    std::vector<std::string> tactical_tags;
    std::vector<std::string> principal_variation;
    std::vector<std::string> acceptable_alternatives;
    std::string book_source;
    std::string book_version;
    int depth{0};
    std::uint64_t nodes{0};
    std::uint64_t time_ms{0};
    int multipv{0};
    std::string engine_version{"stockfish-local-unreported"};
    std::string classification_model_version{
        std::string(::pct::analysis::classification_model_version)};
    std::string expected_points_model_version{
        std::string(::pct::analysis::expected_points_model_version)};
};

struct Mistake {
    std::size_t rank{0};
    std::size_t ply{0};
    std::string san;
    std::string fen;
    int evaluation_before{0};
    int evaluation_after{0};
    int loss{0};
    GamePhase phase{GamePhase::Middlegame};
    std::string category;
    std::string explanation;
    std::string punishment;
    std::vector<std::string> better_moves;
    engine::AnalysisResult engine_details;
    std::vector<std::string> evidence;
    std::string confidence{"proven"};
    std::string classifier_version{"taxonomy-2"};
};

struct GameAnalysis {
    std::string game_id;
    std::vector<MoveAssessment> moves;
    std::vector<Mistake> mistakes;
    std::string eco{"A00"};
    std::string opening{"Uncommon Opening"};
    std::size_t book_ply{0};
    std::optional<std::size_t> departure_ply;
    std::string opening_book_version{std::string(::pct::analysis::opening_book_version)};
    double accuracy{0.0};
    double white_accuracy{0.0};
    double black_accuracy{0.0};
    std::size_t accuracy_sample_size{0};
    std::string accuracy_version{std::string(::pct::analysis::accuracy_model_version)};
};

struct OpeningMatch {
    std::string eco;
    std::string name;
    std::size_t book_ply{0};
    std::optional<std::size_t> departure_ply;
    std::string book_version{std::string(opening_book_version)};
};

struct AnalyzerOptions {
    int shallow_depth{10};
    int deep_depth{18};
    int candidate_threshold_cp{80};
    std::size_t max_deep_candidates{12};
    std::size_t top_mistakes{3};
};

using ProgressCallback = std::function<void(const Progress&)>;

class AnalysisCache {
  public:
    explicit AnalysisCache(std::size_t max_entries = 50000) : max_entries_(max_entries) {}
    [[nodiscard]] bool get(const engine::AnalysisRequest& request,
                           engine::AnalysisResult& result) const;
    void put(const engine::AnalysisRequest& request, engine::AnalysisResult result);
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t hit_count() const;
    [[nodiscard]] std::size_t miss_count() const;
    [[nodiscard]] std::size_t eviction_count() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return max_entries_; }

  private:
    [[nodiscard]] static std::string key(const engine::AnalysisRequest& request);
    mutable std::mutex mutex_;
    std::map<std::string, engine::AnalysisResult> values_;
    std::deque<std::string> insertion_order_;
    std::size_t max_entries_{50000};
    mutable std::size_t hits_{0};
    mutable std::size_t misses_{0};
    std::size_t evictions_{0};
};

class Analyzer {
  public:
    Analyzer(engine::AnalysisEngine& engine, AnalysisCache& cache, AnalyzerOptions options = {});

    [[nodiscard]] GameAnalysis analyze(const chess::Game& game, ProgressCallback progress = {},
                                       CancellationToken stop_token = {},
                                       engine::AnalysisPriority priority =
                                           engine::AnalysisPriority::CurrentGame);
    [[nodiscard]] GameAnalysis analyze_shallow(const chess::Game& game,
                                               ProgressCallback progress = {},
                                               CancellationToken stop_token = {},
                                               engine::AnalysisPriority priority =
                                                   engine::AnalysisPriority::CurrentGame);
    [[nodiscard]] GameAnalysis analyze_deep(const chess::Game& game,
                                            GameAnalysis shallow_analysis,
                                            ProgressCallback progress = {},
                                            CancellationToken stop_token = {},
                                            engine::AnalysisPriority priority =
                                                engine::AnalysisPriority::CurrentGame);
    [[nodiscard]] engine::AnalysisResult analyze_position(
        std::string_view fen, CancellationToken stop_token = {},
        engine::AnalysisPriority priority = engine::AnalysisPriority::Historical);
    [[nodiscard]] static GamePhase classify_phase(const chess::Board& board, std::size_t ply);
    [[nodiscard]] std::size_t cache_hits() const { return cache_.hit_count(); }
    [[nodiscard]] std::size_t cache_misses() const { return cache_.miss_count(); }
    [[nodiscard]] std::size_t cache_evictions() const { return cache_.eviction_count(); }
    [[nodiscard]] std::size_t cache_size() const { return cache_.size(); }
    [[nodiscard]] std::size_t cache_capacity() const { return cache_.capacity(); }

  private:
    engine::AnalysisEngine& engine_;
    AnalysisCache& cache_;
    AnalyzerOptions options_;

    [[nodiscard]] engine::AnalysisResult analyze_cached(const engine::AnalysisRequest& request,
                                                        CancellationToken stop_token);
};

[[nodiscard]] std::string_view name(AnalysisStage stage);
[[nodiscard]] std::string_view name(GamePhase phase);
[[nodiscard]] std::string_view name(MoveQuality quality);
[[nodiscard]] std::string_view name(ClassificationState state);
// Tutor Classification Model 1 uses a neutral, unrated logistic conversion with a 400cp scale
// and clamps engine scores to +/-1000cp before conversion. This is local and is not CAPS2.
[[nodiscard]] double expected_points(int evaluation_cp, chess::Color perspective);
[[nodiscard]] MoveQuality classify_expected_points_loss(double loss);
[[nodiscard]] OpeningMatch recognize_opening(const chess::Game& game);
[[nodiscard]] std::string classify_tactical_motif(chess::Board board,
                                                  const chess::Move& best_move);
[[nodiscard]] std::string
classify_mistake_category(const chess::Game& game, std::size_t ply,
                          const engine::AnalysisResult& best_before,
                          const engine::AnalysisResult& best_after);

// Assemble a review from already validated position evaluations. This is shared by the browser
// observation path and the server analyzer so both execution sources use the same C++ classifiers,
// opening source, tactical tags, explanations, and accuracy contract.
[[nodiscard]] GameAnalysis assemble_observation_review(
    const chess::Game& game, const std::vector<engine::AnalysisResult>& before_results,
    const std::vector<engine::AnalysisResult>& after_results,
    ClassificationState state = ClassificationState::Final, AnalyzerOptions options = {},
    std::string_view engine_version = {});

} // namespace pct::analysis
