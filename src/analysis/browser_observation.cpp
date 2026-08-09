#include "pct/analysis/browser_observation.hpp"

#include "pct/common/error.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace pct::analysis {
namespace {

struct ProfileLimits {
    int max_depth;
    std::uint64_t max_time_ms;
};

constexpr std::size_t max_contract_length = 64;
constexpr std::size_t max_profile_length = 32;
constexpr std::size_t max_engine_field_length = 128;
constexpr std::size_t max_engine_hash_length = 256;

ProfileLimits profile_limits(std::string_view profile) {
    if (profile == "quick")
        return ProfileLimits{10, 15'000};
    if (profile == "balanced")
        return ProfileLimits{14, 30'000};
    throw Error(ErrorCode::InvalidArgument, "browser observation profile is unsupported");
}

bool valid_opaque_id(std::string_view value) {
    if (value.empty() || value.size() > browser_observation_max_id_length)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.';
    });
}

void require_bounded(std::string_view value, std::size_t limit, std::string_view field) {
    if (value.size() > limit)
        throw Error(ErrorCode::InvalidArgument,
                    "browser observation " + std::string(field) + " is too long");
}

bool valid_uci_token(std::string_view value) {
    if (value.size() != 4 && value.size() != 5)
        return false;
    if (value[0] < 'a' || value[0] > 'h' || value[2] < 'a' || value[2] > 'h' ||
        value[1] < '1' || value[1] > '8' || value[3] < '1' || value[3] > '8')
        return false;
    return value.size() == 4 || value[4] == 'q' || value[4] == 'r' || value[4] == 'b' ||
           value[4] == 'n';
}

chess::PieceType promotion_type(char value) {
    switch (value) {
    case 'q': return chess::PieceType::Queen;
    case 'r': return chess::PieceType::Rook;
    case 'b': return chess::PieceType::Bishop;
    case 'n': return chess::PieceType::Knight;
    default: throw Error(ErrorCode::InvalidArgument, "browser observation promotion is invalid");
    }
}

chess::Move require_legal_move(chess::Board& board, std::string_view value) {
    if (!valid_uci_token(value))
        throw Error(ErrorCode::InvalidArgument, "browser observation contains an invalid UCI move");
    const chess::Square from = chess::parse_square(value.substr(0, 2));
    const chess::Square to = chess::parse_square(value.substr(2, 2));
    const chess::PieceType promotion = value.size() == 5 ? promotion_type(value[4])
                                                         : chess::PieceType::Queen;
    const auto move = board.find_legal_move(from, to, promotion);
    if (!move)
        throw Error(ErrorCode::InvalidArgument, "browser observation contains an illegal move");
    return *move;
}

void validate_line(const BrowserObservationLine& line, chess::Board board,
                   std::string_view best_move) {
    if (line.rank != 1 || line.moves.size() > browser_observation_max_pv_length)
        throw Error(ErrorCode::InvalidArgument, "browser observation principal variation is invalid");
    if (line.centipawns.has_value() == line.mate.has_value())
        throw Error(ErrorCode::InvalidArgument, "browser observation score must be cp or mate");
    if (line.centipawns && (*line.centipawns < -1'000'000 || *line.centipawns > 1'000'000))
        throw Error(ErrorCode::InvalidArgument, "browser observation centipawn score is out of bounds");
    if (line.mate && (*line.mate < -1000 || *line.mate > 1000 || *line.mate == 0))
        throw Error(ErrorCode::InvalidArgument, "browser observation mate score is out of bounds");
    if (best_move == "0000") {
        if (!line.moves.empty() || !board.legal_moves().empty())
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation terminal move is only valid without legal replies");
        return;
    }
    if (line.moves.empty() || line.moves.front() != best_move)
        throw Error(ErrorCode::InvalidArgument, "browser observation best move does not match its line");
    for (const auto& value : line.moves) {
        require_bounded(value, 5, "principal variation move");
        board.make_move(require_legal_move(board, value));
    }
}

std::string run_key(std::string_view owner_key, std::string_view run_id) {
    if (owner_key.empty() || owner_key.size() > browser_observation_max_owner_key_length)
        throw Error(ErrorCode::InvalidArgument, "browser observation owner is invalid");
    return std::to_string(owner_key.size()) + ":" + std::string(owner_key) + ":" +
           std::string(run_id);
}

void validate_run_context(const BrowserObservationRunContext& context) {
    if (context.game_id.empty() || context.game_id.size() > browser_observation_max_id_length ||
        context.analysis_run_id.empty() ||
            context.analysis_run_id.size() > browser_observation_max_id_length ||
        !valid_opaque_id(context.game_id) || !valid_opaque_id(context.analysis_run_id))
        throw Error(ErrorCode::InvalidArgument, "browser observation run context is invalid");
    static_cast<void>(profile_limits(context.profile));
    if (context.expected_observations > browser_observation_max_run_observations)
        throw Error(ErrorCode::InvalidArgument,
                    "browser observation run is larger than its bounded contract");
}

engine::AnalysisResult engine_result(const BrowserEngineObservation& observation) {
    const auto& source = observation.lines.front();
    engine::PrincipalVariation line{
        observation.multipv,
        observation.depth,
        source.centipawns,
        source.mate,
        observation.nodes,
        observation.time_ms,
        source.moves,
    };
    return engine::AnalysisResult{{std::move(line)}, observation.best_move, {}};
}

} // namespace

