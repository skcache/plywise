#include "pct/app/repository.hpp"

#include "pct/common/error.hpp"
#include "pct/common/log.hpp"
#include "pct/chess/san.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

namespace pct::app {
namespace {

std::string method_name(import::ImportMethod method) {
    switch (method) {
    case import::ImportMethod::PublicApi:
        return "public_api";
    case import::ImportMethod::PublicPage:
        return "public_page";
    case import::ImportMethod::ManualPgn:
        return "manual_pgn";
    }
    return "manual_pgn";
}

import::ImportMethod parse_method(std::string_view method) {
    if (method == "public_api")
        return import::ImportMethod::PublicApi;
    if (method == "public_page")
        return import::ImportMethod::PublicPage;
    return import::ImportMethod::ManualPgn;
}

std::optional<chess::Move> parse_uci(chess::Board& board, std::string_view uci) {
    if (uci.size() != 4 && uci.size() != 5)
        return std::nullopt;
    chess::PieceType promotion = chess::PieceType::Queen;
    if (uci.size() >= 5 && uci[4] == 'n') promotion = chess::PieceType::Knight;
    if (uci.size() >= 5 && uci[4] == 'b') promotion = chess::PieceType::Bishop;
    if (uci.size() >= 5 && uci[4] == 'r') promotion = chess::PieceType::Rook;
    try {
        return board.find_legal_move(chess::parse_square(uci.substr(0, 2)),
                                     chess::parse_square(uci.substr(2, 2)), promotion);
    } catch (const Error&) {
        return std::nullopt;
    }
}

Variation variation_from_json(const json::Value& value) {
    Variation variation;
    variation.id = value.at("id").as_string();
    variation.game_id = value.at("game_id").as_string();
    variation.root_ply = value.at("root_ply").as_size();
    variation.root_position = value.at("root_position").as_string();
    variation.root_fen = value.at("root_fen").as_string();
    variation.current_node_id = static_cast<std::uint64_t>(value.at("current_node_id").as_number());
    variation.next_node_id = static_cast<std::uint64_t>(value.at("next_node_id").as_number());
    variation.engine_configuration = value.at("engine_configuration").as_string();
    variation.updated_at_ms = static_cast<std::int64_t>(value.at("updated_at_ms").as_number());
    for (const auto& encoded : value.at("nodes").as_array()) {
        VariationNode node;
        node.id = static_cast<std::uint64_t>(encoded.at("id").as_number());
        node.parent_id = static_cast<std::int64_t>(encoded.at("parent_id").as_number());
        node.uci = encoded.at("uci").as_string();
        node.san = encoded.at("san").as_string();
        node.fen = encoded.at("fen").as_string();
        for (const auto& child : encoded.at("children").as_array())
            node.children.push_back(static_cast<std::uint64_t>(child.as_number()));
        variation.nodes.emplace(node.id, std::move(node));
    }
    if (variation.id.empty() || variation.game_id.empty() || variation.nodes.empty() ||
        !variation.nodes.contains(0) || !variation.nodes.contains(variation.current_node_id))
        throw Error(ErrorCode::Corruption, "persisted variation is incomplete");
    static_cast<void>(chess::Board::from_fen(variation.root_fen));
    return variation;
}

std::string piece_name(chess::PieceType type) {
    switch (type) {
    case chess::PieceType::Pawn: return "pawn";
    case chess::PieceType::Knight: return "knight";
    case chess::PieceType::Bishop: return "bishop";
    case chess::PieceType::Rook: return "rook";
    case chess::PieceType::Queen: return "queen";
    case chess::PieceType::King: return "king";
    case chess::PieceType::None: return "piece";
    }
    return "piece";
}

std::vector<std::string> attacked_piece_descriptions(const chess::Board& board,
                                                     chess::Color owner) {
    std::vector<std::string> result;
    for (chess::Square square = 0; square < 64; ++square) {
        const chess::Piece piece = board.at(square);
        if (piece.empty() || piece.color != owner ||
            !board.is_square_attacked(square, chess::opposite(owner)))
            continue;
        result.push_back(piece_name(piece.type) + " on " + chess::square_name(square));
    }
    return result;
}

json::Value line_json(const engine::PrincipalVariation& line) {
    json::Value::Array moves;
    for (const std::string& move : line.moves)
        moves.emplace_back(move);
    json::Value::Object value{
        {"multipv", line.multipv},
        {"depth", line.depth},
        {"nodes", static_cast<double>(line.nodes)},
        {"time_ms", static_cast<double>(line.time_ms)},
        {"moves", std::move(moves)},
    };
    value.emplace("centipawns", line.centipawns ? json::Value(*line.centipawns) : json::Value{});
    value.emplace("mate", line.mate ? json::Value(*line.mate) : json::Value{});
    return value;
}

json::Value engine_json(const engine::AnalysisResult& result) {
    json::Value::Array lines;
    for (const auto& line : result.lines)
        lines.push_back(line_json(line));
    return json::Value::Object{
        {"best_move", result.best_move},
        {"ponder_move", result.ponder_move},
        {"lines", std::move(lines)},
    };
}

engine::AnalysisResult engine_from_json(const json::Value& value) {
    engine::AnalysisResult result;
    result.best_move = value.at("best_move").as_string();
    result.ponder_move = value.at("ponder_move").as_string();
    for (const auto& item : value.at("lines").as_array()) {
        engine::PrincipalVariation line;
        line.multipv = item.at("multipv").as_int();
        line.depth = item.at("depth").as_int();
        if (!item.at("centipawns").is_null())
            line.centipawns = item.at("centipawns").as_int();
        if (!item.at("mate").is_null())
            line.mate = item.at("mate").as_int();
        line.nodes = static_cast<std::uint64_t>(item.at("nodes").as_number());
        line.time_ms = static_cast<std::uint64_t>(item.at("time_ms").as_number());
        for (const auto& move : item.at("moves").as_array())
            line.moves.push_back(move.as_string());
        result.lines.push_back(std::move(line));
    }
    return result;
}

json::Value move_json(const analysis::MoveAssessment& move) {
    json::Value::Array reasons;
    for (const auto& reason : move.classification_reasons)
        reasons.emplace_back(reason);
    json::Value::Array tactical_tags;
    for (const auto& tag : move.tactical_tags)
        tactical_tags.emplace_back(tag);
    json::Value::Array principal_variation;
    for (const auto& uci : move.principal_variation)
        principal_variation.emplace_back(uci);
    json::Value::Array alternatives;
    for (const auto& uci : move.acceptable_alternatives)
        alternatives.emplace_back(uci);
    return json::Value::Object{
        {"ply", move.ply},
        {"move_number", move.move_number},
        {"side", move.side},
        {"san", move.san},
        {"played_uci", move.played_uci},
        {"played_san", move.played_san},
        {"fen_before", move.fen_before},
        {"fen_after", move.fen_after},
        {"best_uci", move.best_uci},
        {"best_san", move.best_san},
        {"evaluation_before", move.evaluation_before},
        {"evaluation_after", move.evaluation_after},
        {"evaluation_after_best", move.evaluation_after_best},
        {"expected_points_before", move.expected_points_before},
        {"expected_points_after", move.expected_points_after},
        {"expected_points_loss", move.expected_points_loss},
        {"loss", move.loss},
        {"material_delta", move.material_delta},
        {"quality", std::string(analysis::name(move.quality))},
        {"classification", [&] {
             std::string label(analysis::name(move.quality));
             if (!label.empty())
                 label.front() = static_cast<char>(std::toupper(
                     static_cast<unsigned char>(label.front())));
             return label;
         }()},
        {"classification_state", std::string(analysis::name(move.classification_state))},
        {"classification_reasons", std::move(reasons)},
        {"tactical_tags", std::move(tactical_tags)},
        {"principal_variation", std::move(principal_variation)},
        {"acceptable_alternatives", std::move(alternatives)},
        {"book_source", move.book_source},
        {"book_version", move.book_version},
        {"depth", move.depth},
        {"nodes", static_cast<double>(move.nodes)},
        {"time_ms", static_cast<double>(move.time_ms)},
        {"multipv", move.multipv},
        {"engine_version", move.engine_version},
        {"classification_model_version", move.classification_model_version},
        {"expected_points_model_version", move.expected_points_model_version},
        {"phase", std::string(analysis::name(move.phase))},
        {"best_response", move.best_response},
    };
}

analysis::GamePhase parse_phase(std::string_view value) {
    if (value == "opening")
        return analysis::GamePhase::Opening;
    if (value == "endgame")
        return analysis::GamePhase::Endgame;
    return analysis::GamePhase::Middlegame;
}

analysis::MoveQuality parse_quality(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    value = normalized;
    if (value == "brilliant")
        return analysis::MoveQuality::Brilliant;
    if (value == "great")
        return analysis::MoveQuality::Great;
    if (value == "best")
        return analysis::MoveQuality::Best;
    if (value == "excellent")
        return analysis::MoveQuality::Excellent;
    if (value == "good")
        return analysis::MoveQuality::Good;
    if (value == "book")
        return analysis::MoveQuality::Book;
    if (value == "inaccuracy")
        return analysis::MoveQuality::Inaccuracy;
    if (value == "mistake")
        return analysis::MoveQuality::Mistake;
    if (value == "miss")
        return analysis::MoveQuality::Miss;
    if (value == "blunder")
        return analysis::MoveQuality::Blunder;
    // Legacy tactical/neutral labels were not quality judgments. They migrate to Good and are
    // retained as tactical tags by move_from_json.
    return analysis::MoveQuality::Good;
}

analysis::ClassificationState parse_classification_state(std::string_view value) {
    if (value == "pending")
        return analysis::ClassificationState::Pending;
    if (value == "provisional")
        return analysis::ClassificationState::Provisional;
    return analysis::ClassificationState::Final;
}

std::vector<std::string> string_array(const json::Value& value, std::string_view key) {
    const json::Value empty{json::Value::Array{}};
    std::vector<std::string> result;
    for (const auto& item : value.get(key, empty).as_array())
        result.push_back(item.as_string());
    return result;
}

analysis::MoveAssessment move_from_json(const json::Value& value) {
    analysis::MoveAssessment move;
    move.ply = value.at("ply").as_size();
    move.move_number = value.get("move_number", move.ply / 2 + 1).as_size();
    move.side = value.get("side", move.ply % 2 == 0 ? "white" : "black").as_string();
    move.san = value.get("san", value.get("played_san", "")).as_string();
    move.played_san = value.get("played_san", move.san).as_string();
    move.played_uci = value.get("played_uci", "").as_string();
    move.fen_before = value.at("fen_before").as_string();
    move.fen_after = value.at("fen_after").as_string();
    move.best_uci = value.get("best_uci", "").as_string();
    move.best_san = value.get("best_san", "").as_string();
    move.evaluation_before = value.at("evaluation_before").as_int();
    move.evaluation_after = value.at("evaluation_after").as_int();
    move.evaluation_after_best =
        value.get("evaluation_after_best", move.evaluation_before).as_int();
    const chess::Color perspective =
        move.side == "black" ? chess::Color::Black : chess::Color::White;
    move.expected_points_before = value.get(
        "expected_points_before", analysis::expected_points(move.evaluation_before, perspective))
                                      .as_number();
    move.expected_points_after = value.get(
        "expected_points_after", analysis::expected_points(move.evaluation_after, perspective))
                                     .as_number();
    move.expected_points_loss = value.get(
        "expected_points_loss",
        std::max(0.0, move.expected_points_before - move.expected_points_after))
                                    .as_number();
    move.loss = value.get("loss", 0).as_int();
    move.material_delta = value.get("material_delta", 0).as_int();
    const std::string quality =
        value.get("classification", value.get("quality", "good")).as_string();
    move.quality = parse_quality(quality);
    move.classification_state = parse_classification_state(
        value.get("classification_state", "final").as_string());
    move.classification_reasons = string_array(value, "classification_reasons");
    move.tactical_tags = string_array(value, "tactical_tags");
    if (quality == "developing" || quality == "capture" || quality == "check" ||
        quality == "recapture" || quality == "threat") {
        move.tactical_tags.push_back(quality == "developing" ? "development" : quality);
        move.classification_reasons.push_back(
            "legacy tactical label migrated to a secondary tag; quality defaults to good");
    } else if (quality == "neutral") {
        move.classification_reasons.push_back(
            "legacy neutral label migrated to good because it was not an outcome classification");
    }
    move.principal_variation = string_array(value, "principal_variation");
    move.acceptable_alternatives = string_array(value, "acceptable_alternatives");
    move.book_source = value.get("book_source", "").as_string();
    move.book_version = value.get("book_version", "").as_string();
    move.depth = value.get("depth", 0).as_int();
    move.nodes = static_cast<std::uint64_t>(value.get("nodes", 0).as_number());
    move.time_ms = static_cast<std::uint64_t>(value.get("time_ms", 0).as_number());
    move.multipv = value.get("multipv", 0).as_int();
    move.engine_version = value.get("engine_version", "legacy-unreported").as_string();
    move.classification_model_version =
        value.get("classification_model_version", "legacy-fixed-cp-thresholds").as_string();
    move.expected_points_model_version =
        value.get("expected_points_model_version", "legacy-derived-on-read").as_string();
    move.phase = parse_phase(value.get("phase", "middlegame").as_string());
    move.best_response = value.get("best_response", "").as_string();
    return move;
}

json::Value mistake_json(const analysis::Mistake& mistake) {
    json::Value::Array better;
    for (const auto& move : mistake.better_moves)
        better.emplace_back(move);
    json::Value::Array evidence;
    for (const auto& item : mistake.evidence)
        evidence.emplace_back(item);
    return json::Value::Object{
        {"rank", mistake.rank},
        {"ply", mistake.ply},
        {"san", mistake.san},
        {"fen", mistake.fen},
        {"evaluation_before", mistake.evaluation_before},
        {"evaluation_after", mistake.evaluation_after},
        {"loss", mistake.loss},
        {"phase", std::string(analysis::name(mistake.phase))},
        {"category", mistake.category},
        {"explanation", mistake.explanation},
        {"punishment", mistake.punishment},
        {"better_moves", std::move(better)},
        {"engine", engine_json(mistake.engine_details)},
        {"evidence", std::move(evidence)},
        {"confidence", mistake.confidence},
        {"classifier_version", mistake.classifier_version},
    };
}

analysis::Mistake mistake_from_json(const json::Value& value) {
    analysis::Mistake mistake;
    mistake.rank = value.at("rank").as_size();
    mistake.ply = value.at("ply").as_size();
    mistake.san = value.at("san").as_string();
    mistake.fen = value.at("fen").as_string();
    mistake.evaluation_before = value.at("evaluation_before").as_int();
    mistake.evaluation_after = value.at("evaluation_after").as_int();
    mistake.loss = value.at("loss").as_int();
    mistake.phase = parse_phase(value.at("phase").as_string());
    mistake.category = value.at("category").as_string();
    mistake.explanation = value.at("explanation").as_string();
    mistake.punishment = value.at("punishment").as_string();
    for (const auto& move : value.at("better_moves").as_array()) {
        mistake.better_moves.push_back(move.as_string());
    }
    mistake.engine_details = engine_from_json(value.at("engine"));
    for (const auto& item : value.get("evidence", json::Value::Array{}).as_array())
        mistake.evidence.push_back(item.as_string());
    mistake.confidence = value.get("confidence", "proven").as_string();
    mistake.classifier_version = value.get("classifier_version", "taxonomy-1").as_string();
    return mistake;
}

training::Drill drill_from_json(const json::Value& payload) {
    training::Drill drill;
    drill.id = payload.at("id").as_string();
    drill.source_game_id = payload.at("source_game_id").as_string();
    drill.source_ply = payload.at("source_ply").as_size();
    drill.fen = payload.at("fen").as_string();
    drill.category = payload.at("category").as_string();
    drill.phase = payload.at("phase").as_string();
    drill.explanation = payload.at("explanation").as_string();
    drill.punishment = payload.at("punishment").as_string();
    drill.difficulty = payload.at("difficulty").as_int();
    drill.impact_cp = payload.at("impact_cp").as_int();
    drill.created_at_ms = static_cast<std::int64_t>(payload.at("created_at_ms").as_number());
    for (const auto& move : payload.at("solutions").as_array())
        drill.solutions.push_back(move.as_string());
    const json::Value empty_attempts{json::Value::Array{}};
    for (const auto& item : payload.get("attempts", empty_attempts).as_array()) {
        drill.attempts.push_back(training::DrillAttempt{
            static_cast<std::uint64_t>(item.at("id").as_number()),
            static_cast<std::int64_t>(item.at("attempted_at_ms").as_number()),
            item.at("correct").as_bool(), item.at("move").as_string(),
            static_cast<std::uint64_t>(item.at("response_time_ms").as_number()),
            item.at("hint_level").as_int(), item.at("retries").as_size()});
    }
    drill.played_move = payload.get("played_move", "").as_string();
    drill.fen_after_mistake = payload.get("fen_after_mistake", drill.fen).as_string();
    drill.fen_after_punishment =
        payload.get("fen_after_punishment", drill.fen_after_mistake).as_string();
    drill.session_hint_level = payload.get("session_hint_level", 0).as_int();
    drill.session_started_at_ms = static_cast<std::int64_t>(
        payload.get("session_started_at_ms", 0).as_number());
    drill.changed_threat =
        payload.get("changed_threat", "The opponent's strongest reply is " + drill.punishment + ".")
            .as_string();
    const json::Value empty_pieces{json::Value::Array{}};
    for (const auto& piece : payload.get("attacked_pieces", empty_pieces).as_array())
        drill.attacked_pieces.push_back(piece.as_string());
    drill.opponent_response = payload.get("opponent_response", drill.punishment).as_string();
    drill.source_type = payload.get("source_type", "personal_game").as_string();
    drill.provenance = payload.get("provenance", "").as_string();
    drill.corpus_version = payload.get("corpus_version", "").as_string();
    const json::Value empty_evidence{json::Value::Array{}};
    for (const auto& evidence : payload.get("validation_evidence", empty_evidence).as_array())
        drill.validation_evidence.push_back(evidence.as_string());
    return drill;
}

json::Value imported_event(const import::ImportedGame& imported) {
    return json::Value::Object{
        {"game_id", imported.game.identity},
        {"source_url", imported.source_url},
        {"pgn", imported.pgn},
        {"method", method_name(imported.method)},
    };
}

void write_index(const std::filesystem::path& path, const json::Value& value) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const std::string encoded = json::dump(value);
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0)
        throw Error(ErrorCode::IoError, "failed to create derived index " + path.string());
    try {
        std::size_t offset = 0;
        while (offset < encoded.size()) {
            const ssize_t written =
                write(descriptor, encoded.data() + offset, encoded.size() - offset);
            if (written < 0)
                throw Error(ErrorCode::IoError, "failed to write derived index " + path.string());
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(descriptor) != 0)
            throw Error(ErrorCode::IoError, "failed to sync derived index " + path.string());
        close(descriptor);
    } catch (...) {
        close(descriptor);
        std::filesystem::remove(temporary);
        throw;
    }
    std::filesystem::rename(temporary, path);
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
}

