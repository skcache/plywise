#include "pct/import/import_service.hpp"

#include "pct/chess/san.hpp"
#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <vector>

namespace pct::import {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool all_digits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> result;
    std::size_t offset = path.starts_with('/') ? 1 : 0;
    while (offset <= path.size()) {
        const std::size_t slash = path.find('/', offset);
        result.push_back(path.substr(offset, slash - offset));
        if (slash == std::string_view::npos)
            break;
        offset = slash + 1;
    }
    if (!result.empty() && result.back().empty())
        result.pop_back();
    return result;
}

void validate_username(std::string_view username) {
    static_cast<void>(ChessComArchiveClient::archive_index_url(username));
}

std::string percent_decode_query(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index] == '+' ? ' ' : value[index]);
            continue;
        }
        if (index + 2 >= value.size() || hex(value[index + 1]) < 0 || hex(value[index + 2]) < 0)
            throw Error(ErrorCode::InvalidArgument, "Chess.com URL has invalid query encoding");
        const char decoded = static_cast<char>((hex(value[index + 1]) << 4) | hex(value[index + 2]));
        if (decoded == '\0' || decoded == '\r' || decoded == '\n')
            throw Error(ErrorCode::InvalidArgument, "Chess.com URL has unsafe query encoding");
        result.push_back(decoded);
        index += 2;
    }
    return result;
}

std::map<std::string, std::string> parse_query(std::string_view query) {
    std::map<std::string, std::string> result;
    std::size_t offset = 0;
    while (offset < query.size()) {
        const std::size_t ampersand = query.find('&', offset);
        const std::string_view item = query.substr(offset, ampersand - offset);
        const std::size_t equals = item.find('=');
        if (item.empty() || equals == std::string_view::npos || equals == 0)
            throw Error(ErrorCode::InvalidArgument, "Chess.com URL has a malformed query");
        const std::string key = percent_decode_query(item.substr(0, equals));
        const std::string value = percent_decode_query(item.substr(equals + 1));
        if (!result.emplace(key, value).second)
            throw Error(ErrorCode::InvalidArgument, "Chess.com URL has duplicate query fields");
        if (ampersand == std::string_view::npos)
            break;
        offset = ampersand + 1;
    }
    return result;
}

bool pgn_matches_game(std::string_view pgn, std::string_view game_id) {
    if (game_id.empty())
        return true;
    try {
        const chess::Game game = chess::parse_pgn(pgn);
        const std::string site = game.tag("Site");
        if (site.empty())
            return false;
        return ImportService::parse_chesscom_url(site).game_id == game_id;
    } catch (const Error&) {
        return false;
    }
}

std::optional<std::string> pgn_from_json(const json::Value& value,
                                         std::string_view target_game_id) {
    if (value.is_array()) {
        for (const auto& child : value.as_array()) {
            if (auto pgn = pgn_from_json(child, target_game_id))
                return pgn;
        }
        return std::nullopt;
    }
    if (!value.is_object())
        return std::nullopt;

    const auto& object = value.as_object();
    const auto pgn = object.find("pgn");
    if (pgn != object.end()) {
        if (!pgn->second.is_string())
            throw Error(ErrorCode::ParseError, "Chess.com JSON field 'pgn' must be a string");
        bool url_match = false;
        const auto url = object.find("url");
        if (url != object.end()) {
            if (!url->second.is_string())
                throw Error(ErrorCode::ParseError, "Chess.com JSON field 'url' must be a string");
            try {
                url_match = ImportService::parse_chesscom_url(url->second.as_string()).game_id ==
                            target_game_id;
            } catch (const Error&) {
                url_match = false;
            }
        }
        const bool bound = target_game_id.empty() ||
                           (url != object.end()
                                ? url_match &&
                                      pgn_matches_game(pgn->second.as_string(), target_game_id)
                                : pgn_matches_game(pgn->second.as_string(), target_game_id));
        if (bound &&
            pgn->second.as_string().find("[Event ") != std::string::npos) {
            return pgn->second.as_string();
        }
    }
    for (const auto& [key, child] : object) {
        if (key != "pgn") {
            if (auto found = pgn_from_json(child, target_game_id))
                return found;
        }
    }
    return std::nullopt;
}