BrowserObservationReceipt
validate_browser_observation(const BrowserObservationContext& context,
                             const BrowserEngineObservation& observation) {
    if (context.game_id.empty() || context.game_id.size() > browser_observation_max_id_length ||
        context.analysis_run_id.empty() ||
        context.analysis_run_id.size() > browser_observation_max_id_length ||
        context.canonical_fen.empty() ||
        context.canonical_fen.size() > browser_observation_max_fen_length)
        throw Error(ErrorCode::InvalidArgument, "browser observation context is invalid");
    if (!valid_opaque_id(context.game_id) || !valid_opaque_id(context.analysis_run_id))
        throw Error(ErrorCode::InvalidArgument, "browser observation context id is invalid");
    require_bounded(observation.contract_version, max_contract_length, "contract version");
    require_bounded(observation.profile, max_profile_length, "profile");
    require_bounded(observation.engine_name, max_engine_field_length, "engine name");
    require_bounded(observation.engine_version, max_engine_field_length, "engine version");
    require_bounded(observation.engine_source, max_engine_field_length, "engine source");
    require_bounded(observation.engine_hash, max_engine_hash_length, "engine hash");
    require_bounded(observation.best_move, 5, "best move");
    if (observation.contract_version != browser_observation_contract_version)
        throw Error(ErrorCode::InvalidArgument, "browser observation contract version is unsupported");
    if (observation.analysis_run_id != context.analysis_run_id ||
        observation.game_id != context.game_id)
        throw Error(ErrorCode::InvalidArgument, "browser observation run or game does not match");
    if (observation.fen != context.canonical_fen)
        throw Error(ErrorCode::InvalidArgument, "browser observation FEN does not match the game");
    if (observation.sequence > browser_observation_max_sequence ||
        observation.ply > browser_observation_max_ply)
        throw Error(ErrorCode::InvalidArgument, "browser observation sequence is out of bounds");
    if (observation.engine_name != browser_engine_name ||
        observation.engine_version != browser_engine_version ||
        observation.engine_source != browser_engine_source ||
        observation.engine_hash != browser_engine_asset_hash)
        throw Error(ErrorCode::InvalidArgument, "browser observation engine identity is unsupported");

    const ProfileLimits limits = profile_limits(context.profile);
    if (observation.profile != context.profile || observation.depth < 1 ||
        observation.depth > limits.max_depth || observation.nodes > browser_observation_max_nodes ||
        observation.time_ms > limits.max_time_ms || observation.multipv != 1)
        throw Error(ErrorCode::InvalidArgument, "browser observation exceeds its profile limits");
    if (observation.best_move.empty() || observation.lines.size() != browser_observation_max_lines)
        throw Error(ErrorCode::InvalidArgument, "browser observation must contain one engine line");

    chess::Board board;
    try {
        board = chess::Board::from_fen(context.canonical_fen);
    } catch (const Error&) {
        throw Error(ErrorCode::InvalidArgument, "browser observation FEN is not a valid position");
    }
    validate_line(observation.lines.front(), board, observation.best_move);
    return BrowserObservationReceipt{BrowserObservationDisposition::Accepted, observation.sequence};
}