std::string trim_ascii(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string sanitize_error(std::string_view error) {
    std::string result;
    result.reserve(std::min<std::size_t>(error.size(), chesscom_profile_error_limit * 2));
    bool previous_space = false;
    for (const char raw_character : error) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (result.size() >= chesscom_profile_error_limit * 2)
            break;
        const bool space = std::iscntrl(character) || std::isspace(character);
        if (space) {
            if (!result.empty() && !previous_space)
                result.push_back(' ');
        } else {
            result.push_back(static_cast<char>(character));
        }
        previous_space = space;
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    static const std::regex query_secret(R"(([?&][^=&\s?#]+)=[^&\s#]+)");
    static const std::regex named_secret(
        R"(((authorization|cookie|set-cookie|token|access[_-]?token|refresh[_-]?token|api[_-]?key|password|secret)\s*[:=]\s*)[^\s,;]+)",
        std::regex_constants::icase);
    static const std::regex bearer_secret(R"((bearer\s+)[^\s,;]+)",
                                          std::regex_constants::icase);
    result = std::regex_replace(result, bearer_secret, "$1[redacted]");
    result = std::regex_replace(result, query_secret, "$1=[redacted]");
    result = std::regex_replace(result, named_secret, "$1[redacted]");
    if (result.size() > chesscom_profile_error_limit)
        result.resize(chesscom_profile_error_limit);
    return result;
}

std::string validate_cursor(std::string_view cursor) {
    std::string result = trim_ascii(cursor);
    if (result.size() > chesscom_profile_cursor_limit)
        throw Error(ErrorCode::InvalidArgument, "Chess.com archive cursor is too long");
    for (const char raw_character : result) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character < 0x21U || character > 0x7eU)
            throw Error(ErrorCode::InvalidArgument, "Chess.com archive cursor is invalid");
    }
    return result;
}

void create_private_directory(const std::filesystem::path& path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        throw Error(ErrorCode::IoError, "failed to create private repository directory");
}

json::Value chesscom_profile_json(const ChessComProfile& profile) {
    json::Value::Array controls;
    for (const auto& control : profile.selected_time_controls)
        controls.emplace_back(control);
    return json::Value::Object{
        {"original_username", profile.original_username},
        {"normalized_username", profile.normalized_username},
        {"selected_time_controls", std::move(controls)},
        {"archive_cursor", profile.archive_cursor},
        {"last_successful_sync_ms", static_cast<double>(profile.last_successful_sync_ms)},
        {"last_error", profile.last_error},
    };
}

ChessComProfile chesscom_profile_from_json(const json::Value& value) {
    ChessComProfile profile;
    profile.original_username = value.at("original_username").as_string();
    profile.normalized_username = value.at("normalized_username").as_string();
    for (const auto& item : value.at("selected_time_controls").as_array())
        profile.selected_time_controls.push_back(item.as_string());
    profile.original_username = trim_ascii(profile.original_username);
    if (profile.normalized_username != normalize_chesscom_username(profile.original_username))
        throw Error(ErrorCode::InvalidArgument, "stored Chess.com username is invalid");
    profile.archive_cursor = validate_cursor(value.get("archive_cursor", "").as_string());
    profile.last_successful_sync_ms = static_cast<std::int64_t>(
        value.get("last_successful_sync_ms", 0).as_number());
    if (profile.last_successful_sync_ms < 0)
        throw Error(ErrorCode::InvalidArgument, "stored Chess.com sync time is invalid");
    profile.last_error = sanitize_error(value.get("last_error", "").as_string());
    return profile;
}

json::Value chesscom_archive_json(const ChessComArchiveEntry& entry) {
    return json::Value::Object{
        {"game_id", entry.game_id}, {"canonical_url", entry.canonical_url},
        {"pgn", entry.pgn}, {"username", entry.username}, {"month", entry.month},
        {"time_class", entry.time_class}, {"end_time_ms", static_cast<double>(entry.end_time_ms)},
        {"fetched_at_ms", static_cast<double>(entry.fetched_at_ms)},
        {"source_url", entry.source_url},
    };
}

ChessComArchiveEntry chesscom_archive_from_json(const json::Value& value) {
    return ChessComArchiveEntry{
        value.at("game_id").as_string(), value.at("canonical_url").as_string(),
        value.at("pgn").as_string(), value.at("username").as_string(),
        value.at("month").as_string(), value.at("time_class").as_string(),
        static_cast<std::int64_t>(value.at("end_time_ms").as_number()),
        static_cast<std::int64_t>(value.at("fetched_at_ms").as_number()),
        value.at("source_url").as_string()};
}

json::Value checkpoint_json(const ChessComMonthCheckpoint& checkpoint) {
    return json::Value::Object{
        {"username", checkpoint.username}, {"month", checkpoint.month},
        {"source_url", checkpoint.source_url}, {"indexed_games", checkpoint.indexed_games},
        {"completed_at_ms", static_cast<double>(checkpoint.completed_at_ms)},
    };
}

ChessComMonthCheckpoint checkpoint_from_json(const json::Value& value) {
    return ChessComMonthCheckpoint{
        value.at("username").as_string(), value.at("month").as_string(),
        value.get("source_url", "").as_string(), value.at("indexed_games").as_size(),
        static_cast<std::int64_t>(value.at("completed_at_ms").as_number())};
}

json::Value sync_state_json(const ChessComSyncState& state) {
    return json::Value::Object{
        {"status", state.status}, {"username", state.username}, {"cursor", state.cursor},
        {"current_month", state.current_month}, {"months_completed", state.months_completed},
        {"games_indexed", state.games_indexed},
        {"started_at_ms", static_cast<double>(state.started_at_ms)},
        {"updated_at_ms", static_cast<double>(state.updated_at_ms)},
        {"last_error", state.last_error},
    };
}

ChessComSyncState sync_state_from_json(const json::Value& value) {
    ChessComSyncState state;
    state.status = value.get("status", "idle").as_string();
    state.username = value.get("username", "").as_string();
    state.cursor = validate_cursor(value.get("cursor", "").as_string());
    state.current_month = value.get("current_month", "").as_string();
    state.months_completed = value.get("months_completed", 0).as_size();
    state.games_indexed = value.get("games_indexed", 0).as_size();
    state.started_at_ms = static_cast<std::int64_t>(value.get("started_at_ms", 0).as_number());
    state.updated_at_ms = static_cast<std::int64_t>(value.get("updated_at_ms", 0).as_number());
    state.last_error = sanitize_error(value.get("last_error", "").as_string());
    return state;
}

std::string hash_string(std::uint64_t hash) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

} // namespace