// Chess.com serves some finished-game pages as a client-rendered shell. The callback endpoint
// contains the canonical TCN move list, so decode it into the same validated PGN path used by
// archive and page imports. Keep the endpoint bounded and bind every response to the requested
// numeric game id before it reaches the chess parser.
constexpr std::string_view tcn_alphabet =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!?{~}(^)[_]@#$,./&-*++=";
constexpr std::string_view tcn_promotion_pieces = "qnrbkp";
constexpr std::size_t max_callback_ply_count = 4096;

std::optional<std::string> callback_string(const json::Value::Object& object,
                                           std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->second.is_string())
        return std::nullopt;
    return found->second.as_string();
}

std::string pgn_tag_value(const json::Value::Object& object, std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end())
        return {};
    if (found->second.is_string())
        return found->second.as_string();
    if (found->second.is_number()) {
        const double number = found->second.as_number();
        if (std::isfinite(number) && std::floor(number) == number &&
            number >= static_cast<double>(std::numeric_limits<long long>::min()) &&
            number <= static_cast<double>(std::numeric_limits<long long>::max()))
            return std::to_string(static_cast<long long>(number));
    }
    throw Error(ErrorCode::ParseError,
                "Chess.com callback field '" + std::string(key) + "' has an invalid type");
}

std::string escape_pgn_tag(std::string_view value) {
    if (value.size() > 512)
        throw Error(ErrorCode::ParseError, "Chess.com callback tag is too long");
    std::string escaped;
    escaped.reserve(value.size());
    for (const char raw_character : value) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if (character < 0x20U || character == 0x7fU)
            throw Error(ErrorCode::ParseError, "Chess.com callback tag contains control data");
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        escaped.push_back(static_cast<char>(character));
    }
    return escaped;
}

chess::PieceType promotion_type(char symbol) {
    switch (symbol) {
    case 'q': return chess::PieceType::Queen;
    case 'r': return chess::PieceType::Rook;
    case 'b': return chess::PieceType::Bishop;
    case 'n': return chess::PieceType::Knight;
    default: return chess::PieceType::None;
    }
}

std::string callback_square(int index) {
    if (index < 0 || index >= 64)
        throw Error(ErrorCode::ParseError, "Chess.com callback move contains an invalid square");
    return std::string(1, static_cast<char>('a' + index % 8)) +
           std::to_string(index / 8 + 1);
}

std::string callback_game_type_url(const ChessComUrl& parsed) {
    return "https://www.chess.com/callback/" + parsed.game_type + "/game/" + parsed.game_id;
}