GameAnalysis assemble_browser_review(
    const chess::Game& game, const std::vector<BrowserEngineObservation>& observations) {
    if (game.plies.empty())
        throw Error(ErrorCode::InvalidArgument, "cannot assemble an empty browser review");
    if (game.plies.size() > browser_observation_max_run_observations / 2 ||
        observations.size() != game.plies.size() * 2)
        throw Error(ErrorCode::InvalidArgument,
                    "browser observation run is incomplete for the canonical game");

    std::vector<std::optional<engine::AnalysisResult>> before(game.plies.size());
    std::vector<std::optional<engine::AnalysisResult>> after(game.plies.size());
    std::set<std::size_t> sequences;
    std::string run_id;
    std::string profile;
    std::string engine_version;
    for (const auto& observation : observations) {
        const BrowserObservationContext context{
            observation.game_id, observation.analysis_run_id, observation.profile, observation.fen};
        static_cast<void>(validate_browser_observation(context, observation));
        if (observation.game_id != game.identity)
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation game does not match the canonical game");
        if (run_id.empty()) {
            run_id = observation.analysis_run_id;
            profile = observation.profile;
            engine_version = observation.engine_version;
        } else if (observation.analysis_run_id != run_id || observation.profile != profile ||
                   observation.engine_version != engine_version) {
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation run metadata is inconsistent");
        }
        if (!sequences.insert(observation.sequence).second)
            throw Error(ErrorCode::InvalidArgument, "browser observation sequence is duplicated");
        if (observation.ply >= game.plies.size())
            throw Error(ErrorCode::InvalidArgument, "browser observation ply is outside the game");
        const auto& ply = game.plies[observation.ply];
        const bool is_before = observation.fen == ply.fen_before;
        const bool is_after = observation.fen == ply.fen_after;
        if (is_before == is_after)
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation does not identify a canonical position");
        auto& target = is_before ? before[observation.ply] : after[observation.ply];
        if (target.has_value())
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation contains duplicate data for a position");
        target = engine_result(observation);
    }
    for (std::size_t sequence = 0; sequence < observations.size(); ++sequence) {
        if (!sequences.contains(sequence))
            throw Error(ErrorCode::InvalidArgument,
                        "browser observation sequence is incomplete");
    }

    std::vector<engine::AnalysisResult> before_results;
    std::vector<engine::AnalysisResult> after_results;
    before_results.reserve(game.plies.size());
    after_results.reserve(game.plies.size());
    for (std::size_t index = 0; index < game.plies.size(); ++index) {
        if (!before[index] || !after[index])
            throw Error(ErrorCode::InvalidArgument,
                        "browser observations must cover each position before and after the move");
        before_results.push_back(std::move(*before[index]));
        after_results.push_back(std::move(*after[index]));
    }
    return assemble_observation_review(game, before_results, after_results,
                                       ClassificationState::Final, {}, engine_version);
}

BrowserObservationLedger::BrowserObservationLedger(std::size_t max_runs,
                                                   std::size_t max_observations_per_run)
    : max_runs_(max_runs), max_observations_per_run_(max_observations_per_run) {
    if (max_runs_ == 0 || max_observations_per_run_ == 0)
        throw Error(ErrorCode::InvalidArgument, "browser observation ledger bounds must be positive");
}