std::string normalize_chesscom_username(std::string_view username) {
    const auto first = username.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        throw Error(ErrorCode::InvalidArgument, "Chess.com username is empty");
    const auto last = username.find_last_not_of(" \t\r\n");
    username = username.substr(first, last - first + 1);
    if (username.size() > 25)
        throw Error(ErrorCode::InvalidArgument, "Chess.com username is too long");
    std::string normalized;
    normalized.reserve(username.size());
    for (const char raw_character : username) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (!std::isalnum(character) && character != '_' && character != '-')
            throw Error(ErrorCode::InvalidArgument, "Chess.com username contains invalid characters");
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

bool valid_chesscom_month(std::string_view month) {
    if (month.size() != 7 || month[4] != '-')
        return false;
    for (std::size_t index = 0; index < month.size(); ++index)
        if (index != 4 && !std::isdigit(static_cast<unsigned char>(month[index])))
            return false;
    const int year = std::stoi(std::string(month.substr(0, 4)));
    const int number = std::stoi(std::string(month.substr(5, 2)));
    return year >= 2000 && year <= 9999 && number >= 1 && number <= 12;
}

bool valid_chesscom_month_checkpoint(const ChessComMonthCheckpoint& checkpoint) {
    if (!valid_chesscom_month(checkpoint.month) || checkpoint.completed_at_ms < 0)
        return false;
    try {
        return checkpoint.username == normalize_chesscom_username(checkpoint.username);
    } catch (const Error&) {
        return false;
    }
}

OwnerId::OwnerId(OwnerKind kind, std::string id) : kind_(kind), value_(std::move(id)) {
    if (value_.empty())
        throw Error(ErrorCode::InvalidArgument, "Repository owner id cannot be empty");
    if (value_.size() > 256)
        throw Error(ErrorCode::InvalidArgument, "Repository owner id is too long");
}

OwnerId OwnerId::local() {
    return OwnerId(OwnerKind::Local, "local");
}

OwnerId OwnerId::guest(std::string id) {
    return OwnerId(OwnerKind::Guest, std::move(id));
}

OwnerId OwnerId::account(std::string id) {
    return OwnerId(OwnerKind::Account, std::move(id));
}

EventLogRepository::EventLogRepository(storage::EventLog& log) : log_(log) {
    replay();
    rebuild_indexes();
}

void EventLogRepository::replay() {
    const storage::ReplayResult events = log_.replay();
    std::uint64_t snapshot_event_id = 0;
    std::uint64_t selected_snapshot_marker_id = 0;
    struct SnapshotAuthorization {
        std::uint64_t watermark{0};
        std::uint64_t marker_id{0};
        int version{0};
    };
    std::map<std::string, std::vector<SnapshotAuthorization>> snapshot_authorizations;
    for (const storage::Event& event : events.events) {
        if (event.type != storage::EventType::ProfileSnapshotCreated)
            continue;
        try {
            const json::Value marker = json::parse(event.payload);
            const std::string path = marker.at("path").as_string();
            const auto watermark =
                static_cast<std::uint64_t>(marker.at("last_event_id").as_number());
            const int version = marker.at("snapshot_version").as_int();
            if ((version != 1 && version != 2) || watermark >= event.id || path.empty() ||
                std::filesystem::path(path).filename() != std::filesystem::path(path))
                continue;
            snapshot_authorizations[path].push_back(
                SnapshotAuthorization{watermark, event.id, version});
        } catch (const std::exception&) {
            // A malformed marker cannot authorize a snapshot accelerator.
        }
    }
    const std::filesystem::path snapshot_directory = log_.path().parent_path() / "snapshots";
    if (std::filesystem::exists(snapshot_directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(snapshot_directory)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            try {
                std::ifstream input(entry.path());
                std::stringstream contents;
                contents << input.rdbuf();
                const json::Value snapshot = json::parse(contents.str());
                const auto event_id = static_cast<std::uint64_t>(snapshot.at("last_event_id").as_number());
                const int snapshot_version = snapshot.at("snapshot_version").as_int();
                const auto authorizations =
                    snapshot_authorizations.find(entry.path().filename().string());
                if ((snapshot_version != 1 && snapshot_version != 2) ||
                    authorizations == snapshot_authorizations.end())
                    continue;
                std::uint64_t marker_id = 0;
                for (const auto& authorization : authorizations->second)
                    if (authorization.watermark == event_id &&
                        authorization.version == snapshot_version)
                        marker_id = std::max(marker_id, authorization.marker_id);
                if (marker_id == 0 || event_id < snapshot_event_id ||
                    (event_id == snapshot_event_id && marker_id <= selected_snapshot_marker_id))
                    continue;
                std::map<std::string, StoredGame> games;
                std::map<std::string, training::Drill> drills;
                std::map<std::string, std::int64_t> completions;
                std::map<std::string, std::string> job_states;
                std::map<std::string, json::Value> batches;
                std::set<std::string> recommendations;
                std::optional<ChessComProfile> chesscom_profile;
                std::map<std::string, ChessComArchiveEntry> chesscom_archive;
                std::map<std::string, ChessComMonthCheckpoint> chesscom_checkpoints;
                ChessComSyncState chesscom_sync_state;
                const json::Value null_value;
                for (const auto& value : snapshot.at("games").as_array()) {
                    const std::string pgn = value.at("pgn").as_string();
                    import::ImportedGame imported{chess::parse_pgn(pgn),
                                                  value.at("source_url").as_string(), pgn,
                                                  parse_method(value.at("import_method").as_string())};
                    std::optional<analysis::GameAnalysis> completed;
                    const std::string analysis_status =
                        value.get("analysis_status", "pending").as_string();
                    if (analysis_status == "complete" && !value.at("analysis").is_null())
                        completed = analysis_from_json(value.at("analysis"));
                    std::optional<analysis::GameAnalysis> shallow;
                    if (analysis_status == "shallow" &&
                        !value.get("shallow_analysis", null_value).is_null())
                        shallow = analysis_from_json(value.at("shallow_analysis"));
                    const std::string game_id = imported.game.identity;
                    games.emplace(
                        game_id,
                        StoredGame{std::move(imported), std::move(completed),
                                   static_cast<std::int64_t>(
                                       value.get("imported_at_ms", 0).as_number()),
                                   static_cast<std::int64_t>(
                                       value.get("analyzed_at_ms", 0).as_number()),
                                   std::move(shallow)});
                }
                for (const auto& value : snapshot.at("drills").as_array()) {
                    training::Drill drill = drill_from_json(value);
                    drills.emplace(drill.id, std::move(drill));
                }
                for (const auto& value : snapshot.at("resource_completions").as_array())
                    completions.emplace(value.at("resource_id").as_string(),
                                        static_cast<std::int64_t>(value.at("completed_at_ms").as_number()));
                const json::Value empty_jobs{json::Value::Array{}};
                for (const auto& value : snapshot.get("analysis_jobs", empty_jobs).as_array())
                    job_states.emplace(value.at("game_id").as_string(),
                                       value.at("status").as_string());
                const json::Value empty_batches{json::Value::Array{}};
                for (const auto& value : snapshot.get("batches", empty_batches).as_array())
                    batches.emplace(value.at("id").as_string(), value);
                const json::Value empty_recommendations{json::Value::Array{}};
                for (const auto& value :
                     snapshot.get("recommended_resources", empty_recommendations).as_array())
                    recommendations.insert(value.as_string());
                if (snapshot_version == 2) {
                    if (!snapshot.get("chesscom_profile", null_value).is_null())
                        chesscom_profile =
                            chesscom_profile_from_json(snapshot.at("chesscom_profile"));
                    const json::Value empty_archive{json::Value::Array{}};
                    for (const auto& value :
                         snapshot.get("chesscom_archive", empty_archive).as_array()) {
                        auto archive = chesscom_archive_from_json(value);
                        chesscom_archive.insert_or_assign(archive.game_id, std::move(archive));
                    }
                    const json::Value empty_checkpoints{json::Value::Array{}};
                    for (const auto& value :
                         snapshot.get("chesscom_checkpoints", empty_checkpoints).as_array()) {
                        auto checkpoint = checkpoint_from_json(value);
                        chesscom_checkpoints.insert_or_assign(
                            checkpoint.username + "\n" + checkpoint.month,
                            std::move(checkpoint));
                    }
                    chesscom_sync_state = sync_state_from_json(
                        snapshot.get("chesscom_sync_state", json::Value::Object{}));
                }
                games_ = std::move(games);
                drills_ = std::move(drills);
                resource_completions_ = std::move(completions);
                analysis_job_states_ = std::move(job_states);
                batches_ = std::move(batches);
                recommended_resources_ = std::move(recommendations);
                chesscom_profile_ = std::move(chesscom_profile);
                chesscom_archive_ = std::move(chesscom_archive);
                chesscom_checkpoints_ = std::move(chesscom_checkpoints);
                chesscom_sync_state_ = std::move(chesscom_sync_state);
                for (const auto& [_, batch] : batches_)
                    next_batch_id_ = std::max(
                        next_batch_id_,
                        static_cast<std::uint64_t>(batch.at("sequence").as_number()) + 1);
                background_paused_ = snapshot.get("background_paused", false).as_bool();
                for (const auto& [_, drill] : drills_)
                    for (const auto& attempt : drill.attempts)
                        next_attempt_id_ = std::max(next_attempt_id_, attempt.id + 1);
                snapshot_event_id = event_id;
                selected_snapshot_marker_id = marker_id;
            } catch (const std::exception& error) {
                log(LogLevel::Warning, "repository",
                    "skipping invalid snapshot " + entry.path().filename().string() + ": " +
                        error.what());
                continue;
            }
        }
    }
    for (const storage::Event& event : events.events) {
        if (event.id <= snapshot_event_id &&
            event.type != storage::EventType::VariationSaved &&
            event.type != storage::EventType::VariationDeleted &&
            event.type != storage::EventType::ReviewAttempted)
            continue;
        try {
            const json::Value payload = json::parse(event.payload);
            if (event.type == storage::EventType::GameImported) {
                const std::string pgn = payload.at("pgn").as_string();
                import::ImportedGame imported{
                    chess::parse_pgn(pgn),
                    payload.at("source_url").as_string(),
                    pgn,
                    parse_method(payload.at("method").as_string()),
                };
                const std::string id = imported.game.identity;
                games_.insert_or_assign(
                    id, StoredGame{std::move(imported), std::nullopt, event.timestamp_ms, 0,
                                   std::nullopt});
            } else if (event.type == storage::EventType::AnalysisCompleted) {
                const std::string id = payload.at("game_id").as_string();
                const auto found = games_.find(id);
                if (found != games_.end())
                    found->second.analysis = analysis_from_json(payload.at("analysis"));
                if (found != games_.end())
                    found->second.analyzed_at_ms = event.timestamp_ms;
            } else if (event.type == storage::EventType::ShallowAnalysisCompleted) {
                const auto found = games_.find(payload.at("game_id").as_string());
                if (found != games_.end() && !found->second.analysis)
                    found->second.shallow_analysis =
                        analysis_from_json(payload.at("analysis"));
            } else if (event.type == storage::EventType::DrillCreated) {
                training::Drill drill = drill_from_json(payload);
                drills_.insert_or_assign(drill.id, std::move(drill));
            } else if (event.type == storage::EventType::DrillAttempted) {
                const auto found = drills_.find(payload.at("drill_id").as_string());
                if (found != drills_.end()) {
                    training::DrillAttempt attempt;
                    attempt.id = static_cast<std::uint64_t>(payload.at("attempt_id").as_number());
                    attempt.attempted_at_ms = static_cast<std::int64_t>(payload.at("attempted_at_ms").as_number());
                    attempt.correct = payload.at("correct").as_bool();
                    attempt.move = payload.at("move").as_string();
                    attempt.response_time_ms = static_cast<std::uint64_t>(payload.at("response_time_ms").as_number());
                    attempt.hint_level = payload.at("hint_level").as_int();
                    attempt.retries = payload.at("retries").as_size();
                    next_attempt_id_ = std::max(next_attempt_id_, attempt.id + 1);
                    found->second.attempts.push_back(std::move(attempt));
                }
            } else if (event.type == storage::EventType::DrillSessionUpdated) {
                const auto found = drills_.find(payload.at("drill_id").as_string());
                if (found != drills_.end()) {
                    found->second.session_hint_level = payload.at("hint_level").as_int();
                    found->second.session_started_at_ms = static_cast<std::int64_t>(
                        payload.at("started_at_ms").as_number());
                }
            } else if (event.type == storage::EventType::ResourceCompleted) {
                resource_completions_.insert_or_assign(
                    payload.at("resource_id").as_string(),
                    static_cast<std::int64_t>(payload.at("completed_at_ms").as_number()));
            } else if (event.type == storage::EventType::ResourceRecommended) {
                recommended_resources_.insert(payload.at("resource_id").as_string());
            } else if (event.type == storage::EventType::AnalysisJobStateChanged) {
                analysis_job_states_.insert_or_assign(payload.at("game_id").as_string(),
                                                      payload.at("status").as_string());
            } else if (event.type == storage::EventType::BatchStateChanged) {
                if (payload.as_object().contains("paused"))
                    background_paused_ = payload.at("paused").as_bool();
            } else if (event.type == storage::EventType::BatchCreated) {
                const std::string id = payload.at("id").as_string();
                batches_.insert_or_assign(id, payload);
                next_batch_id_ = std::max(next_batch_id_,
                                          static_cast<std::uint64_t>(payload.at("sequence").as_number()) + 1);
            } else if (event.type == storage::EventType::ChessComProfileUpdated) {
                chesscom_profile_ = chesscom_profile_from_json(payload);
            } else if (event.type == storage::EventType::ChessComArchiveChunkIndexed) {
                for (const auto& value : payload.at("entries").as_array()) {
                    auto entry = chesscom_archive_from_json(value);
                    chesscom_archive_.try_emplace(entry.game_id, std::move(entry));
                }
            } else if (event.type == storage::EventType::ChessComMonthCheckpointed) {
                auto checkpoint = checkpoint_from_json(payload);
                chesscom_checkpoints_.insert_or_assign(
                    checkpoint.username + "\n" + checkpoint.month, std::move(checkpoint));
            } else if (event.type == storage::EventType::ChessComSyncStateChanged) {
                chesscom_sync_state_ = sync_state_from_json(payload);
            } else if (event.type == storage::EventType::VariationSaved) {
                Variation variation = variation_from_json(payload);
                if (variation.id.starts_with("variation-")) {
                    try {
                        next_variation_id_ = std::max(
                            next_variation_id_,
                            static_cast<std::uint64_t>(std::stoull(variation.id.substr(10))) + 1);
                    } catch (const std::exception&) {
                        // Non-sequential imported identifiers remain valid and do not affect allocation.
                    }
                }
                variations_.insert_or_assign(variation.id, std::move(variation));
            } else if (event.type == storage::EventType::VariationDeleted) {
                variations_.erase(payload.at("id").as_string());
            } else if (event.type == storage::EventType::ReviewAttempted) {
                ReviewAttempt attempt;
                attempt.id = static_cast<std::uint64_t>(payload.at("id").as_number());
                attempt.game_id = payload.at("game_id").as_string();
                attempt.ply = payload.at("ply").as_size();
                attempt.uci = payload.at("uci").as_string();
                attempt.accepted = payload.at("accepted").as_bool();
                attempt.attempted_at_ms = static_cast<std::int64_t>(
                    payload.at("attempted_at_ms").as_number());
                next_review_attempt_id_ = std::max(next_review_attempt_id_, attempt.id + 1);
                review_attempts_.push_back(std::move(attempt));
            }
        } catch (const Error& error) {
            log(LogLevel::Warning, "repository",
                "skipping event " + std::to_string(event.id) + ": " + error.what());
        }
    }
    projection_event_id_ = events.events.empty() ? 0 : events.events.back().id;
    projection_contiguous_ = events.corruptions.empty() && !events.truncated_tail;
}

void EventLogRepository::note_applied_event(const storage::Event& event) {
    if (projection_contiguous_ && event.id == projection_event_id_ + 1) {
        projection_event_id_ = event.id;
        return;
    }
    projection_contiguous_ = false;
}

AddResult EventLogRepository::add(const import::ImportedGame& imported) {
    std::lock_guard lock(mutex_);
    return add_unlocked(imported, true);
}

BulkAddResult EventLogRepository::bulk_add(std::vector<import::ImportedGame> imported_games) {
    if (imported_games.size() > bulk_game_import_limit)
        throw Error(ErrorCode::InvalidArgument, "bulk game import has too many games");
    std::size_t total_pgn_bytes = 0;
    for (const auto& imported : imported_games) {
        if (imported.pgn.size() > bulk_game_import_single_pgn_byte_limit ||
            imported.source_url.size() > bulk_game_import_source_url_limit ||
            imported.pgn.size() > bulk_game_import_pgn_byte_limit - total_pgn_bytes)
            throw Error(ErrorCode::InvalidArgument, "bulk game import PGN data is too large");
        total_pgn_bytes += imported.pgn.size();
    }

    std::lock_guard lock(mutex_);
    BulkAddResult result;
    result.added_game_ids.reserve(imported_games.size());
    result.duplicate_game_ids.reserve(imported_games.size());
    try {
        for (const auto& imported : imported_games) {
            if (add_unlocked(imported, false) == AddResult::Added) {
                ++result.added;
                result.added_game_ids.push_back(imported.game.identity);
            } else {
                ++result.duplicates;
                result.duplicate_game_ids.push_back(imported.game.identity);
            }
        }
    } catch (...) {
        if (result.added > 0)
            rebuild_indexes();
        throw;
    }
    if (result.added > 0)
        rebuild_indexes();
    return result;
}

AddResult EventLogRepository::add_unlocked(const import::ImportedGame& imported,
                                   bool rebuild_indexes_after_add) {
    if (games_.contains(imported.game.identity))
        return AddResult::Duplicate;
    const storage::Event imported_event_record =
        log_.append(storage::EventType::GameImported, json::dump(imported_event(imported)));
    const storage::Event parsed_event_record =
        log_.append(storage::EventType::GameParsed, json::dump(json::Value::Object{
                                                        {"game_id", imported.game.identity},
                                                        {"plies", imported.game.plies.size()},
                                                    }));
    games_.emplace(imported.game.identity,
                   StoredGame{imported, std::nullopt, imported_event_record.timestamp_ms, 0,
                              std::nullopt});
    note_applied_event(imported_event_record);
    note_applied_event(parsed_event_record);
    for (const auto& color : {std::string("White"), std::string("Black")}) {
        const std::string rating = imported.game.tag(color + "Elo");
        if (rating.empty())
            continue;
        try {
            const int value = std::stoi(rating);
            const storage::Event rating_event = log_.append(
                storage::EventType::RatingObserved,
                json::dump(json::Value::Object{{"game_id", imported.game.identity},
                                                {"player", imported.game.tag(color)},
                                                {"color", color == "White" ? "white" : "black"},
                                                {"rating", value}}));
            note_applied_event(rating_event);
        } catch (const std::exception&) {
        }
    }
    if (rebuild_indexes_after_add)
        rebuild_indexes();
    return AddResult::Added;
}

void EventLogRepository::save_analysis(const analysis::GameAnalysis& analysis) {
    std::unique_lock lock(mutex_);
    const auto found = games_.find(analysis.game_id);
    if (found == games_.end())
        throw Error(ErrorCode::NotFound, "cannot save analysis for unknown game");
    for (const auto& move : analysis.moves) {
        note_applied_event(
            log_.append(storage::EventType::PositionAnalyzed, json::dump(json::Value::Object{
                                                                  {"game_id", analysis.game_id},
                                                                  {"move", move_json(move)},
                                                              })));
    }
    for (const auto& mistake : analysis.mistakes) {
        json::Value::Array classification_evidence;
        for (const auto& evidence : mistake.evidence)
            classification_evidence.emplace_back(evidence);
        note_applied_event(
            log_.append(storage::EventType::MistakeDetected, json::dump(json::Value::Object{
                                                                 {"game_id", analysis.game_id},
                                                                 {"mistake", mistake_json(mistake)},
                                                             })));
        note_applied_event(
            log_.append(storage::EventType::MistakeClassified, json::dump(json::Value::Object{
                                                                   {"game_id", analysis.game_id},
                                                                   {"ply", mistake.ply},
                                                                   {"category", mistake.category},
                                                                   {"evidence", std::move(classification_evidence)},
                                                                   {"confidence", mistake.confidence},
                                                                   {"classifier_version", mistake.classifier_version},
                                                               })));
        note_applied_event(log_.append(storage::EventType::ExplanationCreated,
                                       json::dump(json::Value::Object{
                                           {"game_id", analysis.game_id},
                                           {"ply", mistake.ply},
                                           {"explanation", mistake.explanation},
                                       })));
    }
    const storage::Event completed_event =
        log_.append(storage::EventType::AnalysisCompleted, json::dump(json::Value::Object{
                                                               {"game_id", analysis.game_id},
                                                               {"analysis", to_json(analysis)},
                                                           }));
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    found->second.analysis = analysis;
    found->second.shallow_analysis.reset();
    found->second.analyzed_at_ms = now;
    note_applied_event(completed_event);
    for (const auto& mistake : analysis.mistakes) {
        const std::string id = analysis.game_id + ":" + std::to_string(mistake.ply);
        if (drills_.contains(id) || mistake.better_moves.empty())
            continue;
        std::set<std::string> engine_moves;
        for (const auto& line : mistake.engine_details.lines)
            if (!line.moves.empty())
                engine_moves.insert(line.moves.front());
        std::vector<std::string> verified_solutions;
        chess::Board solution_board = chess::Board::from_fen(mistake.fen);
        for (const auto& solution : mistake.better_moves) {
            if (!engine_moves.contains(solution))
                continue;
            chess::Board candidate_board = solution_board;
            if (parse_uci(candidate_board, solution))
                verified_solutions.push_back(solution);
        }
        if (verified_solutions.empty())
            continue;
        training::Drill drill;
        drill.id = id;
        drill.source_game_id = analysis.game_id;
        drill.source_ply = mistake.ply;
        drill.fen = mistake.fen;
        drill.category = mistake.category;
        drill.phase = std::string(analysis::name(mistake.phase));
        drill.explanation = mistake.explanation;
        drill.punishment = mistake.punishment;
        drill.solutions = std::move(verified_solutions);
        drill.difficulty = std::clamp(1 + mistake.loss / 150, 1, 5);
        drill.impact_cp = mistake.loss;
        drill.created_at_ms = now;
        drill.opponent_response = mistake.punishment;
        if (mistake.ply < found->second.imported.game.plies.size()) {
            drill.played_move = chess::uci(found->second.imported.game.plies[mistake.ply].move);
            chess::Board lesson_board = chess::Board::from_fen(drill.fen);
            if (const auto played = parse_uci(lesson_board, drill.played_move)) {
                static_cast<void>(lesson_board.make_move(*played));
                drill.fen_after_mistake = lesson_board.to_fen();
                drill.attacked_pieces = attacked_piece_descriptions(
                    lesson_board, chess::opposite(lesson_board.side_to_move()));
                if (const auto reply = parse_uci(lesson_board, drill.punishment)) {
                    const chess::Piece captured =
                        reply->has(chess::EnPassant)
                            ? chess::Piece{chess::opposite(lesson_board.side_to_move()),
                                           chess::PieceType::Pawn}
                            : lesson_board.at(reply->to);
                    static_cast<void>(lesson_board.make_move(*reply));
                    drill.fen_after_punishment = lesson_board.to_fen();
                    const bool check = lesson_board.in_check(lesson_board.side_to_move());
                    const bool mate = check && lesson_board.legal_moves().empty();
                    if (mate) {
                        drill.changed_threat = "The reply " + drill.punishment + " is checkmate.";
                    } else if (!captured.empty()) {
                        drill.changed_threat = "The reply " + drill.punishment + " captures your " +
                                               piece_name(captured.type) + " on " +
                                               chess::square_name(reply->to) + ".";
                    } else if (check) {
                        drill.changed_threat =
                            "The reply " + drill.punishment + " gives check and takes the initiative.";
                    } else {
                        drill.changed_threat =
                            "The opponent's strongest reply is " + drill.punishment + ".";
                    }
                } else {
                    drill.fen_after_punishment = drill.fen_after_mistake;
                }
            }
        }
        if (drill.changed_threat.empty())
            drill.changed_threat = "The opponent's strongest reply is " + drill.punishment + ".";
        const storage::Event drill_event = log_.append(
            storage::EventType::DrillCreated, json::dump(training::to_json(drill, now)));
        drills_.emplace(id, std::move(drill));
        note_applied_event(drill_event);
    }
    rebuild_indexes();
    const std::size_t analyzed_count = static_cast<std::size_t>(std::count_if(
        games_.begin(), games_.end(), [](const auto& entry) { return entry.second.analysis.has_value(); }));
    lock.unlock();
    if (analyzed_count > 0 && analyzed_count % 10 == 0)
        static_cast<void>(create_snapshot());
}

void EventLogRepository::save_shallow_analysis(const analysis::GameAnalysis& analysis) {
    std::lock_guard lock(mutex_);
    const auto found = games_.find(analysis.game_id);
    if (found == games_.end())
        throw Error(ErrorCode::NotFound,
                    "cannot save shallow analysis for unknown game");
    if (found->second.analysis)
        return;
    const storage::Event shallow_event = log_.append(
        storage::EventType::ShallowAnalysisCompleted,
        json::dump(json::Value::Object{{"game_id", analysis.game_id},
                                       {"analysis", to_json(analysis)}}));
    found->second.shallow_analysis = analysis;
    note_applied_event(shallow_event);
    rebuild_indexes();
}

std::optional<StoredGame> EventLogRepository::get(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = games_.find(std::string(id));
    if (found == games_.end())
        return std::nullopt;
    return found->second;
}

bool EventLogRepository::add_validated_drill(training::Drill drill) {
    std::lock_guard lock(mutex_);
    if (drill.id.empty() || drill.fen.empty() || drill.solutions.empty() ||
        drill.validation_evidence.empty())
        throw Error(ErrorCode::InvalidArgument, "validated drill evidence is incomplete");
    if (drills_.contains(drill.id))
        return false;
    chess::Board board = chess::Board::from_fen(drill.fen);
    for (const auto& solution : drill.solutions)
        if (!parse_uci(board, solution))
            throw Error(ErrorCode::IllegalMove, "validated drill contains an illegal solution");
    if (drill.created_at_ms == 0) {
        drill.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    }
    const storage::Event drill_event =
        log_.append(storage::EventType::DrillCreated,
                    json::dump(training::to_json(drill, drill.created_at_ms)));
    drills_.emplace(drill.id, std::move(drill));
    note_applied_event(drill_event);
    rebuild_indexes();
    return true;
}

std::vector<StoredGame> EventLogRepository::list() const {
    std::lock_guard lock(mutex_);
    std::vector<StoredGame> result;
    result.reserve(games_.size());
    for (const auto& [_, game] : games_)
        result.push_back(game);
    return result;
}

std::size_t EventLogRepository::size() const {
    std::lock_guard lock(mutex_);
    return games_.size();
}

std::vector<training::Drill> EventLogRepository::drills(std::int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    std::vector<training::Drill> result;
    for (const auto& [_, drill] : drills_)
        result.push_back(drill);
    return training::review_queue(std::move(result), now_ms);
}

std::optional<training::Drill> EventLogRepository::drill(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto found = drills_.find(std::string(id));
    return found == drills_.end() ? std::nullopt : std::optional<training::Drill>(found->second);
}

training::DrillAttempt EventLogRepository::record_attempt(std::string_view drill_id, std::string move,
                                                  std::uint64_t response_time_ms, int hint_level,
                                                  std::int64_t attempted_at_ms) {
    std::lock_guard lock(mutex_);
    const auto found = drills_.find(std::string(drill_id));
    if (found == drills_.end())
        throw Error(ErrorCode::NotFound, "drill does not exist");
    chess::Board board = chess::Board::from_fen(found->second.fen);
    bool legal = false;
    for (const auto& candidate : board.legal_moves()) {
        if (chess::uci(candidate) == move) {
            legal = true;
            break;
        }
    }
    if (!legal)
        throw Error(ErrorCode::InvalidArgument, "attempt move is not legal in the drill position");
    const bool correct =
        std::find(found->second.solutions.begin(), found->second.solutions.end(), move) !=
        found->second.solutions.end();
    std::size_t retries = 0;
    for (auto it = found->second.attempts.rbegin();
         it != found->second.attempts.rend() && !it->correct; ++it)
        ++retries;
    static_cast<void>(hint_level);
    const std::uint64_t measured_response = found->second.session_started_at_ms > 0 &&
                                                    attempted_at_ms >=
                                                        found->second.session_started_at_ms
                                                ? static_cast<std::uint64_t>(
                                                      attempted_at_ms -
                                                      found->second.session_started_at_ms)
                                                : response_time_ms;
    training::DrillAttempt attempt{next_attempt_id_++, attempted_at_ms, correct, std::move(move),
                                   measured_response, found->second.session_hint_level, retries};
    const storage::Event attempt_event = log_.append(
        storage::EventType::DrillAttempted,
        json::dump(json::Value::Object{
            {"drill_id", found->second.id}, {"attempt_id", static_cast<double>(attempt.id)},
            {"attempted_at_ms", static_cast<double>(attempt.attempted_at_ms)},
            {"correct", attempt.correct}, {"move", attempt.move},
            {"response_time_ms", static_cast<double>(attempt.response_time_ms)},
            {"hint_level", attempt.hint_level}, {"retries", attempt.retries},
            {"scheduler_version", std::string(training::scheduler_version)},
        }));
    found->second.attempts.push_back(attempt);
    note_applied_event(attempt_event);
    if (correct) {
        const storage::Event session_event = log_.append(
            storage::EventType::DrillSessionUpdated,
            json::dump(json::Value::Object{{"drill_id", found->second.id},
                                            {"hint_level", 0},
                                            {"started_at_ms", 0}}));
        found->second.session_hint_level = 0;
        found->second.session_started_at_ms = 0;
        note_applied_event(session_event);
    }
    rebuild_indexes();
    return attempt;
}

training::Drill EventLogRepository::begin_drill_session(std::string_view drill_id,
                                                std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    const auto found = drills_.find(std::string(drill_id));
    if (found == drills_.end())
        throw Error(ErrorCode::NotFound, "drill does not exist");
    if (found->second.session_started_at_ms == 0) {
        const storage::Event session_event = log_.append(
            storage::EventType::DrillSessionUpdated,
            json::dump(json::Value::Object{
                {"drill_id", found->second.id},
                {"hint_level", found->second.session_hint_level},
                {"started_at_ms", static_cast<double>(now_ms)},
            }));
        found->second.session_started_at_ms = now_ms;
        note_applied_event(session_event);
    }
    return found->second;
}

training::Drill EventLogRepository::advance_hint(std::string_view drill_id, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    const auto found = drills_.find(std::string(drill_id));
    if (found == drills_.end())
        throw Error(ErrorCode::NotFound, "drill does not exist");
    static_cast<void>(now_ms);
    const int available = training::available_hint_level(found->second);
    if (found->second.session_hint_level >= available)
        throw Error(ErrorCode::InvalidArgument,
                    "another failed attempt is required before the next hint");
    const int next_hint_level = std::min(3, found->second.session_hint_level + 1);
    const storage::Event session_event = log_.append(
        storage::EventType::DrillSessionUpdated,
        json::dump(json::Value::Object{
            {"drill_id", found->second.id},
            {"hint_level", next_hint_level},
            {"started_at_ms", static_cast<double>(found->second.session_started_at_ms)},
        }));
    found->second.session_hint_level = next_hint_level;
    note_applied_event(session_event);
    return found->second;
}

training::Profile EventLogRepository::profile_unlocked() const {
    training::Profile result;
    result.games_imported = games_.size();
    std::map<std::string, std::size_t> player_frequency;
    for (const auto& [_, game] : games_) {
        const std::string white = game.imported.game.tag("White");
        const std::string black = game.imported.game.tag("Black");
        if (!white.empty()) ++player_frequency[white];
        if (!black.empty()) ++player_frequency[black];
    }
    for (const auto& [player, count] : player_frequency)
        if (result.player_name.empty() || count > player_frequency[result.player_name])
            result.player_name = player;
    std::map<std::string, training::Weakness> weaknesses;
    std::map<std::string, std::set<std::string>> category_games;
    std::map<std::string, std::vector<std::int64_t>> category_times;
    std::map<std::string, training::Profile::OpeningPerformance> openings;
    std::int64_t latest_rating_at = 0;
    double loss_total = 0.0;
    std::size_t move_count = 0;
    const auto profile_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    constexpr std::int64_t day_ms = 24LL * 60LL * 60LL * 1000LL;
    const std::int64_t today_start = profile_now / day_ms * day_ms;
    std::map<std::int64_t, training::Profile::TrendPoint> trend;
    for (int offset = 13; offset >= 0; --offset) {
        const std::int64_t day = today_start - static_cast<std::int64_t>(offset) * day_ms;
        trend.emplace(day, training::Profile::TrendPoint{day});
    }
    for (const auto& [game_id, game] : games_) {
        const bool player_is_white = game.imported.game.tag("White") == result.player_name;
        const bool player_is_black = game.imported.game.tag("Black") == result.player_name;
        if (player_is_white || player_is_black) {
            const std::string rating =
                game.imported.game.tag(player_is_white ? "WhiteElo" : "BlackElo");
            if (!rating.empty()) {
                try {
                    if (game.imported_at_ms >= latest_rating_at) {
                        result.latest_rating = std::stoi(rating);
                        latest_rating_at = game.imported_at_ms;
                    }
                    ++result.rating_observations;
                } catch (const std::exception&) {
                }
            }
        }
        const analysis::GameAnalysis* projected =
            game.analysis ? &*game.analysis
                          : game.shallow_analysis ? &*game.shallow_analysis : nullptr;
        if (!projected)
            continue;
        if (game.analysis) {
            ++result.games_analyzed;
            const std::int64_t analyzed_day = game.analyzed_at_ms / day_ms * day_ms;
            if (const auto point = trend.find(analyzed_day); point != trend.end())
                ++point->second.games_analyzed;
            if (game.analyzed_at_ms > 0 && profile_now - game.analyzed_at_ms <= 7 * day_ms)
                ++result.games_analyzed_7_days;
            if (game.analyzed_at_ms > 0 && profile_now - game.analyzed_at_ms <= 30 * day_ms)
                ++result.games_analyzed_30_days;
        } else {
            ++result.games_shallow_analyzed;
        }
        auto& opening = openings[projected->eco + "|" + projected->opening];
        opening.eco = projected->eco;
        opening.name = projected->opening;
        ++opening.games;
        result.total_positions += projected->moves.size();
        const bool reached_endgame = std::any_of(
            projected->moves.begin(), projected->moves.end(),
            [](const auto& move) { return move.phase == analysis::GamePhase::Endgame; });
        if (reached_endgame && (player_is_white || player_is_black)) {
            ++result.endgame_conversion.denominator;
            const std::string result_tag = game.imported.game.tag("Result");
            if ((player_is_white && result_tag == "1-0") ||
                (player_is_black && result_tag == "0-1"))
                ++result.endgame_conversion.numerator;
        }
        bool king_safety_violation = false;
        const bool has_clock_data = std::any_of(
            game.imported.game.plies.begin(), game.imported.game.plies.end(),
            [](const auto& ply) {
                return ply.clock_ms.has_value() || ply.elapsed_ms.has_value();
            });
        bool time_management_failure = false;
        for (const auto& move : projected->moves) {
            loss_total += move.loss;
            opening.average_centipawn_loss += move.loss;
            ++move_count;
        }
        for (const auto& mistake : projected->mistakes) {
            ++opening.mistakes;
            ++result.total_mistakes;
            if (game.analysis) {
                const std::int64_t analyzed_day = game.analyzed_at_ms / day_ms * day_ms;
                if (const auto point = trend.find(analyzed_day); point != trend.end())
                    ++point->second.mistakes;
            }
            auto& weakness = weaknesses[mistake.category];
            weakness.category = mistake.category;
            ++weakness.occurrences;
            if (mistake.category.find("king") != std::string::npos ||
                mistake.category.find("mate") != std::string::npos ||
                mistake.category.find("Back-rank") != std::string::npos)
                king_safety_violation = true;
            if (mistake.category == "Instant-move blunder" ||
                mistake.category == "Excessive early time use" ||
                mistake.category == "Time-management failure")
                time_management_failure = true;
            if (game.analyzed_at_ms > 0 && profile_now - game.analyzed_at_ms <= 7 * day_ms)
                ++weakness.occurrences_7_days;
            if (game.analyzed_at_ms > 0 && profile_now - game.analyzed_at_ms <= 30 * day_ms)
                ++weakness.occurrences_30_days;
            weakness.average_loss_cp += mistake.loss;
            ++weakness.phases[std::string(analysis::name(mistake.phase))];
            category_games[mistake.category].insert(game_id);
            if (game.analyzed_at_ms > 0)
                category_times[mistake.category].push_back(game.analyzed_at_ms);
        }
        if (game.analysis) {
            ++result.king_safety_violations.denominator;
            if (king_safety_violation)
                ++result.king_safety_violations.numerator;
            if (has_clock_data) {
                ++result.time_management_failures.denominator;
                if (time_management_failure)
                    ++result.time_management_failures.numerator;
            }
        }
    }
    for (const auto& [_, drill] : drills_) {
        auto& weakness = weaknesses[drill.category];
        weakness.category = drill.category;
        for (const auto& attempt : drill.attempts) {
            ++weakness.attempts;
            ++result.drill_attempts;
            const std::int64_t attempt_day = attempt.attempted_at_ms / day_ms * day_ms;
            if (const auto point = trend.find(attempt_day); point != trend.end()) {
                ++point->second.drill_attempts;
                if (attempt.correct)
                    ++point->second.drill_correct;
            }
            if (attempt.correct) {
                ++weakness.correct;
                ++result.drill_correct;
            }
        }
        if (!drill.attempts.empty()) {
            ++result.retention_reviews;
            const auto& latest = drill.attempts.back();
            if (latest.correct && latest.hint_level < 3)
                ++result.retained_reviews;
        }
    }
    for (auto& [category, weakness] : weaknesses) {
        weakness.games = category_games[category].size();
        weakness.drill_accuracy = weakness.attempts == 0
                                      ? 0.0
                                      : static_cast<double>(weakness.correct) /
                                            static_cast<double>(weakness.attempts);
        weakness.average_loss_cp = weakness.occurrences == 0
                                       ? 0.0
                                       : weakness.average_loss_cp /
                                             static_cast<double>(weakness.occurrences);
        weakness.recurrence_rate = result.games_analyzed == 0
                                       ? 0.0
                                       : static_cast<double>(weakness.games) /
                                             static_cast<double>(result.games_analyzed);
        auto& times = category_times[category];
        std::sort(times.begin(), times.end());
        if (times.size() >= 2) {
            std::int64_t total_gap = 0;
            for (std::size_t index = 1; index < times.size(); ++index)
                total_gap += times[index] - times[index - 1];
            weakness.repeated_interval_days =
                static_cast<double>(total_gap) /
                static_cast<double>(times.size() - 1) / static_cast<double>(day_ms);
        }
        result.weaknesses.push_back(weakness);
    }
    std::stable_sort(result.weaknesses.begin(), result.weaknesses.end(),
                     [](const auto& left, const auto& right) {
                         return left.occurrences > right.occurrences;
                     });
    result.analysis_completion_rate = result.games_imported == 0
                                          ? 0.0
                                          : static_cast<double>(result.games_analyzed) /
                                                static_cast<double>(result.games_imported);
    result.drill_accuracy = result.drill_attempts == 0
                                ? 0.0
                                : static_cast<double>(result.drill_correct) /
                                      static_cast<double>(result.drill_attempts);
    result.retention_rate = result.retention_reviews == 0
                                ? 0.0
                                : static_cast<double>(result.retained_reviews) /
                                      static_cast<double>(result.retention_reviews);
    result.average_centipawn_loss =
        move_count == 0 ? 0.0 : loss_total / static_cast<double>(move_count);
    for (const auto& [_, point] : trend)
        result.activity_trend.push_back(point);
    for (auto& [_, opening] : openings) {
        std::size_t positions = 0;
        for (const auto& [__, stored] : games_) {
            const analysis::GameAnalysis* projected =
                stored.analysis ? &*stored.analysis
                                : stored.shallow_analysis ? &*stored.shallow_analysis : nullptr;
            if (projected && projected->eco == opening.eco &&
                projected->opening == opening.name)
                positions += projected->moves.size();
        }
        opening.average_centipawn_loss = positions == 0
                                             ? 0.0
                                             : opening.average_centipawn_loss /
                                                   static_cast<double>(positions);
        result.openings.push_back(opening);
    }
    const auto finalize_rate = [](training::Profile::RateMetric& metric) {
        if (metric.denominator >= 5)
            metric.rate = static_cast<double>(metric.numerator) /
                          static_cast<double>(metric.denominator);
    };
    finalize_rate(result.endgame_conversion);
    finalize_rate(result.king_safety_violations);
    finalize_rate(result.time_management_failures);
    return result;
}

training::Profile EventLogRepository::profile() const {
    std::lock_guard lock(mutex_);
    return profile_unlocked();
}

std::vector<training::Recommendation> EventLogRepository::recommendations() {
    std::lock_guard lock(mutex_);
    auto recommendations =
        training::recommend(profile_unlocked(), training::default_catalog(), resource_completions_);
    bool changed = false;
    for (const auto& recommendation : recommendations) {
        if (recommended_resources_.contains(recommendation.resource.id))
            continue;
        const storage::Event recommendation_event = log_.append(
            storage::EventType::ResourceRecommended,
            json::dump(json::Value::Object{
                {"resource_id", recommendation.resource.id},
                {"evidence", recommendation.evidence}, {"priority", recommendation.priority},
                {"catalog_version", std::string(training::catalog_version)},
            }));
        recommended_resources_.insert(recommendation.resource.id);
        note_applied_event(recommendation_event);
        changed = true;
    }
    if (changed)
        rebuild_indexes();
    return recommendations;
}

void EventLogRepository::complete_resource(std::string resource_id, std::int64_t completed_at_ms) {
    std::lock_guard lock(mutex_);
    const auto catalog = training::default_catalog();
    const bool known = std::any_of(catalog.begin(), catalog.end(), [&](const auto& resource) {
        return resource.id == resource_id;
    });
    if (!known)
        throw Error(ErrorCode::NotFound, "resource does not exist");
    const storage::Event resource_event = log_.append(
        storage::EventType::ResourceCompleted,
        json::dump(json::Value::Object{
            {"resource_id", resource_id},
            {"completed_at_ms", static_cast<double>(completed_at_ms)},
            {"catalog_version", std::string(training::catalog_version)},
        }));
    resource_completions_.insert_or_assign(std::move(resource_id), completed_at_ms);
    note_applied_event(resource_event);
    rebuild_indexes();
}

std::filesystem::path EventLogRepository::create_snapshot() {
    std::lock_guard lock(mutex_);
    const auto replayed = log_.replay();
    if (!replayed.corruptions.empty() || replayed.truncated_tail)
        throw Error(ErrorCode::IoError, "cannot snapshot an invalid event log");
    const std::uint64_t authoritative_tail =
        replayed.events.empty() ? 0 : replayed.events.back().id;
    if (!projection_contiguous_ || authoritative_tail != projection_event_id_)
        throw Error(ErrorCode::IoError, "cannot snapshot a stale repository projection");
    const std::uint64_t last_event_id = projection_event_id_;
    json::Value::Array games;
    for (const auto& [_, game] : games_)
        games.push_back(to_json(game, true));
    json::Value::Array drills;
    for (const auto& [_, drill] : drills_)
        drills.push_back(training::to_json(drill, 0));
    json::Value::Array completions;
    for (const auto& [id, completed_at] : resource_completions_)
        completions.emplace_back(json::Value::Object{
            {"resource_id", id}, {"completed_at_ms", static_cast<double>(completed_at)}});
    json::Value::Array jobs;
    for (const auto& [game_id, status] : analysis_job_states_)
        jobs.emplace_back(json::Value::Object{{"game_id", game_id}, {"status", status}});
    json::Value::Array batches;
    for (const auto& [_, batch] : batches_)
        batches.push_back(batch);
    json::Value::Array recommendations;
    for (const auto& id : recommended_resources_)
        recommendations.emplace_back(id);
    json::Value::Array chesscom_archive;
    for (const auto& [_, entry] : chesscom_archive_)
        chesscom_archive.push_back(chesscom_archive_json(entry));
    json::Value::Array chesscom_checkpoints;
    for (const auto& [_, checkpoint] : chesscom_checkpoints_)
        chesscom_checkpoints.push_back(checkpoint_json(checkpoint));
    const std::filesystem::path directory = log_.path().parent_path() / "snapshots";
    create_private_directory(directory);
    const auto nonce = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const std::filesystem::path path =
        directory / ("projection-" + std::to_string(last_event_id) + "-" +
                     std::to_string(::getpid()) + "-" + std::to_string(nonce) + ".json");
    write_index(path, json::Value::Object{
                          {"snapshot_version", 2},
                          {"profile_version", std::string(training::profile_version)},
                          {"last_event_id", static_cast<double>(last_event_id)},
                          {"games", std::move(games)}, {"drills", std::move(drills)},
                          {"resource_completions", std::move(completions)},
                          {"analysis_jobs", std::move(jobs)},
                          {"batches", std::move(batches)},
                          {"recommended_resources", std::move(recommendations)},
                          {"background_paused", background_paused_},
                          {"chesscom_profile", chesscom_profile_
                                                   ? chesscom_profile_json(*chesscom_profile_)
                                                   : json::Value{}},
                          {"chesscom_archive", std::move(chesscom_archive)},
                          {"chesscom_checkpoints", std::move(chesscom_checkpoints)},
                          {"chesscom_sync_state", sync_state_json(chesscom_sync_state_)},
                      });
    const storage::Event snapshot_event =
        log_.append(storage::EventType::ProfileSnapshotCreated,
                    json::dump(json::Value::Object{
                        {"path", path.filename().string()},
                        {"last_event_id", static_cast<double>(last_event_id)},
                        {"snapshot_version", 2},
                    }));
    note_applied_event(snapshot_event);
    rebuild_indexes();
    return path;
}

std::size_t EventLogRepository::compact_storage() {
    std::lock_guard lock(mutex_);
    const std::size_t events = log_.compact();
    const auto replayed = log_.replay();
    for (const auto& event : replayed.events)
        if (event.id > projection_event_id_)
            note_applied_event(event);
    rebuild_indexes();
    return events;
}

void EventLogRepository::record_job_state(std::string game_id, std::string status) {
    std::lock_guard lock(mutex_);
    const storage::Event job_event = log_.append(
        storage::EventType::AnalysisJobStateChanged,
        json::dump(json::Value::Object{{"game_id", game_id}, {"status", status}}));
    analysis_job_states_.insert_or_assign(std::move(game_id), std::move(status));
    note_applied_event(job_event);
}

std::vector<std::string> EventLogRepository::recoverable_analysis_jobs() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [game_id, status] : analysis_job_states_) {
        const auto game = games_.find(game_id);
        if ((status == "queued" || status == "running") && game != games_.end() &&
            !game->second.analysis)
            result.push_back(game_id);
    }
    return result;
}