std::string callback_pgn(const HttpResponse& response, const ChessComUrl& parsed) {
    if (response.body.size() > chesscom_max_body_size)
        throw ChessComClientError(ChessComFailure::BodyTooLarge, ErrorCode::NetworkError,
                                  "Chess.com callback response exceeded the 10 MiB limit");
    if (response.status != 200)
        throw ChessComClientError(response.status == 404 ? ChessComFailure::NotFound
                                                         : ChessComFailure::Transport,
                                  response.status == 404 ? ErrorCode::NotFound
                                                         : ErrorCode::NetworkError,
                                  "Chess.com callback returned HTTP " +
                                      std::to_string(response.status), response.status);
    const std::string expected_url = callback_game_type_url(parsed);
    if (!response.effective_url.empty() && response.effective_url != expected_url)
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::NetworkError,
                                  "Chess.com callback changed the requested game");

    const json::Value root = json::parse(response.body);
    if (!root.is_object())
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback returned an invalid game object");
    const auto game_member = root.as_object().find("game");
    if (game_member == root.as_object().end() || !game_member->second.is_object())
        throw ChessComClientError(ChessComFailure::NotFound, ErrorCode::NotFound,
                                  "Chess.com callback did not return the requested game");
    const auto& game = game_member->second.as_object();

    std::uint64_t requested_id = 0;
    const auto parsed_id = std::from_chars(parsed.game_id.data(),
                                           parsed.game_id.data() + parsed.game_id.size(),
                                           requested_id);
    if (parsed_id.ec != std::errc{} ||
        parsed_id.ptr != parsed.game_id.data() + parsed.game_id.size())
        throw Error(ErrorCode::InvalidArgument, "Chess.com game identifier is out of range");
    const auto id = game.find("id");
    if (id == game.end())
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback game has no identifier");
    if (id->second.is_number()) {
        const double number = id->second.as_number();
        if (!std::isfinite(number) || number < 0 || std::floor(number) != number ||
            number > static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ||
            static_cast<std::uint64_t>(number) != requested_id)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::NetworkError,
                                      "Chess.com callback game identifier did not match");
    } else if (id->second.is_string()) {
        if (id->second.as_string() != parsed.game_id)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::NetworkError,
                                      "Chess.com callback game identifier did not match");
    } else {
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback game identifier has an invalid type");
    }

    const auto headers_member = game.find("pgnHeaders");
    const auto move_list = callback_string(game, "moveList");
    if (headers_member == game.end() || !headers_member->second.is_object() || !move_list ||
        move_list->empty() || move_list->size() % 2 != 0 ||
        move_list->size() / 2 > max_callback_ply_count)
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback game data is incomplete");
    if (const auto ply_count = game.find("plyCount"); ply_count != game.end()) {
        if (!ply_count->second.is_number())
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback ply count has an invalid type");
        const double value = ply_count->second.as_number();
        if (!std::isfinite(value) || std::floor(value) != value || value < 0 ||
            value > static_cast<double>(max_callback_ply_count) ||
            static_cast<std::size_t>(value) != move_list->size() / 2)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback ply count did not match its move list");
    }
    const auto& headers = headers_member->second.as_object();
    const std::string white = pgn_tag_value(headers, "White");
    const std::string black = pgn_tag_value(headers, "Black");
    const std::string result = pgn_tag_value(headers, "Result");
    if (white.empty() || black.empty() || result.empty())
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback game headers are incomplete");
    if (result == "*")
        throw Error(ErrorCode::Unsupported, "Chess.com game is not finished");
    if (result != "1-0" && result != "0-1" && result != "1/2-1/2")
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                  "Chess.com callback game has an invalid result");

    chess::Board board = chess::Board::initial();
    const std::string fen = pgn_tag_value(headers, "FEN");
    const std::string setup = pgn_tag_value(headers, "SetUp");
    if (setup == "1") {
        if (fen.empty())
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback setup game has no FEN");
        board = chess::Board::from_fen(fen);
    }

    std::ostringstream moves;
    for (std::size_t offset = 0; offset < move_list->size(); offset += 2) {
        const std::size_t source = tcn_alphabet.find((*move_list)[offset]);
        const std::size_t target_symbol = tcn_alphabet.find((*move_list)[offset + 1]);
        if (source == std::string_view::npos || target_symbol == std::string_view::npos)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback move uses an invalid TCN symbol");
        int target = static_cast<int>(target_symbol);
        if (source > 75)
            throw Error(ErrorCode::Unsupported, "Chess.com variant moves are not supported");
        chess::PieceType promotion = chess::PieceType::None;
        if (target > 63) {
            const std::size_t promotion_index = static_cast<std::size_t>((target - 64) / 3);
            if (promotion_index >= tcn_promotion_pieces.size())
                throw ChessComClientError(ChessComFailure::InvalidResponse,
                                          ErrorCode::ParseError,
                                          "Chess.com callback promotion is invalid");
            promotion = promotion_type(tcn_promotion_pieces[promotion_index]);
            const int direction = source < 16 ? -8 : 8;
            target = static_cast<int>(source) + direction + ((target - 1) % 3) - 1;
        }
        if (target < 0 || target >= 64)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback move target is invalid");
        const chess::Square from = chess::parse_square(callback_square(static_cast<int>(source)));
        const chess::Square to = chess::parse_square(callback_square(target));
        const auto move = board.find_legal_move(from, to, promotion);
        if (!move)
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::ParseError,
                                      "Chess.com callback contained an illegal move");
        if (board.side_to_move() == chess::Color::White)
            moves << board.fullmove_number() << ". ";
        else if (offset == 0)
            moves << board.fullmove_number() << "... ";
        moves << chess::to_san(board, *move) << ' ';
        board.make_move(*move);
    }
    moves << result;

    std::ostringstream pgn;
    constexpr std::array<std::string_view, 14> tag_keys = {
        "Event", "Site", "Date", "Round", "White", "Black", "Result", "ECO",
        "WhiteElo", "BlackElo", "TimeControl", "Termination", "SetUp", "FEN"};
    for (const std::string_view key : tag_keys) {
        const std::string value = pgn_tag_value(headers, key);
        if (!value.empty())
            pgn << '[' << key << " \"" << escape_pgn_tag(value) << "\"]\n";
    }
    pgn << "[Link \"" << escape_pgn_tag(parsed.canonical) << "\"]\n\n" << moves.str();
    return pgn.str();
}