void BrowserObservationLedger::begin(std::string_view owner_key,
                                     const BrowserObservationRunContext& context) {
    validate_run_context(context);
    const std::string key = run_key(owner_key, context.analysis_run_id);
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(key);
    if (found != runs_.end()) {
        if (found->second.context.game_id != context.game_id ||
            found->second.context.profile != context.profile)
            throw Error(ErrorCode::InvalidArgument, "browser observation run context changed");
        if (found->second.context.expected_observations != 0 &&
            context.expected_observations != 0 &&
            found->second.context.expected_observations != context.expected_observations)
            throw Error(ErrorCode::InvalidArgument, "browser observation run length changed");
        if (found->second.context.expected_observations == 0)
            found->second.context.expected_observations = context.expected_observations;
        return;
    }
    if (runs_.size() >= max_runs_)
        throw Error(ErrorCode::InvalidArgument, "browser observation run capacity reached");
    runs_.emplace(key, RunState{context, 0, {}, false});
}

BrowserObservationReceipt BrowserObservationLedger::submit(
    std::string_view owner_key, const BrowserObservationContext& context,
    const BrowserEngineObservation& observation) {
    static_cast<void>(validate_browser_observation(context, observation));
    const std::string key = run_key(owner_key, context.analysis_run_id);
    std::lock_guard lock(mutex_);
    auto found = runs_.find(key);
    if (found == runs_.end())
        throw Error(ErrorCode::InvalidArgument, "browser observation run is not registered");
    RunState& state = found->second;
    if (state.context.game_id != context.game_id || state.context.profile != context.profile ||
        state.context.analysis_run_id != context.analysis_run_id)
        throw Error(ErrorCode::InvalidArgument, "browser observation run context changed");
    const auto observation_key = std::make_pair(observation.ply, observation.sequence);
    if (const auto existing = state.observations.find(observation_key);
        existing != state.observations.end()) {
        if (existing->second == observation)
            return BrowserObservationReceipt{BrowserObservationDisposition::Duplicate,
                                             observation.sequence};
        throw Error(ErrorCode::InvalidArgument, "browser observation replay conflicts with prior data");
    }
    if (state.finalized)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is already finalized");
    if (state.context.expected_observations != 0 &&
        state.observations.size() >= state.context.expected_observations)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is already complete");
    if (state.observations.size() >= max_observations_per_run_)
        throw Error(ErrorCode::InvalidArgument, "browser observation run capacity reached");
    if (observation.sequence != state.next_sequence)
        throw Error(ErrorCode::InvalidArgument, "browser observation sequence is stale or incomplete");
    state.observations.emplace(observation_key, observation);
    ++state.next_sequence;
    return BrowserObservationReceipt{BrowserObservationDisposition::Accepted, observation.sequence};
}

BrowserObservationBundle BrowserObservationLedger::finalize(std::string_view owner_key,
                                                            std::string_view game_id,
                                                            std::string_view analysis_run_id) {
    const std::string key = run_key(owner_key, analysis_run_id);
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(key);
    if (found == runs_.end())
        throw Error(ErrorCode::InvalidArgument, "browser observation run is not registered");
    RunState& state = found->second;
    if (state.context.game_id != game_id)
        throw Error(ErrorCode::InvalidArgument, "browser observation game does not match the run");
    if (state.context.expected_observations == 0)
        throw Error(ErrorCode::InvalidArgument,
                    "browser observation run has no expected position count");
    if (state.observations.size() != state.context.expected_observations)
        throw Error(ErrorCode::InvalidArgument, "browser observation run is incomplete");
    state.finalized = true;
    BrowserObservationBundle result;
    result.context = state.context;
    result.observations.reserve(state.observations.size());
    for (const auto& [_, observation] : state.observations)
        result.observations.push_back(observation);
    return result;
}

std::size_t BrowserObservationLedger::run_count() const {
    std::lock_guard lock(mutex_);
    return runs_.size();
}

std::size_t BrowserObservationLedger::observation_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [_, run] : runs_)
        count += run.observations.size();
    return count;
}

} // namespace pct::analysis