void EventLogRepository::set_background_paused(bool paused) {
    std::lock_guard lock(mutex_);
    const storage::Event paused_event = log_.append(
        storage::EventType::BatchStateChanged,
        json::dump(json::Value::Object{{"paused", paused}, {"scope", "analysis_queue"}}));
    background_paused_ = paused;
    note_applied_event(paused_event);
}

bool EventLogRepository::background_paused() const {
    std::lock_guard lock(mutex_);
    return background_paused_;
}

json::Value EventLogRepository::create_batch(std::vector<std::string> game_ids, std::size_t discovered,
                                     std::size_t imported, std::size_t duplicates,
                                     std::size_t failed) {
    std::lock_guard lock(mutex_);
    const std::uint64_t sequence = next_batch_id_++;
    const std::string id = "batch-" + std::to_string(sequence);
    json::Value::Array ids;
    for (auto& game_id : game_ids)
        ids.emplace_back(std::move(game_id));
    json::Value value{json::Value::Object{
        {"id", id}, {"sequence", static_cast<double>(sequence)}, {"discovered", discovered},
        {"imported", imported}, {"duplicates", duplicates}, {"failed", failed},
        {"game_ids", std::move(ids)},
    }};
    const storage::Event batch_event =
        log_.append(storage::EventType::BatchCreated, json::dump(value));
    batches_.insert_or_assign(id, value);
    note_applied_event(batch_event);
    return value;
}