std::string html_unescape(std::string value) {
    const std::pair<std::string_view, std::string_view> entities[] = {
        {"&quot;", "\""}, {"&#34;", "\""}, {"&amp;", "&"},
        {"&lt;", "<"},    {"&gt;", ">"},   {"&#39;", "'"},
    };
    for (const auto& [encoded, decoded] : entities) {
        std::size_t offset = 0;
        while ((offset = value.find(encoded, offset)) != std::string::npos) {
            value.replace(offset, encoded.size(), decoded);
            offset += decoded.size();
        }
    }
    return value;
}

std::optional<std::string> html_value_after(std::string_view response,
                                            std::string_view marker,
                                            std::string_view terminator) {
    const std::size_t begin = response.find(marker);
    if (begin == std::string_view::npos) return std::nullopt;
    const std::size_t value_begin = begin + marker.size();
    const std::size_t end = response.find(terminator, value_begin);
    if (end == std::string_view::npos || end - value_begin > 512) return std::nullopt;
    return html_unescape(std::string(response.substr(value_begin, end - value_begin)));
}

std::optional<ChessComGamePlayers> players_from_title(std::string title) {
    constexpr std::string_view prefix = "Chess: ";
    if (!title.starts_with(prefix)) return std::nullopt;
    title.erase(0, prefix.size());
    if (title.ends_with(" - Chess.com"))
        title.erase(title.size() - std::string_view(" - Chess.com").size());
    const std::size_t versus = title.find(" vs ");
    if (versus == std::string::npos) return std::nullopt;
    ChessComGamePlayers players{title.substr(0, versus), title.substr(versus + 4)};
    try {
        validate_username(players.white);
        validate_username(players.black);
    } catch (const Error&) {
        return std::nullopt;
    }
    return players;
}

std::optional<std::string> pgn_from_html_scripts(std::string_view response,
                                                 std::string_view target_game_id) {
    std::size_t offset = 0;
    while ((offset = response.find("<script", offset)) != std::string_view::npos) {
        const std::size_t begin = response.find('>', offset + 7);
        const std::size_t end = begin == std::string_view::npos
                                    ? std::string_view::npos
                                    : response.find("</script>", begin + 1);
        if (begin == std::string_view::npos || end == std::string_view::npos)
            break;
        std::string_view payload = response.substr(begin + 1, end - begin - 1);
        while (!payload.empty() && std::isspace(static_cast<unsigned char>(payload.front())) != 0)
            payload.remove_prefix(1);
        if (payload.starts_with('{') || payload.starts_with('[')) {
            try {
                if (auto pgn = pgn_from_json(json::parse(payload), target_game_id))
                    return pgn;
            } catch (const Error&) {
                // Public pages contain unrelated scripts; this fallback remains best-effort.
            }
        }
        offset = end + 9;
    }
    return std::nullopt;
}

} // namespace

ImportService::ImportService(HttpGet get)
    : transport_(get ? HttpTransport([legacy = std::move(get)](const HttpRequest& request) {
          if (request.cancellation.stop_requested())
              throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                        "Chess.com request was cancelled");
          std::string body = legacy(request.url);
          if (body.size() > request.max_body_size)
              throw ChessComClientError(ChessComFailure::BodyTooLarge, ErrorCode::NetworkError,
                                        "Chess.com response exceeded the 10 MiB limit");
          return HttpResponse{200, {}, request.url, std::move(body)};
      })
                     : HttpTransport([](const HttpRequest& request) {
                           return curl_http_get(request);
                       })) {}

ImportService::ImportService(HttpTransport transport, RetrySleeper sleeper)
    : transport_(transport ? std::move(transport)
                           : HttpTransport([](const HttpRequest& request) {
                                 return curl_http_get(request);
                             })),
      sleeper_(std::move(sleeper)) {}

