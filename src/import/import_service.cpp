#include "pct/import/import_service.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <optional>
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

    return ChessComUrl{"https://www.chess.com" + std::string(path), std::string(game_id),
                       std::move(player), std::move(year), std::move(month)};
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
                if (!archive_failure)
                    archive_failure = std::current_exception();
            } catch (const Error&) {
                if (!archive_failure)
                    archive_failure = std::current_exception();
            }
        }
    }

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
        if (archive_failure)
            std::rethrow_exception(archive_failure);
        throw;
    } catch (const Error&) {
        if (cancellation.stop_requested())
            throw ChessComClientError(ChessComFailure::Cancelled, ErrorCode::NetworkError,
                                      "Chess.com request was cancelled");
        if (archive_failure)
            std::rethrow_exception(archive_failure);
        throw;
    } catch (...) {
        throw;
    }
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