json::Value EventLogRepository::batches() const {
    std::lock_guard lock(mutex_);
    json::Value::Array result;
    for (const auto& [_, stored] : batches_) {
        json::Value::Object batch = stored.as_object();
        std::size_t queued = 0;
        std::size_t completed = 0;
        std::size_t job_failed = 0;
        std::size_t positions_analyzed = 0;
        std::size_t positions_remaining = 0;
        for (const auto& id : stored.at("game_ids").as_array()) {
            const auto found = analysis_job_states_.find(id.as_string());
            if (found == analysis_job_states_.end() || found->second == "queued" ||
                found->second == "running")
                ++queued;
            else if (found->second == "complete")
                ++completed;
            else if (found->second == "failed" || found->second == "cancelled")
                ++job_failed;
            const auto game = games_.find(id.as_string());
            if (game != games_.end()) {
                if (game->second.analysis)
                    positions_analyzed += game->second.analysis->moves.size();
                else if (game->second.shallow_analysis)
                    positions_analyzed += game->second.shallow_analysis->moves.size();
                else
                    positions_remaining += game->second.imported.game.plies.size();
            }
        }
        batch.insert_or_assign("queued", queued);
        batch.insert_or_assign("completed", completed);
        batch.insert_or_assign("job_failures", job_failed);
        batch.insert_or_assign("remaining", queued);
        batch.insert_or_assign("positions_analyzed", positions_analyzed);
        batch.insert_or_assign("positions_remaining", positions_remaining);
        batch.insert_or_assign("paused", background_paused_);
        result.emplace_back(std::move(batch));
    }
    return json::Value::Object{{"batches", std::move(result)},
                               {"paused", background_paused_}};
}