ChessComUrl ImportService::parse_chesscom_url(std::string_view input) {
    if (input.size() > 2048)
        throw Error(ErrorCode::InvalidArgument, "Chess.com URL is too long");
    if (input.find('#') != std::string_view::npos)
        throw Error(ErrorCode::InvalidArgument, "Chess.com game URL must not contain a fragment");
    constexpr std::string_view scheme = "https://";
    if (!input.starts_with(scheme))
        throw Error(ErrorCode::InvalidArgument, "Chess.com game URL must use HTTPS");

    const std::size_t path_start = input.find('/', scheme.size());
    const std::string_view authority = input.substr(
        scheme.size(), path_start == std::string_view::npos ? input.size() - scheme.size()
                                                            : path_start - scheme.size());
    if (authority.find_first_of("@:%\\") != std::string_view::npos ||
        (lowercase(std::string(authority)) != "chess.com" &&
         lowercase(std::string(authority)) != "www.chess.com")) {
        throw Error(ErrorCode::InvalidArgument, "URL host must be exactly chess.com");
    }
    if (path_start == std::string_view::npos)
        throw Error(ErrorCode::InvalidArgument, "Chess.com URL has no game path");

    const std::size_t query_start = input.find('?', path_start);
    const std::string_view path = input.substr(path_start, query_start - path_start);
    if (path.find_first_of("%\\") != std::string_view::npos)
        throw Error(ErrorCode::InvalidArgument, "Chess.com game path must not be encoded");
    const std::vector<std::string_view> parts = split_path(path);
    std::string_view game_id;
    if (parts.size() == 3 && parts[0] == "game" &&
        (parts[1] == "live" || parts[1] == "daily")) {
        game_id = parts[2];
    } else if ((parts.size() == 4 || parts.size() == 5) && parts[0] == "analysis" &&
               parts[1] == "game" && (parts[2] == "live" || parts[2] == "daily") &&
               (parts.size() == 4 || parts[4] == "analysis")) {
        game_id = parts[3];
    } else {
        throw Error(ErrorCode::InvalidArgument, "URL is not a supported Chess.com game URL");
    }
    if (game_id.size() > 32 || !all_digits(game_id))
        throw Error(ErrorCode::InvalidArgument, "Chess.com game identifier must be numeric");

    const auto query = query_start == std::string_view::npos
                           ? std::map<std::string, std::string>{}
                           : parse_query(input.substr(query_start + 1));
    auto value = [&](std::string_view key) {
        const auto found = query.find(std::string(key));
        return found == query.end() ? std::string{} : found->second;
    };
    std::string player = value("player");
    std::string year = value("year");
    std::string month = value("month");
    if (!player.empty())
        validate_username(player);
    if (!year.empty() || !month.empty()) {
        if (year.size() != 4 || !all_digits(year) || month.size() != 2 ||
            !all_digits(month) || month < "01" || month > "12") {
            throw Error(ErrorCode::InvalidArgument,
                        "Chess.com archive metadata requires a valid year and month");
        }
    }

    const std::string_view game_type = parts[0] == "game" ? parts[1] : parts[2];
    return ChessComUrl{"https://www.chess.com" + std::string(path), std::string(game_id),
                       std::move(player), std::move(year), std::move(month),
                       std::string(game_type)};
}

std::string ImportService::extract_pgn(std::string_view response,
                                       std::string_view target_game_id) {
    if (response.size() > chesscom_max_body_size)
        throw Error(ErrorCode::ParseError, "Chess.com response exceeded the 10 MiB limit");
    std::string_view trimmed = response;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)
        trimmed.remove_prefix(1);
    if (trimmed.starts_with('{') || trimmed.starts_with('[')) {
        const json::Value root = json::parse(trimmed);
        if (auto pgn = pgn_from_json(root, target_game_id))
            return *pgn;
        throw Error(ErrorCode::NotFound, "target game was not present in the Chess.com JSON");
    }
    if (auto pgn = pgn_from_html_scripts(response, target_game_id))
        return *pgn;

    const std::string decoded = html_unescape(std::string(response));
    const std::size_t start = decoded.find("[Event ");
    if (start != std::string::npos) {
        const std::size_t textarea_end = decoded.find("</textarea>", start);
        const std::string candidate = decoded.substr(
            start, textarea_end == std::string::npos ? decoded.size() - start
                                                     : textarea_end - start);
        if (pgn_matches_game(candidate, target_game_id))
            return candidate;
    }
    throw Error(ErrorCode::NotFound,
                "the public game page did not contain the requested game's PGN");
}