void EventLogRepository::save_chesscom_profile(ChessComProfile profile) {
    std::lock_guard lock(mutex_);
    profile.original_username = trim_ascii(profile.original_username);
    profile.normalized_username = normalize_chesscom_username(profile.original_username);
    profile.archive_cursor = validate_cursor(profile.archive_cursor);
    if (profile.last_successful_sync_ms < 0)
        throw Error(ErrorCode::InvalidArgument, "Chess.com profile sync time is invalid");
    profile.last_error = sanitize_error(profile.last_error);
    static const std::set<std::string> allowed_controls{"bullet", "blitz", "rapid", "daily"};
    if (profile.selected_time_controls.size() > 16)
        throw Error(ErrorCode::InvalidArgument, "too many Chess.com time-control values");
    std::set<std::string> controls;
    for (auto control : profile.selected_time_controls) {
        control = trim_ascii(control);
        if (control.size() > 16)
            throw Error(ErrorCode::InvalidArgument, "Chess.com time control is too long");
        std::transform(control.begin(), control.end(), control.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (!control.empty() && !allowed_controls.contains(control))
            throw Error(ErrorCode::InvalidArgument, "Chess.com time control is invalid");
        if (!control.empty()) controls.insert(std::move(control));
    }
    if (controls.size() > allowed_controls.size())
        throw Error(ErrorCode::InvalidArgument, "too many Chess.com time controls");
    profile.selected_time_controls.assign(controls.begin(), controls.end());
    if (chesscom_profile_ == profile)
        return;
    const storage::Event profile_event = log_.append(
        storage::EventType::ChessComProfileUpdated, json::dump(chesscom_profile_json(profile)));
    chesscom_profile_ = std::move(profile);
    note_applied_event(profile_event);
    rebuild_indexes();
}

std::optional<ChessComProfile> EventLogRepository::chesscom_profile() const {
    std::lock_guard lock(mutex_);
    return chesscom_profile_;
}

std::size_t
EventLogRepository::index_chesscom_archive_chunk(std::vector<ChessComArchiveEntry> entries) {
    std::lock_guard lock(mutex_);
    if (entries.size() > chesscom_archive_chunk_limit)
        throw Error(ErrorCode::InvalidArgument, "Chess.com archive chunk has too many entries");
    std::map<std::string, ChessComArchiveEntry> unique;
    for (auto& entry : entries) {
        if (entry.game_id.empty() || entry.pgn.empty() || !valid_chesscom_month(entry.month) ||
            entry.game_id.size() > 64 || entry.canonical_url.size() > 2048 ||
            entry.source_url.size() > 2048 || entry.time_class.size() > 16 ||
            entry.end_time_ms < 0 || entry.fetched_at_ms < 0)
            throw Error(ErrorCode::InvalidArgument, "Chess.com archive entry is invalid");
        entry.username = normalize_chesscom_username(entry.username);
        if (!entry.canonical_url.starts_with("https://www.chess.com/game/") &&
            !entry.canonical_url.starts_with("https://chess.com/game/"))
            throw Error(ErrorCode::InvalidArgument, "Chess.com archive URL is invalid");
        if (!entry.source_url.starts_with("https://api.chess.com/"))
            throw Error(ErrorCode::InvalidArgument, "Chess.com archive source URL is invalid");
        if (!chesscom_archive_.contains(entry.game_id))
            unique.try_emplace(entry.game_id, std::move(entry));
    }
    if (unique.empty())
        return 0;
    json::Value::Array encoded;
    for (const auto& [_, entry] : unique)
        encoded.push_back(chesscom_archive_json(entry));
    const std::string payload =
        json::dump(json::Value::Object{{"entries", std::move(encoded)}});
    if (payload.size() > chesscom_archive_chunk_encoded_byte_limit)
        throw Error(ErrorCode::InvalidArgument, "Chess.com archive chunk is too large");
    const storage::Event archive_event = log_.append(
        storage::EventType::ChessComArchiveChunkIndexed,
        payload);
    for (auto& [game_id, entry] : unique)
        chesscom_archive_.emplace(std::move(game_id), std::move(entry));
    note_applied_event(archive_event);
    rebuild_indexes();
    return unique.size();
}

std::optional<ChessComArchiveEntry>
EventLogRepository::chesscom_archive_entry(std::string_view game_id) const {
    std::lock_guard lock(mutex_);
    const auto found = chesscom_archive_.find(std::string(game_id));
    return found == chesscom_archive_.end() ? std::nullopt
                                           : std::optional<ChessComArchiveEntry>(found->second);
}

ChessComArchivePage
EventLogRepository::search_chesscom_archive(const ChessComArchiveSearch& search) const {
    std::lock_guard lock(mutex_);
    const std::string username =
        search.username.empty() ? std::string{} : normalize_chesscom_username(search.username);
    if (!search.month.empty() && !valid_chesscom_month(search.month))
        throw Error(ErrorCode::InvalidArgument, "Chess.com archive month is invalid");
    const std::size_t limit = std::min(search.limit, chesscom_archive_search_limit);
    std::vector<const ChessComArchiveEntry*> matches;
    for (const auto& [_, entry] : chesscom_archive_) {
        if ((!username.empty() && entry.username != username) ||
            (!search.month.empty() && entry.month != search.month) ||
            (!search.time_class.empty() && entry.time_class != search.time_class) ||
            (search.ended_after_ms > 0 && entry.end_time_ms < search.ended_after_ms) ||
            (search.ended_before_ms > 0 && entry.end_time_ms > search.ended_before_ms))
            continue;
        matches.push_back(&entry);
    }
    std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
        if (left->end_time_ms != right->end_time_ms)
            return left->end_time_ms > right->end_time_ms;
        return left->game_id < right->game_id;
    });
    ChessComArchivePage page;
    if (search.offset >= matches.size() || limit == 0) {
        page.next_offset = std::min(search.offset, matches.size());
        page.has_more = search.offset < matches.size();
        return page;
    }
    std::size_t cursor = search.offset;
    std::size_t copied_pgn_bytes = 0;
    page.entries.reserve(std::min(limit, matches.size() - cursor));
    while (cursor < matches.size() && page.entries.size() < limit) {
        const ChessComArchiveEntry& source = *matches[cursor];
        if (search.include_pgn && !page.entries.empty() &&
            source.pgn.size() > chesscom_archive_chunk_encoded_byte_limit - copied_pgn_bytes)
            break;
        page.entries.push_back(source);
        if (search.include_pgn) {
            copied_pgn_bytes += source.pgn.size();
        } else {
            page.entries.back().pgn.clear();
        }
        ++cursor;
    }
    page.next_offset = cursor;
    page.has_more = cursor < matches.size();
    return page;
}

void EventLogRepository::checkpoint_chesscom_month(ChessComMonthCheckpoint checkpoint) {
    std::lock_guard lock(mutex_);
    checkpoint.username = normalize_chesscom_username(checkpoint.username);
    if (!valid_chesscom_month_checkpoint(checkpoint))
        throw Error(ErrorCode::InvalidArgument, "Chess.com month checkpoint is invalid");
    const std::string key = checkpoint.username + "\n" + checkpoint.month;
    const auto existing = chesscom_checkpoints_.find(key);
    if (existing != chesscom_checkpoints_.end()) {
        if (existing->second == checkpoint)
            return;
        if (checkpoint.completed_at_ms < existing->second.completed_at_ms)
            throw Error(ErrorCode::InvalidArgument, "Chess.com month checkpoint is stale");
    }
    const storage::Event checkpoint_event =
        log_.append(storage::EventType::ChessComMonthCheckpointed,
                    json::dump(checkpoint_json(checkpoint)));
    chesscom_checkpoints_.insert_or_assign(key, std::move(checkpoint));
    note_applied_event(checkpoint_event);
    rebuild_indexes();
}

std::optional<ChessComMonthCheckpoint>
EventLogRepository::chesscom_month_checkpoint(std::string_view username, std::string_view month) const {
    std::lock_guard lock(mutex_);
    if (!valid_chesscom_month(month))
        throw Error(ErrorCode::InvalidArgument, "Chess.com archive month is invalid");
    const std::string key = normalize_chesscom_username(username) + "\n" + std::string(month);
    const auto found = chesscom_checkpoints_.find(key);
    return found == chesscom_checkpoints_.end()
               ? std::nullopt
               : std::optional<ChessComMonthCheckpoint>(found->second);
}

std::vector<ChessComMonthCheckpoint>
EventLogRepository::chesscom_month_checkpoints(std::string_view username) const {
    std::lock_guard lock(mutex_);
    const std::string normalized =
        username.empty() ? std::string{} : normalize_chesscom_username(username);
    std::vector<ChessComMonthCheckpoint> result;
    for (const auto& [_, checkpoint] : chesscom_checkpoints_)
        if (normalized.empty() || checkpoint.username == normalized)
            result.push_back(checkpoint);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.username != right.username)
            return left.username < right.username;
        return left.month < right.month;
    });
    return result;
}

void EventLogRepository::save_chesscom_sync_state(ChessComSyncState state) {
    std::lock_guard lock(mutex_);
    static const std::set<std::string> statuses{
        "idle", "running", "paused", "succeeded", "failed"};
    if (!statuses.contains(state.status) || state.started_at_ms < 0 || state.updated_at_ms < 0 ||
        (!state.current_month.empty() && !valid_chesscom_month(state.current_month)))
        throw Error(ErrorCode::InvalidArgument, "Chess.com sync state is invalid");
    if (!state.username.empty())
        state.username = normalize_chesscom_username(state.username);
    state.cursor = validate_cursor(state.cursor);
    state.last_error = sanitize_error(state.last_error);
    if (state.updated_at_ms < chesscom_sync_state_.updated_at_ms)
        throw Error(ErrorCode::InvalidArgument, "Chess.com sync state is stale");
    if (state == chesscom_sync_state_)
        return;
    const storage::Event sync_event =
        log_.append(storage::EventType::ChessComSyncStateChanged,
                    json::dump(sync_state_json(state)));
    chesscom_sync_state_ = std::move(state);
    note_applied_event(sync_event);
    rebuild_indexes();
}

ChessComSyncState EventLogRepository::chesscom_sync_state() const {
    std::lock_guard lock(mutex_);
    return chesscom_sync_state_;
}

Variation EventLogRepository::create_variation(std::string_view game_id, std::size_t root_ply,
                                       std::string root_position) {
    std::lock_guard lock(mutex_);
    const auto game = games_.find(std::string(game_id));
    if (game == games_.end())
        throw Error(ErrorCode::NotFound, "game does not exist");
    if (root_ply >= game->second.imported.game.plies.size())
        throw Error(ErrorCode::InvalidArgument, "variation root ply is outside the game");
    if (root_position != "before" && root_position != "after")
        throw Error(ErrorCode::InvalidArgument, "variation root position must be before or after");
    const auto& ply = game->second.imported.game.plies[root_ply];
    Variation variation;
    variation.id = "variation-" + std::to_string(next_variation_id_++);
    variation.game_id = std::string(game_id);
    variation.root_ply = root_ply;
    variation.root_position = std::move(root_position);
    variation.root_fen = variation.root_position == "before" ? ply.fen_before : ply.fen_after;
    variation.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    variation.nodes.emplace(0, VariationNode{0, -1, {}, {}, variation.root_fen, {}});
    const storage::Event event = log_.append(storage::EventType::VariationSaved,
                                             json::dump(to_json(variation)));
    variations_.emplace(variation.id, variation);
    note_applied_event(event);
    return variation;
}

Variation EventLogRepository::extend_variation(std::string_view variation_id, std::uint64_t node_id,
                                       std::string_view move_uci) {
    std::lock_guard lock(mutex_);
    const auto found = variations_.find(std::string(variation_id));
    if (found == variations_.end())
        throw Error(ErrorCode::NotFound, "variation does not exist");
    Variation updated = found->second;
    const auto parent = updated.nodes.find(node_id);
    if (parent == updated.nodes.end())
        throw Error(ErrorCode::NotFound, "variation node does not exist");
    chess::Board board = chess::Board::from_fen(parent->second.fen);
    const auto move = parse_uci(board, move_uci);
    if (!move)
        throw Error(ErrorCode::IllegalMove, "move is not legal in the variation position");
    const std::string canonical_uci = chess::uci(*move);
    for (const std::uint64_t child_id : parent->second.children) {
        const auto child = updated.nodes.find(child_id);
        if (child != updated.nodes.end() && child->second.uci == canonical_uci) {
            updated.current_node_id = child_id;
            updated.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
            const storage::Event event = log_.append(storage::EventType::VariationSaved,
                                                     json::dump(to_json(updated)));
            found->second = updated;
            note_applied_event(event);
            return updated;
        }
    }
    const std::string san = chess::to_san(board, *move);
    board.make_move(*move);
    const std::uint64_t child_id = updated.next_node_id++;
    updated.nodes.emplace(child_id, VariationNode{child_id, static_cast<std::int64_t>(node_id),
                                                   canonical_uci, san, board.to_fen(), {}});
    updated.nodes.at(node_id).children.push_back(child_id);
    updated.current_node_id = child_id;
    updated.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const storage::Event event = log_.append(storage::EventType::VariationSaved,
                                             json::dump(to_json(updated)));
    found->second = updated;
    note_applied_event(event);
    return updated;
}

Variation EventLogRepository::set_variation_cursor(std::string_view variation_id,
                                           std::uint64_t node_id) {
    std::lock_guard lock(mutex_);
    const auto found = variations_.find(std::string(variation_id));
    if (found == variations_.end())
        throw Error(ErrorCode::NotFound, "variation does not exist");
    if (!found->second.nodes.contains(node_id))
        throw Error(ErrorCode::NotFound, "variation node does not exist");
    Variation updated = found->second;
    updated.current_node_id = node_id;
    updated.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const storage::Event event = log_.append(storage::EventType::VariationSaved,
                                             json::dump(to_json(updated)));
    found->second = updated;
    note_applied_event(event);
    return updated;
}

Variation EventLogRepository::reset_variation(std::string_view variation_id) {
    return set_variation_cursor(variation_id, 0);
}

std::optional<Variation> EventLogRepository::variation(std::string_view variation_id) const {
    std::lock_guard lock(mutex_);
    const auto found = variations_.find(std::string(variation_id));
    return found == variations_.end() ? std::nullopt : std::optional<Variation>{found->second};
}

std::vector<Variation> EventLogRepository::variations(std::string_view game_id) const {
    std::lock_guard lock(mutex_);
    std::vector<Variation> result;
    for (const auto& [_, variation] : variations_)
        if (variation.game_id == game_id)
            result.push_back(variation);
    return result;
}

bool EventLogRepository::delete_variation(std::string_view variation_id) {
    std::lock_guard lock(mutex_);
    const auto found = variations_.find(std::string(variation_id));
    if (found == variations_.end())
        return false;
    const storage::Event event = log_.append(
        storage::EventType::VariationDeleted,
        json::dump(json::Value::Object{{"id", std::string(variation_id)},
                                       {"game_id", found->second.game_id}}));
    variations_.erase(found);
    note_applied_event(event);
    return true;
}