ImportedGame ImportService::from_url(std::string_view url,
                                     CancellationToken cancellation) const {
    const ChessComUrl parsed = parse_chesscom_url(url);
    std::exception_ptr archive_failure;
    bool archive_not_found = false;
    std::vector<std::string> archive_players;
    if (!parsed.player.empty()) {
        archive_players.push_back(parsed.player);
    } else {
        // A plain public game link does not carry a username, but its public page does. Use
        // those player names to look up the canonical PGN in the Chess.com archive before
        // falling back to scraping the page. This keeps hosted imports tenant-local while
        // retaining the archive's exact game-id binding.
        try {
            const auto players = discover_players(parsed.canonical, cancellation);
            archive_players = {players.white, players.black};
        } catch (const ChessComClientError& error) {
            if (error.failure() == ChessComFailure::Cancelled)
                throw;
        } catch (const Error&) {
            // Restricted/older public pages may not expose player metadata. The page fallback
            // below remains available for those links.
        }
    }
    if (!archive_players.empty()) {
        ChessComArchiveClient client(transport_, sleeper_);
        std::set<std::string> attempted_players;
        for (const auto& player : archive_players) {
            const std::string normalized = lowercase(player);
            if (!attempted_players.emplace(normalized).second)
                continue;
            try {
                const ChessComArchiveGame found = client.find_game(
                    player, parsed.game_id, parsed.year, parsed.month, cancellation);
                return ImportedGame{chess::parse_pgn(found.pgn), parsed.canonical, found.pgn,
                                    ImportMethod::PublicApi};
            } catch (const ChessComClientError& error) {
                if (error.failure() == ChessComFailure::Cancelled)
                    throw;
                if (error.failure() == ChessComFailure::NotFound ||
                    error.failure() == ChessComFailure::Gone)
                    archive_not_found = true;
                if (!archive_failure)
                    archive_failure = std::current_exception();
            } catch (const Error&) {
                if (!archive_failure)
                    archive_failure = std::current_exception();
            }
        }
    }

    const auto callback_import = [&]() {
        const std::string callback_url = callback_game_type_url(parsed);
        const HttpResponse callback = transport_(HttpRequest{callback_url, {},
                                                              chesscom_max_body_size,
                                                              cancellation});
        if (cancellation.stop_requested())
            throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                      "Chess.com request was cancelled");
        const std::string pgn = callback_pgn(callback, parsed);
        return ImportedGame{chess::parse_pgn(pgn), parsed.canonical, pgn,
                            ImportMethod::PublicApi};
    };

    std::exception_ptr callback_failure;
    bool callback_attempted = false;
    // A player's archive is frequently absent for a freshly-finished or client-rendered game.
    // Try the exact callback as soon as that bounded lookup confirms a miss.
    if (archive_not_found) {
        callback_attempted = true;
        try {
            return callback_import();
        } catch (const ChessComClientError& error) {
            if (error.failure() == ChessComFailure::Cancelled)
                throw;
            callback_failure = std::current_exception();
        } catch (const Error&) {
            if (cancellation.stop_requested())
                throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                          "Chess.com request was cancelled");
            callback_failure = std::current_exception();
        }
    }

    std::exception_ptr page_failure;
    try {
        if (cancellation.stop_requested())
            throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                      "Chess.com request was cancelled");
        HttpResponse page = transport_(HttpRequest{parsed.canonical, {}, chesscom_max_body_size,
                                                   cancellation});
        if (cancellation.stop_requested())
            throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                      "Chess.com request was cancelled");
        if (page.body.size() > chesscom_max_body_size)
            throw ChessComClientError(ChessComFailure::BodyTooLarge, ErrorCode::NetworkError,
                                      "Chess.com response exceeded the 10 MiB limit");
        if (page.status != 200)
            throw ChessComClientError(page.status == 404 ? ChessComFailure::NotFound
                                                         : ChessComFailure::Transport,
                                      page.status == 404 ? ErrorCode::NotFound
                                                         : ErrorCode::NetworkError,
                                      "Chess.com public page returned HTTP " +
                                          std::to_string(page.status),
                                      page.status);
        if (!page.effective_url.empty() &&
            parse_chesscom_url(page.effective_url).game_id != parsed.game_id) {
            throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::NetworkError,
                                      "Chess.com public-page redirect changed the target game");
        }
        const std::string pgn = extract_pgn(page.body, parsed.game_id);
        return ImportedGame{chess::parse_pgn(pgn), parsed.canonical, pgn,
                            ImportMethod::PublicPage};
    } catch (const ChessComClientError& error) {
        if (error.failure() == ChessComFailure::Cancelled)
            throw;
        page_failure = std::current_exception();
    } catch (const Error&) {
        if (cancellation.stop_requested())
            throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                      "Chess.com request was cancelled");
        page_failure = std::current_exception();
    } catch (...) {
        throw;
    }

    // Some public pages are only a React shell. The callback is the canonical last fallback,
    // and its response is still parsed through the C++ chess truth boundary above.
    if (!callback_attempted) {
        callback_attempted = true;
        try {
            return callback_import();
        } catch (const ChessComClientError& error) {
            if (error.failure() == ChessComFailure::Cancelled)
                throw;
            callback_failure = std::current_exception();
        } catch (const Error&) {
            if (cancellation.stop_requested())
                throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                          "Chess.com request was cancelled");
            callback_failure = std::current_exception();
        }
    }

    if (archive_failure)
        std::rethrow_exception(archive_failure);
    if (page_failure)
        std::rethrow_exception(page_failure);
    if (callback_failure)
        std::rethrow_exception(callback_failure);
    throw Error(ErrorCode::NotFound, "the requested Chess.com game could not be imported");
}