ReviewAttempt EventLogRepository::record_review_attempt(std::string_view game_id, std::size_t ply,
                                                 std::string_view uci) {
    std::lock_guard lock(mutex_);
    const auto game = games_.find(std::string(game_id));
    if (game == games_.end())
        throw Error(ErrorCode::NotFound, "game does not exist");
    if (!game->second.analysis || ply >= game->second.analysis->moves.size() ||
        ply >= game->second.imported.game.plies.size())
        throw Error(ErrorCode::InvalidArgument, "review attempt requires an analyzed move");
    chess::Board board = chess::Board::from_fen(game->second.imported.game.plies[ply].fen_before);
    const auto move = parse_uci(board, uci);
    if (!move)
        throw Error(ErrorCode::IllegalMove, "move is not legal in the retry position");
    const std::string canonical = chess::uci(*move);
    const auto& assessment = game->second.analysis->moves[ply];
    const bool accepted = canonical == assessment.best_uci ||
                          std::find(assessment.acceptable_alternatives.begin(),
                                    assessment.acceptable_alternatives.end(), canonical) !=
                              assessment.acceptable_alternatives.end();
    ReviewAttempt attempt;
    attempt.id = next_review_attempt_id_++;
    attempt.game_id = std::string(game_id);
    attempt.ply = ply;
    attempt.uci = canonical;
    attempt.accepted = accepted;
    attempt.attempted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    const storage::Event event = log_.append(storage::EventType::ReviewAttempted,
                                             json::dump(to_json(attempt)));
    review_attempts_.push_back(attempt);
    note_applied_event(event);
    return attempt;
}

std::vector<ReviewAttempt> EventLogRepository::review_attempts(std::string_view game_id) const {
    std::lock_guard lock(mutex_);
    std::vector<ReviewAttempt> result;
    for (const auto& attempt : review_attempts_)
        if (attempt.game_id == game_id)
            result.push_back(attempt);
    return result;
}

void EventLogRepository::rebuild_indexes() const {
    json::Value::Array games;
    json::Value::Array positions;
    json::Value::Array mistakes;
    json::Value::Array drills;
    json::Value::Array ratings;
    json::Value::Array chesscom_archive;
    json::Value::Array chesscom_checkpoints;
    for (const auto& [id, stored] : games_) {
        games.emplace_back(json::Value::Object{
            {"game_id", id},
            {"white", stored.imported.game.tag("White")},
            {"black", stored.imported.game.tag("Black")},
            {"plies", stored.imported.game.plies.size()},
        });
        for (const std::string color : {"White", "Black"}) {
            const std::string rating = stored.imported.game.tag(color + "Elo");
            if (rating.empty())
                continue;
            try {
                ratings.emplace_back(json::Value::Object{
                    {"game_id", id},
                    {"player", stored.imported.game.tag(color)},
                    {"color", color},
                    {"rating", std::stoi(rating)},
                    {"observed_at_ms", static_cast<double>(stored.imported_at_ms)},
                });
            } catch (const std::exception&) {
                // Invalid source ratings are ignored just as they are in the profile projection.
            }
        }
        if (!stored.imported.game.plies.empty()) {
            const auto& first = stored.imported.game.plies.front();
            positions.emplace_back(json::Value::Object{
                {"hash", hash_string(chess::Board::from_fen(first.fen_before).hash())},
                {"game_id", id},
                {"ply", 0},
            });
        }
        for (std::size_t ply = 0; ply < stored.imported.game.plies.size(); ++ply) {
            positions.emplace_back(json::Value::Object{
                {"hash",
                 hash_string(
                     chess::Board::from_fen(stored.imported.game.plies[ply].fen_after).hash())},
                {"game_id", id},
                {"ply", ply + 1},
            });
        }
        if (stored.analysis) {
            for (const auto& mistake : stored.analysis->mistakes) {
                mistakes.emplace_back(json::Value::Object{
                    {"game_id", id},
                    {"ply", mistake.ply},
                    {"category", mistake.category},
                });
            }
        }
    }
    for (const auto& [id, drill] : drills_) {
        drills.emplace_back(json::Value::Object{{"drill_id", id},
                                                 {"game_id", drill.source_game_id},
                                                 {"ply", drill.source_ply},
                                                 {"category", drill.category}});
    }
    for (const auto& [_, entry] : chesscom_archive_)
        chesscom_archive.push_back(chesscom_archive_json(entry));
    for (const auto& [_, checkpoint] : chesscom_checkpoints_)
        chesscom_checkpoints.push_back(checkpoint_json(checkpoint));
    const std::filesystem::path directory = log_.path().parent_path();
    json::Value::Array resources;
    for (const auto& resource : training::default_catalog()) {
        const auto completed = resource_completions_.find(resource.id);
        resources.emplace_back(json::Value::Object{
            {"resource_id", resource.id},
            {"kind", resource.kind},
            {"opening", resource.opening},
            {"recommended", recommended_resources_.contains(resource.id)},
            {"completed_at_ms",
             completed == resource_completions_.end()
                 ? json::Value{}
                 : json::Value(static_cast<double>(completed->second))},
        });
    }
    json::Value::Array snapshots;
    const std::filesystem::path snapshot_directory = directory / "snapshots";
    if (std::filesystem::exists(snapshot_directory)) {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(snapshot_directory))
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            json::Value::Object snapshot{{"path", path.filename().string()}, {"valid", false}};
            try {
                std::ifstream input(path);
                std::stringstream contents;
                contents << input.rdbuf();
                const auto value = json::parse(contents.str());
                snapshot.insert_or_assign("last_event_id", value.at("last_event_id"));
                snapshot.insert_or_assign("snapshot_version", value.at("snapshot_version"));
                snapshot.insert_or_assign("valid", true);
            } catch (const std::exception&) {
                // Snapshot files are accelerators; invalid ones are recorded but never authoritative.
            }
            snapshots.emplace_back(std::move(snapshot));
        }
    }
    write_index(directory / "games.idx",
                json::Value::Object{{"version", 1}, {"games", std::move(games)}});
    write_index(directory / "positions.idx",
                json::Value::Object{{"version", 1}, {"positions", std::move(positions)}});
    write_index(directory / "mistakes.idx",
                json::Value::Object{{"version", 1}, {"mistakes", std::move(mistakes)}});
    write_index(directory / "drills.idx",
                json::Value::Object{{"version", 1}, {"drills", std::move(drills)}});
    write_index(directory / "profile.idx",
                json::Value::Object{{"version", 1}, {"profile", training::to_json(profile_unlocked())}});
    write_index(directory / "resources.idx",
                json::Value::Object{{"version", 1},
                                    {"catalog_version", std::string(training::catalog_version)},
                                    {"resources", std::move(resources)}});
    write_index(directory / "ratings.idx",
                json::Value::Object{{"version", 1}, {"ratings", std::move(ratings)}});
    write_index(directory / "snapshots.idx",
                json::Value::Object{{"version", 1}, {"snapshots", std::move(snapshots)}});
    write_index(directory / "chesscom-profile.idx",
                json::Value::Object{
                    {"version", 1},
                    {"profile", chesscom_profile_ ? chesscom_profile_json(*chesscom_profile_)
                                                  : json::Value{}},
                    {"checkpoints", std::move(chesscom_checkpoints)},
                    {"sync_state", sync_state_json(chesscom_sync_state_)},
                });
    write_index(directory / "chesscom-archive.idx",
                json::Value::Object{{"version", 1},
                                    {"entries", std::move(chesscom_archive)}});
}

json::Value to_json(const Variation& variation) {
    json::Value::Array nodes;
    for (const auto& [_, node] : variation.nodes) {
        json::Value::Array children;
        for (const std::uint64_t child : node.children)
            children.emplace_back(static_cast<std::size_t>(child));
        nodes.emplace_back(json::Value::Object{
            {"id", static_cast<std::size_t>(node.id)},
            {"parent_id", static_cast<double>(node.parent_id)},
            {"uci", node.uci},
            {"san", node.san},
            {"fen", node.fen},
            {"children", std::move(children)},
        });
    }
    return json::Value::Object{
        {"id", variation.id},
        {"game_id", variation.game_id},
        {"root_ply", variation.root_ply},
        {"root_position", variation.root_position},
        {"root_fen", variation.root_fen},
        {"current_node_id", static_cast<std::size_t>(variation.current_node_id)},
        {"next_node_id", static_cast<std::size_t>(variation.next_node_id)},
        {"engine_configuration", variation.engine_configuration},
        {"updated_at_ms", static_cast<double>(variation.updated_at_ms)},
        {"nodes", std::move(nodes)},
    };
}

json::Value to_json(const ReviewAttempt& attempt) {
    return json::Value::Object{
        {"id", static_cast<std::size_t>(attempt.id)},
        {"game_id", attempt.game_id},
        {"ply", attempt.ply},
        {"uci", attempt.uci},
        {"accepted", attempt.accepted},
        {"attempted_at_ms", static_cast<double>(attempt.attempted_at_ms)},
    };
}

json::Value to_json(const chess::Game& game) {
    json::Value::Array plies;
    for (std::size_t index = 0; index < game.plies.size(); ++index) {
        const auto& ply = game.plies[index];
        plies.emplace_back(json::Value::Object{
            {"ply", index},
            {"san", ply.san},
            {"uci", chess::uci(ply.move)},
            {"fen_before", ply.fen_before},
            {"fen_after", ply.fen_after},
            {"clock_ms", ply.clock_ms ? json::Value(static_cast<double>(*ply.clock_ms))
                                        : json::Value{}},
            {"elapsed_ms", ply.elapsed_ms ? json::Value(static_cast<double>(*ply.elapsed_ms))
                                            : json::Value{}},
        });
    }
    json::Value::Object tags;
    for (const auto& [key, value] : game.tags)
        tags.emplace(key, value);
    return json::Value::Object{
        {"id", game.identity},
        {"tags", std::move(tags)},
        {"plies", std::move(plies)},
    };
}

json::Value to_json(const analysis::GameAnalysis& analysis) {
    json::Value::Array moves;
    for (const auto& move : analysis.moves)
        moves.push_back(move_json(move));
    json::Value::Array mistakes;
    for (const auto& mistake : analysis.mistakes)
        mistakes.push_back(mistake_json(mistake));
    return json::Value::Object{
        {"game_id", analysis.game_id},
        {"moves", std::move(moves)},
        {"mistakes", std::move(mistakes)},
        {"eco", analysis.eco},
        {"opening", analysis.opening},
        {"book_ply", analysis.book_ply},
        {"departure_ply", analysis.departure_ply ? json::Value(*analysis.departure_ply)
                                                   : json::Value{}},
        {"opening_book_version", analysis.opening_book_version},
        {"accuracy", analysis.accuracy},
        {"white_accuracy", analysis.white_accuracy},
        {"black_accuracy", analysis.black_accuracy},
        {"accuracy_sample_size", analysis.accuracy_sample_size},
        {"accuracy_version", analysis.accuracy_version},
        {"requested_engine_profile", analysis.requested_engine_profile},
        {"actual_engine_profile", analysis.actual_engine_profile},
        {"engine_name", analysis.engine_name},
        {"engine_source", analysis.engine_source},
        {"engine_hash", analysis.engine_hash},
    };
}

json::Value to_json(const StoredGame& stored, bool include_pgn) {
    json::Value::Object result{
        {"game", to_json(stored.imported.game)},
        {"source_url", stored.imported.source_url},
        {"import_method", method_name(stored.imported.method)},
        {"analysis_status", stored.analysis ? "complete"
                                             : stored.shallow_analysis ? "shallow" : "pending"},
        {"analysis", stored.analysis
                         ? to_json(*stored.analysis)
                         : stored.shallow_analysis ? to_json(*stored.shallow_analysis)
                                                   : json::Value{}},
        {"shallow_analysis", stored.shallow_analysis
                                 ? to_json(*stored.shallow_analysis)
                                 : json::Value{}},
        {"imported_at_ms", static_cast<double>(stored.imported_at_ms)},
        {"analyzed_at_ms", static_cast<double>(stored.analyzed_at_ms)},
    };
    if (include_pgn)
        result.emplace("pgn", stored.imported.pgn);
    return result;
}

analysis::GameAnalysis analysis_from_json(const json::Value& value) {
    analysis::GameAnalysis result;
    result.game_id = value.at("game_id").as_string();
    for (const auto& move : value.at("moves").as_array())
        result.moves.push_back(move_from_json(move));
    for (const auto& mistake : value.at("mistakes").as_array()) {
        result.mistakes.push_back(mistake_from_json(mistake));
    }
    result.eco = value.get("eco", "A00").as_string();
    result.opening = value.get("opening", "Uncommon Opening").as_string();
    result.book_ply = value.get("book_ply", 0).as_size();
    const json::Value null_value;
    const auto& departure = value.get("departure_ply", null_value);
    if (!departure.is_null())
        result.departure_ply = departure.as_size();
    result.opening_book_version =
        value.get("opening_book_version", "legacy").as_string();
    result.accuracy = value.get("accuracy", 0.0).as_number();
    result.white_accuracy = value.get("white_accuracy", 0.0).as_number();
    result.black_accuracy = value.get("black_accuracy", 0.0).as_number();
    result.accuracy_sample_size = value.get("accuracy_sample_size", 0).as_size();
    result.accuracy_version = value.get("accuracy_version", "legacy").as_string();
    result.requested_engine_profile = value.get("requested_engine_profile", "").as_string();
    result.actual_engine_profile = value.get("actual_engine_profile", "").as_string();
    result.engine_name = value.get("engine_name", "").as_string();
    result.engine_source = value.get("engine_source", "").as_string();
    result.engine_hash = value.get("engine_hash", "").as_string();
    return result;
}

} // namespace pct::app