ImportedGame ImportService::from_pgn(std::string_view pgn, std::string_view source_url) const {
    if (pgn.size() > chesscom_max_body_size)
        throw Error(ErrorCode::InvalidArgument, "PGN exceeds the 10 MiB import limit");
    return ImportedGame{chess::parse_pgn(pgn), std::string(source_url), std::string(pgn),
                        ImportMethod::ManualPgn};
}

ChessComGamePlayers ImportService::extract_players(std::string_view response) {
    if (auto title = html_value_after(response, "<meta property=\"og:title\" content=\"", "\"")) {
        if (auto players = players_from_title(*title)) return *players;
    }
    if (auto title = html_value_after(response, "<title>", "</title>")) {
        if (auto players = players_from_title(*title)) return *players;
    }
    throw Error(ErrorCode::NotFound,
                "Chess.com game page did not expose the player usernames");
}

ChessComGamePlayers ImportService::discover_players(std::string_view url,
                                                     CancellationToken cancellation) const {
    const ChessComUrl parsed = parse_chesscom_url(url);
    if (cancellation.stop_requested())
        throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                  "Chess.com request was cancelled");
    HttpResponse page = transport_(HttpRequest{parsed.canonical, {}, chesscom_max_body_size,
                                               cancellation});
    if (cancellation.stop_requested())
        throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                  "Chess.com request was cancelled");
    if (page.body.size() > chesscom_max_body_size)
        throw ChessComClientError(ChessComFailure::BodyTooLarge, ErrorCode::NetworkError,
                                  "Chess.com response exceeded the 10 MiB limit");
    if (page.status != 200)
        throw ChessComClientError(page.status == 404 ? ChessComFailure::NotFound
                                                     : ChessComFailure::Transport,
                                  page.status == 404 ? ErrorCode::NotFound
                                                     : ErrorCode::NetworkError,
                                  "Chess.com public page returned HTTP " +
                                      std::to_string(page.status), page.status);
    if (!page.effective_url.empty() &&
        parse_chesscom_url(page.effective_url).game_id != parsed.game_id)
        throw ChessComClientError(ChessComFailure::InvalidResponse, ErrorCode::NetworkError,
                                  "Chess.com public-page redirect changed the target game");
    return extract_players(page.body);
}

std::string curl_get(const std::string& url) {
    HttpResponse response =
        curl_http_get(HttpRequest{url, {}, chesscom_max_body_size, CancellationToken{}});
    if (response.status != 200)
        throw Error(ErrorCode::NetworkError,
                    "Chess.com returned HTTP " + std::to_string(response.status));
    return response.body;
}

} // namespace pct::import
