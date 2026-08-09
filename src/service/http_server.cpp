#include "pct/service/http_server.hpp"

#include "pct/common/error.hpp"
#include "pct/common/json.hpp"
#include "pct/common/log.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace pct::service {
namespace {

constexpr std::size_t max_header_size = 64 * 1024;
constexpr std::size_t max_body_size = 10 * 1024 * 1024;
constexpr std::string_view websocket_auth_protocol = "plywise-auth";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool imported_game_matches(const import::ImportedGame& imported,
                           std::string_view game_id) {
    try {
        if (import::ImportService::parse_chesscom_url(imported.source_url).game_id != game_id)
            return false;
        for (const std::string_view tag : {std::string_view{"Site"}, std::string_view{"Link"}}) {
            const std::string value = imported.game.tag(tag);
            if (value.empty()) continue;
            try {
                if (import::ImportService::parse_chesscom_url(value).game_id != game_id)
                    return false;
            } catch (const Error&) {
                // Generic current archive tags such as `[Site "Chess.com"]`
                // do not carry an identifier; the exact stored source URL does.
            }
        }
        return true;
    } catch (const Error&) {
        return false;
    }
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool valid_loopback_authority(std::string_view authority) {
    constexpr std::array<std::string_view, 2> allowed = {"127.0.0.1", "localhost"};
    for (const std::string_view host : allowed) {
        if (authority == host)
            return true;
        if (!authority.starts_with(host) || authority.size() <= host.size() ||
            authority[host.size()] != ':')
            continue;
        const std::string_view port = authority.substr(host.size() + 1);
        if (port.empty() || port.size() > 5)
            return false;
        unsigned value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
               value > 0 && value <= 65535;
    }
    return false;
}

bool valid_loopback_origin(std::string_view origin) {
    constexpr std::string_view scheme = "http://";
    return origin.starts_with(scheme) &&
           valid_loopback_authority(origin.substr(scheme.size()));
}

std::optional<std::string> websocket_bearer_token(std::string_view protocols) {
    const std::size_t separator = protocols.find(',');
    if (separator == std::string_view::npos)
        return std::nullopt;
    const std::string_view scheme = protocols.substr(0, separator);
    std::size_t token_start = separator + 1;
    while (token_start < protocols.size() && protocols[token_start] == ' ')
        ++token_start;
    const std::string_view token = protocols.substr(token_start);
    if (scheme != websocket_auth_protocol || token.empty() || token.size() > 4096 ||
        token.find(',') != std::string_view::npos ||
        std::any_of(token.begin(), token.end(), [](unsigned char character) {
            return std::isspace(character) != 0 || character < 0x20U || character == 0x7fU;
        }))
        return std::nullopt;
    return std::string(token);
}

bool valid_port(std::string_view value) {
    if (value.empty() || value.size() > 5)
        return false;
    unsigned port = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
           port > 0 && port <= 65535;
}

bool matches_authority(std::string_view authority, std::string_view allowed) {
    if (authority == allowed)
        return true;
    if (allowed.find(':') != std::string_view::npos || !authority.starts_with(allowed) ||
        authority.size() <= allowed.size() || authority[allowed.size()] != ':')
        return false;
    return valid_port(authority.substr(allowed.size() + 1));
}

bool valid_trusted_host(std::string_view host) {
    if (host.empty() || host.find_first_of("/\\?#@* \t\r\n") != std::string_view::npos)
        return false;
    const std::size_t colon = host.find(':');
    return colon == std::string_view::npos ||
           (host.find(':', colon + 1) == std::string_view::npos &&
            colon > 0 && valid_port(host.substr(colon + 1)));
}

bool valid_configured_origin(std::string_view origin) {
    if (valid_loopback_origin(origin))
        return true;
    constexpr std::string_view scheme = "https://";
    if (!origin.starts_with(scheme))
        return false;
    const std::string_view authority = origin.substr(scheme.size());
    return valid_trusted_host(authority) && authority.find('/') == std::string_view::npos;
}

std::string percent_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high < 0 || low < 0)
                throw Error(ErrorCode::InvalidArgument, "path contains invalid percent encoding");
            result.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            result.push_back(value[index]);
        }
    }
    return result;
}

std::vector<std::string> path_parts(std::string_view path) {
    const std::size_t query = path.find('?');
    path = path.substr(0, query);
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < path.size()) {
        while (offset < path.size() && path[offset] == '/')
            ++offset;
        if (offset >= path.size())
            break;
        const std::size_t end = path.find('/', offset);
        result.push_back(percent_decode(path.substr(offset, end - offset)));
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return result;
}

std::map<std::string, std::string> query_parameters(std::string_view path) {
    std::map<std::string, std::string> result;
    const std::size_t query = path.find('?');
    if (query == std::string_view::npos)
        return result;
    std::string_view fields = path.substr(query + 1);
    while (!fields.empty()) {
        const std::size_t ampersand = fields.find('&');
        const std::string_view field = fields.substr(0, ampersand);
        const std::size_t equals = field.find('=');
        if (equals == std::string_view::npos || equals == 0)
            throw Error(ErrorCode::InvalidArgument, "query parameter is malformed");
        const std::string key = percent_decode(field.substr(0, equals));
        const std::string value = percent_decode(field.substr(equals + 1));
        if (!result.emplace(key, value).second)
            throw Error(ErrorCode::InvalidArgument, "query parameter is duplicated");
        if (ampersand == std::string_view::npos)
            break;
        fields.remove_prefix(ampersand + 1);
    }
    return result;
}

Response json_response(int status, json::Value value) {
    return Response{
        status, {{"Content-Type", "application/json; charset=utf-8"}}, json::dump(value)};
}

json::Value engine_result_json(const engine::AnalysisResult& result) {
    json::Value::Array lines;
    for (const auto& line : result.lines) {
        json::Value::Array moves;
        for (const auto& move : line.moves)
            moves.emplace_back(move);
        lines.emplace_back(json::Value::Object{
            {"multipv", line.multipv},
            {"depth", line.depth},
            {"centipawns", line.centipawns ? json::Value(*line.centipawns) : json::Value{}},
            {"mate", line.mate ? json::Value(*line.mate) : json::Value{}},
            {"nodes", static_cast<double>(line.nodes)},
            {"time_ms", static_cast<double>(line.time_ms)},
            {"moves", std::move(moves)},
        });
    }
    return json::Value::Object{{"best_move", result.best_move},
                               {"ponder_move", result.ponder_move},
                               {"lines", std::move(lines)}};
}

Response error_response(int status, std::string message) {
    return json_response(status, json::Value::Object{{"error", std::move(message)}});
}

std::string_view error_code(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::ParseError: return "parse_error";
    case ErrorCode::IllegalMove: return "illegal_move";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::NetworkError: return "network_error";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::EngineError: return "engine_error";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::Corruption: return "corruption";
    }
    return "unknown";
}

Response error_response(int status, std::string message, ErrorCode code) {
    return json_response(status, json::Value::Object{{"error", std::move(message)},
                                                      {"code", std::string(error_code(code))}});
}

Response auth_response(int status, std::string message, std::string code) {
    Response response = json_response(status, json::Value::Object{
                                                    {"error", std::move(message)},
                                                    {"code", std::move(code)},
                                                });
    if (status == 401)
        response.headers.insert_or_assign("WWW-Authenticate", "Bearer");
    return response;
}

Response ingest_error(int status, std::string message, std::string code,
                      std::vector<std::string> actions = {}) {
    json::Value::Array encoded;
    for (auto& action : actions)
        encoded.emplace_back(std::move(action));
    return json_response(status, json::Value::Object{{"error", std::move(message)},
                                                      {"code", std::move(code)},
                                                      {"actions", std::move(encoded)}});
}

int status_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::ParseError:
    case ErrorCode::IllegalMove:
    case ErrorCode::Unsupported:
        return 400;
    case ErrorCode::NotFound:
        return 404;
    case ErrorCode::NetworkError:
        return 502;
    case ErrorCode::Timeout:
        return 504;
    case ErrorCode::EngineError:
    case ErrorCode::IoError:
    case ErrorCode::Corruption:
        return 500;
    }
    return 500;
}

std::uint64_t parse_id(std::string_view value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw Error(ErrorCode::InvalidArgument, "identifier must be numeric");
    }
    return result;
}

std::size_t query_size(const std::map<std::string, std::string>& query,
                       std::string_view key, std::size_t fallback) {
    const auto found = query.find(std::string(key));
    if (found == query.end())
        return fallback;
    return static_cast<std::size_t>(parse_id(found->second));
}

std::string reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Error";
    }
}

bool send_all(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::optional<Request> read_request(int fd) {
    std::string data;
    std::array<char, 8192> buffer{};
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0)
            return std::nullopt;
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > max_header_size)
            throw Error(ErrorCode::InvalidArgument, "HTTP headers are too large");
    }
    std::istringstream head(data.substr(0, header_end));
    Request request;
    std::string version;
    if (!(head >> request.method >> request.path >> version) || !version.starts_with("HTTP/1.")) {
        throw Error(ErrorCode::InvalidArgument, "invalid HTTP request line");
    }
    std::string line;
    std::getline(head, line);
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = lowercase(line.substr(0, colon));
        std::size_t start = colon + 1;
        while (start < line.size() && line[start] == ' ')
            ++start;
        request.headers.insert_or_assign(std::move(key), line.substr(start));
    }
    std::size_t content_length = 0;
    if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
        content_length = static_cast<std::size_t>(parse_id(found->second));
        if (content_length > max_body_size)
            throw Error(ErrorCode::InvalidArgument, "request body is too large");
    }
    request.body = data.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0)
            throw Error(ErrorCode::InvalidArgument, "request body ended early");
        request.body.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (request.body.size() > content_length)
        request.body.resize(content_length);
    return request;
}

std::array<std::uint32_t, 5> sha1(std::string_view input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while (data.size() % 64 != 56)
        data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(
            static_cast<std::uint8_t>((bit_length >> static_cast<unsigned>(shift)) & 0xffU));
    }
    std::array<std::uint32_t, 5> hash{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U,
                                      0xc3d2e1f0U};
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = chunk + index * 4;
            words[index] = (static_cast<std::uint32_t>(data[offset]) << 24U) |
                           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
                           static_cast<std::uint32_t>(data[offset + 3]);
        }
        for (std::size_t index = 16; index < 80; ++index) {
            words[index] = std::rotl(
                words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        }
        auto [a, b, c, d, e] = hash;
        for (std::size_t index = 0; index < 80; ++index) {
            std::uint32_t function = 0;
            std::uint32_t constant = 0;
            if (index < 20) {
                function = (b & c) | ((~b) & d);
                constant = 0x5a827999U;
            } else if (index < 40) {
                function = b ^ c ^ d;
                constant = 0x6ed9eba1U;
            } else if (index < 60) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8f1bbcdcU;
            } else {
                function = b ^ c ^ d;
                constant = 0xca62c1d6U;
            }
            const std::uint32_t temp = std::rotl(a, 5) + function + e + constant + words[index];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
    }
    return hash;
}

std::string websocket_accept(std::string_view key) {
    const auto hash = sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    std::array<std::uint8_t, 20> bytes{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        bytes[index * 4] = static_cast<std::uint8_t>(hash[index] >> 24U);
        bytes[index * 4 + 1] = static_cast<std::uint8_t>(hash[index] >> 16U);
        bytes[index * 4 + 2] = static_cast<std::uint8_t>(hash[index] >> 8U);
        bytes[index * 4 + 3] = static_cast<std::uint8_t>(hash[index]);
    }
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(index + 1 < bytes.size() ? bytes[index + 1] : 0) << 8U) |
            static_cast<std::uint32_t>(index + 2 < bytes.size() ? bytes[index + 2] : 0);
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6U) & 63U] : '=');
        output.push_back(index + 2 < bytes.size() ? alphabet[value & 63U] : '=');
    }
    return output;
}

std::string websocket_frame(std::string_view message) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (message.size() < 126) {
        frame.push_back(static_cast<char>(message.size()));
    } else if (message.size() <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((message.size() >> 8U) & 0xffU));
        frame.push_back(static_cast<char>(message.size() & 0xffU));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(
                static_cast<char>((message.size() >> static_cast<unsigned>(shift)) & 0xffU));
        }
    }
    frame.append(message);
    return frame;
}

std::string mime_type(const std::filesystem::path& path) {
    const std::string extension = lowercase(path.extension().string());
    if (extension == ".html")
        return "text/html; charset=utf-8";
    if (extension == ".js")
        return "text/javascript; charset=utf-8";
    if (extension == ".css")
        return "text/css; charset=utf-8";
    if (extension == ".json")
        return "application/json; charset=utf-8";
    if (extension == ".svg")
        return "image/svg+xml";
    if (extension == ".png")
        return "image/png";
    return "application/octet-stream";
}

} // namespace

Api::Authentication Api::authenticate(const Request& request) const {
    if (!auth_.required)
        return {};

    if (!auth_.verify)
        return Authentication{{}, auth_response(503, "authentication is not configured",
                                                 "auth_unavailable")};

    auto authorization = request.headers.find("authorization");
    if (authorization == request.headers.end())
        authorization = request.headers.find("Authorization");
    if (authorization == request.headers.end())
        return Authentication{{}, auth_response(401, "authentication is required",
                                                 "auth_required")};

    constexpr std::string_view bearer_prefix = "Bearer ";
    const std::string_view value = authorization->second;
    if (!value.starts_with(bearer_prefix))
        return Authentication{{}, auth_response(401, "bearer authentication is required",
                                                 "invalid_token")};
    const std::string_view token = value.substr(bearer_prefix.size());
    if (token.empty() || token.size() > 4096 ||
        std::any_of(token.begin(), token.end(), [](unsigned char character) {
            return std::isspace(character) != 0 || character < 0x20U || character == 0x7fU;
        })) {
        return Authentication{{}, auth_response(401, "bearer token is invalid", "invalid_token")};
    }

    std::optional<app::OwnerId> owner;
    try {
        owner = auth_.verify(token);
    } catch (...) {
        // A verifier must never leak provider details or token material through this boundary.
        return Authentication{{}, auth_response(401, "bearer token is invalid", "invalid_token")};
    }
    if (!owner)
        return Authentication{{}, auth_response(401, "bearer token is invalid", "invalid_token")};
    return Authentication{std::move(owner), {}};
}

std::optional<Response> Api::authorize(
    const Request& request, std::optional<app::OwnerId>* authenticated_owner) const {
    const Authentication authentication = authenticate(request);
    if (authenticated_owner)
        *authenticated_owner = authentication.owner;
    if (authentication.denial)
        return authentication.denial;
    // A scoped resolver owns the repository choice. The fixed-owner comparison remains for the
    // legacy local/one-owner hosted adapter and keeps this public helper backwards compatible.
    if (authentication.owner && !auth_.resolve_scope &&
        *authentication.owner != default_repository_.owner())
        return auth_response(403, "resource is outside the authenticated owner", "forbidden");
    return std::nullopt;
}

std::optional<ApiScope> Api::scope_for_owner(const app::OwnerId& owner) const {
    if (!auth_.resolve_scope) {
        if (owner == default_repository_.owner())
            return ApiScope{&default_repository_, &default_jobs_};
        return std::nullopt;
    }
    try {
        const auto scope = auth_.resolve_scope(owner);
        if (!scope || scope->repository == nullptr || scope->jobs == nullptr ||
            scope->repository->owner() != owner)
            return std::nullopt;
        return scope;
    } catch (...) {
        return std::nullopt;
    }
}

Response Api::handle(const Request& request) {
    try {
        const auto parts = path_parts(request.path);
        const bool guest_session_route =
            request.method == "POST" && parts == std::vector<std::string>{"api", "guest", "session"};
        const bool guest_claim_route =
            request.method == "POST" && parts == std::vector<std::string>{"api", "guest", "claim"};
        const bool public_route = guest_session_route ||
                                  (request.method == "GET" &&
                                   (parts == std::vector<std::string>{"api", "health"} ||
                                    parts == std::vector<std::string>{"api", "ready"}));
        ApiScope scope{&default_repository_, &default_jobs_};
        std::optional<app::OwnerId> authenticated_owner;
        if (!public_route) {
            const Authentication authentication = authenticate(request);
            if (authentication.denial)
                return *authentication.denial;
            authenticated_owner = authentication.owner;
            if (authentication.owner && !guest_claim_route) {
                if (auth_.resolve_scope) {
                    std::optional<ApiScope> resolved;
                    try {
                        resolved = auth_.resolve_scope(*authentication.owner);
                    } catch (...) {
                        return auth_response(503, "authenticated storage is unavailable",
                                             "storage_unavailable");
                    }
                    if (!resolved || resolved->repository == nullptr || resolved->jobs == nullptr ||
                        resolved->repository->owner() != *authentication.owner)
                        return auth_response(503, "authenticated storage is unavailable",
                                             "storage_unavailable");
                    scope = *resolved;
                } else if (*authentication.owner != default_repository_.owner()) {
                    return auth_response(403, "resource is outside the authenticated owner",
                                         "forbidden");
                }
            }
        }
        if (scope.repository == nullptr || scope.jobs == nullptr)
            return error_response(500, "request scope is unavailable");

        if (guest_session_route) {
            if (!auth_.create_guest_session)
                return auth_response(503, "guest sessions are not configured",
                                     "guest_sessions_unavailable");
            const auto session = auth_.create_guest_session();
            if (!session)
                return auth_response(503, "guest sessions are unavailable",
                                     "guest_sessions_unavailable");
            return json_response(201, json::Value::Object{
                                          {"guest_id", session->guest_id},
                                          {"token", session->token},
                                          {"expires_at_ms", static_cast<double>(session->expires_at_ms)},
                                      });
        }

        if (guest_claim_route) {
            if (!authenticated_owner || authenticated_owner->kind() != app::OwnerKind::Account)
                return auth_response(403, "account authentication is required", "account_required");
            if (!auth_.claim_guest)
                return auth_response(503, "guest claiming is not configured",
                                     "guest_claim_unavailable");
            const json::Value body = json::parse(request.body);
            for (const auto& [key, _] : body.as_object()) {
                if (key != "guest_token" && key != "idempotency_key")
                    throw Error(ErrorCode::InvalidArgument,
                                "guest claim accepts only guest_token and idempotency_key");
            }
            const std::string token = body.at("guest_token").as_string();
            std::string idempotency_key = body.get("idempotency_key", "").as_string();
            if (idempotency_key.empty()) {
                if (const auto found = request.headers.find("idempotency-key");
                    found != request.headers.end())
                    idempotency_key = found->second;
            }
            if (idempotency_key.empty())
                throw Error(ErrorCode::InvalidArgument, "guest claim requires an idempotency key");
            const GuestClaimResult result =
                auth_.claim_guest(token, *authenticated_owner, idempotency_key);
            return json_response(200, json::Value::Object{
                                          {"guest_id", result.guest_id},
                                          {"account_id", result.account_id},
                                          {"transferred_games", result.transferred_games},
                                          {"already_claimed", result.already_claimed},
                                      });
        }

        // Keep the route implementation below unchanged while selecting its dependencies per
        // request. These locals intentionally shadow the fixed constructor members.
        app::IRepository& repository_ = *scope.repository;
        app::JobManager& jobs_ = *scope.jobs;
        if (request.method == "GET" && parts == std::vector<std::string>{"api", "health"}) {
            const Readiness state = readiness_ ? readiness_() : Readiness{};
            json::Value::Object health{
                {"status", "ok"},
                {"version", "0.3.0"},
                {"service", "plywise-api"},
                {"local_only", state.local_only},
                {"auth_required", auth_.required},
            };
            if (!auth_.required)
                health.emplace("games", repository_.size());
            return json_response(200, std::move(health));
        }
        if (request.method == "GET" && parts == std::vector<std::string>{"api", "ready"}) {
            const Readiness state = readiness_ ? readiness_() : Readiness{};
            const bool ready = state.storage_ready && state.engine_ready;
            return json_response(
                ready ? 200 : 503,
                json::Value::Object{
                    {"status", ready ? "ready" : "not_ready"},
                    {"components",
                     json::Value::Object{
                         {"api", "ready"},
                         {"storage", state.storage_ready ? "ready" : "unavailable"},
                         {"engine", state.engine_ready ? "ready" : "unavailable"},
                     }},
                    {"engine", state.engine},
                });
        }
        if (request.method == "GET" &&
            parts == std::vector<std::string>{"api", "diagnostics"}) {
            json::Value result = diagnostics_ ? diagnostics_() : json::Value::Object{};
            result.as_object().insert_or_assign("job_workers", jobs_.worker_count());
            result.as_object().insert_or_assign("jobs_queued", jobs_.queued_count());
            result.as_object().insert_or_assign("job_queue_capacity", jobs_.queue_capacity());
            result.as_object().insert_or_assign("analysis_cache_hits", jobs_.cache_hits());
            result.as_object().insert_or_assign("analysis_cache_misses", jobs_.cache_misses());
            result.as_object().insert_or_assign("analysis_cache_evictions",
                                                jobs_.cache_evictions());
            result.as_object().insert_or_assign("analysis_cache_entries", jobs_.cache_size());
            result.as_object().insert_or_assign("analysis_cache_capacity",
                                                jobs_.cache_capacity());
            return json_response(200, std::move(result));
        }
        if (parts == std::vector<std::string>{"api", "games"} && request.method == "GET") {
            json::Value::Array games;
            for (const auto& game : repository_.list())
                games.push_back(to_json(game));
            return json_response(200, json::Value::Object{{"games", std::move(games)}});
        }
        if (parts == std::vector<std::string>{"api", "mistakes"} && request.method == "GET") {
            json::Value::Array mistakes;
            for (const auto& game : repository_.list()) {
                if (!game.analysis)
                    continue;
                const json::Value analysis = app::to_json(*game.analysis);
                for (const auto& mistake : analysis.at("mistakes").as_array()) {
                    mistakes.push_back(mistake);
                }
            }
            return json_response(200, json::Value::Object{{"mistakes", std::move(mistakes)}});
        }
        if (parts == std::vector<std::string>{"api", "settings"} && request.method == "GET") {
            return json_response(200, json::Value::Object{
                                          {"bind_address", "127.0.0.1"},
                                          {"shallow_depth", 10},
                                          {"deep_depth", 18},
                                          {"top_mistakes", 3},
                                          {"job_workers", jobs_.worker_count()},
                                          {"job_queue_capacity", jobs_.queue_capacity()},
                                          {"storage", "append_only_event_log"},
                                      });
        }
        if (parts == std::vector<std::string>{"api", "drills"} && request.method == "GET") {
            const auto current = now_ms();
            const auto drills = repository_.drills(current);
            std::map<std::string, std::size_t> frequency;
            for (const auto& drill : drills)
                ++frequency[drill.category];
            json::Value::Array values;
            for (const auto& drill : drills)
                values.push_back(training::to_json(drill, current, frequency[drill.category]));
            return json_response(200, json::Value::Object{{"drills", std::move(values)}});
        }
        if (parts == std::vector<std::string>{"api", "drills", "supplemental"} &&
            request.method == "POST") {
            if (!advanced_drills_)
                throw Error(ErrorCode::Unsupported, "optional tactical corpus is disabled");
            json::Value::Array generated;
            std::size_t added = 0;
            for (auto drill : advanced_drills_()) {
                if (repository_.add_validated_drill(drill))
                    ++added;
                generated.push_back(training::to_json(drill, now_ms()));
            }
            return json_response(200, json::Value::Object{{"added", added},
                                                           {"drills", std::move(generated)}});
        }
        if (parts.size() >= 3 && parts[0] == "api" && parts[1] == "drills") {
            const auto drill = repository_.drill(parts[2]);
            if (!drill)
                throw Error(ErrorCode::NotFound, "drill does not exist");
            if (parts.size() == 3 && request.method == "GET")
                return json_response(200, training::to_json(*drill, now_ms()));
            if (parts.size() == 4 && parts[3] == "session" && request.method == "POST") {
                return json_response(
                    200, training::to_json(repository_.begin_drill_session(parts[2], now_ms()),
                                            now_ms()));
            }
            if (parts.size() == 4 && parts[3] == "hint" && request.method == "POST") {
                return json_response(
                    200, training::to_json(repository_.advance_hint(parts[2], now_ms()),
                                            now_ms()));
            }
            if (parts.size() == 4 && parts[3] == "attempt" && request.method == "POST") {
                const json::Value body = json::parse(request.body);
                const auto attempt = repository_.record_attempt(
                    parts[2], body.at("move").as_string(),
                    static_cast<std::uint64_t>(body.get("response_time_ms", 0).as_number()),
                    body.get("hint_level", 0).as_int(), now_ms());
                const auto updated = repository_.drill(parts[2]);
                return json_response(200, json::Value::Object{
                                              {"attempt", training::to_json(attempt)},
                                              {"drill", training::to_json(*updated, now_ms())},
                                          });
            }
        }
        if (parts == std::vector<std::string>{"api", "profile"} && request.method == "GET")
            return json_response(200, training::to_json(repository_.profile()));
        if (parts == std::vector<std::string>{"api", "chesscom", "profile"}) {
            if (request.method == "GET") {
                const auto profile = repository_.chesscom_profile();
                return profile ? json_response(200, json::Value::Object{
                                                        {"connected", true},
                                                        {"profile", app::to_json(*profile)}})
                               : json_response(200, json::Value::Object{
                                                        {"connected", false},
                                                        {"profile", nullptr}});
            }
            if (request.method == "PUT") {
                if (!ingest_)
                    return ingest_error(503, "Chess.com ingest is unavailable",
                                        "ingest_unavailable", {"retry"});
                const json::Value body = json::parse(request.body);
                for (const auto& [key, _] : body.as_object()) {
                    if (key != "username" && key != "time_controls")
                        return ingest_error(400,
                                            "only a public username and time_controls are accepted",
                                            "sensitive_fields_forbidden");
                }
                std::vector<std::string> controls;
                const json::Value empty{json::Value::Array{}};
                for (const auto& control : body.get("time_controls", empty).as_array())
                    controls.push_back(control.as_string());
                ingest_->configure_profile(body.at("username").as_string(),
                                           std::move(controls));
                return json_response(200, json::Value::Object{
                                              {"connected", true},
                                              {"profile", app::to_json(*ingest_->profile())}});
            }
        }
        if (parts == std::vector<std::string>{"api", "chesscom", "sync"} &&
            request.method == "POST") {
            if (!ingest_)
                return ingest_error(503, "Chess.com ingest is unavailable",
                                    "ingest_unavailable", {"retry"});
            const json::Value body = json::parse(request.body);
            for (const auto& [key, _] : body.as_object()) {
                if (key != "days" && key != "username")
                    return ingest_error(400, "sync accepts only days and public username",
                                        "sensitive_fields_forbidden");
            }
            try {
                const auto sync = ingest_->start_sync(body.get("days", 30).as_int(),
                                                      body.get("username", "").as_string());
                return json_response(202, app::to_json(sync));
            } catch (const app::IngestConflict& error) {
                return ingest_error(409, error.what(), "sync_conflict", {"cancel_current"});
            }
        }
        if (parts.size() == 4 && parts[0] == "api" && parts[1] == "chesscom" &&
            parts[2] == "sync") {
            if (!ingest_)
                return ingest_error(503, "Chess.com ingest is unavailable",
                                    "ingest_unavailable", {"retry"});
            if (request.method == "GET") {
                const auto sync = ingest_->sync(parts[3]);
                if (!sync)
                    return ingest_error(404, "sync does not exist", "sync_not_found");
                return json_response(200, app::to_json(*sync));
            }
            if (request.method == "DELETE") {
                if (!ingest_->cancel_sync(parts[3]))
                    return ingest_error(404, "active sync does not exist", "sync_not_found");
                return json_response(200, app::to_json(*ingest_->sync(parts[3])));
            }
        }
        if (parts == std::vector<std::string>{"api", "chesscom", "archive"} &&
            request.method == "GET") {
            const auto query = query_parameters(request.path);
            static const std::set<std::string> allowed{
                "username", "month", "time_class", "ended_after_ms", "ended_before_ms",
                "offset", "limit"};
            for (const auto& [key, _] : query)
                if (!allowed.contains(key))
                    throw Error(ErrorCode::InvalidArgument, "unknown archive query field");
            app::ChessComArchiveSearch search;
            const auto field = [&](std::string_view key) {
                const auto found = query.find(std::string(key));
                return found == query.end() ? std::string{} : found->second;
            };
            search.username = field("username");
            search.month = field("month");
            search.time_class = field("time_class");
            search.ended_after_ms = static_cast<std::int64_t>(query_size(query, "ended_after_ms", 0));
            search.ended_before_ms = static_cast<std::int64_t>(query_size(query, "ended_before_ms", 0));
            search.offset = query_size(query, "offset", 0);
            search.limit = query_size(query, "limit", 50);
            const auto page = repository_.search_chesscom_archive(search);
            json::Value::Array entries;
            for (const auto& entry : page.entries)
                entries.push_back(app::to_json(entry));
            return json_response(200, json::Value::Object{{"entries", std::move(entries)},
                                                          {"next_offset", page.next_offset},
                                                          {"has_more", page.has_more},
                                                          {"limit", std::min(search.limit, app::chesscom_archive_search_limit)}});
        }
        if (parts == std::vector<std::string>{"api", "storage", "snapshot"} &&
            request.method == "POST") {
            const auto path = repository_.create_snapshot();
            return json_response(200, json::Value::Object{{"created", true},
                                                           {"snapshot", path.filename().string()}});
        }
        if (parts == std::vector<std::string>{"api", "storage", "compact"} &&
            request.method == "POST") {
            return json_response(200, json::Value::Object{
                                          {"compacted", true},
                                          {"events", repository_.compact_storage()},
                                      });
        }
        if (parts == std::vector<std::string>{"api", "resources"} && request.method == "GET") {
            json::Value::Array resources;
            for (const auto& recommendation : repository_.recommendations())
                resources.push_back(training::to_json(recommendation));
            return json_response(200, json::Value::Object{{"resources", std::move(resources)}});
        }
        if (parts.size() == 4 && parts[0] == "api" && parts[1] == "resources" &&
            parts[3] == "complete" && request.method == "POST") {
            repository_.complete_resource(parts[2], now_ms());
            return json_response(200, json::Value::Object{{"completed", true},
                                                           {"resource_id", parts[2]}});
        }
        if (parts == std::vector<std::string>{"api", "import"} && request.method == "POST") {
            const json::Value body = json::parse(request.body);
            import::ImportedGame imported;
            if (body.as_object().contains("url")) {
                const auto parsed = import::ImportService::parse_chesscom_url(
                    body.at("url").as_string());
                if (const auto local = repository_.chesscom_archive_entry(parsed.game_id)) {
                    imported = importer_.from_pgn(local->pgn, local->canonical_url);
                    if (!imported_game_matches(imported, parsed.game_id))
                        throw Error(ErrorCode::Corruption,
                                    "local archive PGN did not match its exact game identifier");
                } else if (ingest_) {
                    const auto resolution = ingest_->resolve(
                        body.at("url").as_string(), body.get("username", "").as_string());
                    return json_response(202, json::Value::Object{
                                                  {"status", "resolving"},
                                                  {"resolution_id", resolution.id},
                                                  {"resolution", app::to_json(resolution)}});
                } else {
                    imported = importer_.from_url(body.at("url").as_string());
                }
            } else if (body.as_object().contains("pgn")) {
                imported = importer_.from_pgn(body.at("pgn").as_string());
            } else {
                throw Error(ErrorCode::InvalidArgument, "request requires url or pgn");
            }
            const app::AddResult added = repository_.add(imported);
            return json_response(added == app::AddResult::Added ? 202 : 200,
                                 json::Value::Object{
                                     {"status", "imported"},
                                     {"duplicate", added == app::AddResult::Duplicate},
                                     {"game_id", imported.game.identity},
                                 });
        }
        if (parts == std::vector<std::string>{"api", "import", "resolve"} &&
            request.method == "POST") {
            if (!ingest_)
                return ingest_error(503, "Chess.com ingest is unavailable",
                                    "ingest_unavailable", {"retry", "paste_pgn"});
            const json::Value body = json::parse(request.body);
            for (const auto& [key, _] : body.as_object()) {
                if (key != "url" && key != "username")
                    return ingest_error(400, "resolution accepts only url and public username",
                                        "sensitive_fields_forbidden", {"paste_pgn"});
            }
            const auto resolution = ingest_->resolve(body.at("url").as_string(),
                                                     body.get("username", "").as_string());
            return json_response(202, app::to_json(resolution));
        }
        if (parts.size() == 4 && parts[0] == "api" && parts[1] == "import" &&
            parts[2] == "resolutions") {
            if (!ingest_)
                return ingest_error(503, "Chess.com ingest is unavailable",
                                    "ingest_unavailable", {"retry"});
            if (request.method == "GET") {
                const auto resolution = ingest_->resolution(parts[3]);
                if (!resolution)
                    return ingest_error(404, "resolution does not exist", "resolution_not_found");
                return json_response(200, app::to_json(*resolution));
            }
            if (request.method == "DELETE") {
                if (!ingest_->cancel_resolution(parts[3]))
                    return ingest_error(404, "active resolution does not exist",
                                        "resolution_not_found");
                return json_response(200, app::to_json(*ingest_->resolution(parts[3])));
            }
        }
        if (parts == std::vector<std::string>{"api", "import", "batch"} &&
            request.method == "POST") {
            const json::Value body = json::parse(request.body);
            const json::Value empty_sources{json::Value::Array{}};
            const auto& pgns = body.get("pgns", empty_sources).as_array();
            const auto& urls = body.get("urls", empty_sources).as_array();
            if (pgns.empty() && urls.empty())
                throw Error(ErrorCode::InvalidArgument, "batch requires pgns or urls");
            if (pgns.size() + urls.size() > 100)
                throw Error(ErrorCode::InvalidArgument,
                            "batch exceeds the 100-game backpressure limit");
            json::Value::Array game_ids;
            std::vector<std::string> batch_game_ids;
            json::Value::Array jobs;
            std::size_t imported_count = 0;
            std::size_t duplicate_count = 0;
            std::size_t failed_count = 0;
            for (const auto& pgn : pgns) {
                try {
                    const auto imported = importer_.from_pgn(pgn.as_string());
                    const auto added = repository_.add(imported);
                    added == app::AddResult::Added ? ++imported_count : ++duplicate_count;
                    game_ids.emplace_back(imported.game.identity);
                    batch_game_ids.push_back(imported.game.identity);
                } catch (const Error&) {
                    ++failed_count;
                }
            }
            for (const auto& url : urls) {
                try {
                    const auto imported = importer_.from_url(url.as_string());
                    const auto added = repository_.add(imported);
                    added == app::AddResult::Added ? ++imported_count : ++duplicate_count;
                    game_ids.emplace_back(imported.game.identity);
                    batch_game_ids.push_back(imported.game.identity);
                } catch (const Error&) {
                    ++failed_count;
                }
            }
            const std::size_t discovered = pgns.size() + urls.size();
            std::vector<std::string> queued_game_ids;
            for (const auto& game_id : batch_game_ids)
                if (std::find(queued_game_ids.begin(), queued_game_ids.end(), game_id) ==
                    queued_game_ids.end())
                    queued_game_ids.push_back(game_id);
            std::size_t queued_count = 0;
            for (const auto& job : jobs_.start_batch(queued_game_ids)) {
                jobs.push_back(app::to_json(job));
                if (job.status == app::JobStatus::Queued ||
                    job.status == app::JobStatus::Running)
                    ++queued_count;
            }
            const json::Value batch = repository_.create_batch(
                std::move(queued_game_ids), discovered, imported_count, duplicate_count, failed_count);
            return json_response(202, json::Value::Object{
                                          {"batch_id", batch.at("id").as_string()},
                                          {"discovered", discovered}, {"imported", imported_count},
                                          {"duplicates", duplicate_count}, {"queued", queued_count},
                                          {"failed", failed_count}, {"game_ids", std::move(game_ids)},
                                          {"jobs", std::move(jobs)},
                                      });
        }
        if (parts == std::vector<std::string>{"api", "batches"} && request.method == "GET") {
            json::Value batches = repository_.batches();
            batches.as_object().insert_or_assign("cache_hits", jobs_.cache_hits());
            return json_response(200, std::move(batches));
        }
        if (parts.size() >= 3 && parts[0] == "api" && parts[1] == "games") {
            const auto game = repository_.get(parts[2]);
            if (!game)
                throw Error(ErrorCode::NotFound, "game does not exist");
            if (parts.size() == 3 && request.method == "GET") {
                return json_response(200, to_json(*game, true));
            }
            if (parts.size() == 4 && parts[3] == "analysis" && request.method == "POST") {
                return json_response(202, app::to_json(jobs_.start(parts[2])));
            }
            if (parts.size() == 4 && parts[3] == "analysis" && request.method == "GET") {
                if (!game->analysis)
                    return json_response(202, json::Value::Object{{"status", "pending"}});
                return json_response(200, app::to_json(*game->analysis));
            }
            if (parts.size() == 4 && parts[3] == "retry-attempts" &&
                request.method == "GET") {
                json::Value::Array attempts;
                for (const auto& attempt : repository_.review_attempts(parts[2]))
                    attempts.emplace_back(app::to_json(attempt));
                return json_response(200,
                                     json::Value::Object{{"attempts", std::move(attempts)}});
            }
            if (parts.size() == 6 && parts[3] == "moves" && parts[5] == "retry" &&
                request.method == "POST") {
                const std::size_t ply = static_cast<std::size_t>(parse_id(parts[4]));
                const json::Value body = json::parse(request.body);
                return json_response(201, app::to_json(repository_.record_review_attempt(
                                              parts[2], ply, body.at("uci").as_string())));
            }
            if (parts.size() == 4 && parts[3] == "variations") {
                if (request.method == "POST") {
                    const json::Value body = json::parse(request.body);
                    const auto variation = repository_.create_variation(
                        parts[2], body.at("root_ply").as_size(),
                        body.get("root_position", "after").as_string());
                    return json_response(201, app::to_json(variation));
                }
                if (request.method == "GET") {
                    json::Value::Array variations;
                    for (const auto& variation : repository_.variations(parts[2]))
                        variations.emplace_back(app::to_json(variation));
                    return json_response(200, json::Value::Object{
                                                  {"variations", std::move(variations)}});
                }
            }
            if (parts.size() == 5 && parts[3] == "variations") {
                const auto variation = repository_.variation(parts[4]);
                if (!variation || variation->game_id != parts[2])
                    throw Error(ErrorCode::NotFound, "variation does not exist");
                if (request.method == "GET")
                    return json_response(200, app::to_json(*variation));
                if (request.method == "DELETE") {
                    static_cast<void>(repository_.delete_variation(parts[4]));
                    return json_response(200, json::Value::Object{{"deleted", true},
                                                                   {"id", parts[4]}});
                }
            }
            if (parts.size() == 6 && parts[3] == "variations" &&
                request.method == "POST") {
                const auto variation = repository_.variation(parts[4]);
                if (!variation || variation->game_id != parts[2])
                    throw Error(ErrorCode::NotFound, "variation does not exist");
                if (parts[5] == "moves") {
                    const json::Value body = json::parse(request.body);
                    return json_response(200, app::to_json(repository_.extend_variation(
                                                  parts[4],
                                                  static_cast<std::uint64_t>(
                                                      body.at("node_id").as_number()),
                                                  body.at("uci").as_string())));
                }
                if (parts[5] == "cursor") {
                    const json::Value body = json::parse(request.body);
                    return json_response(200, app::to_json(repository_.set_variation_cursor(
                                                  parts[4],
                                                  static_cast<std::uint64_t>(
                                                      body.at("node_id").as_number()))));
                }
                if (parts[5] == "reset")
                    return json_response(200,
                                         app::to_json(repository_.reset_variation(parts[4])));
                if (parts[5] == "analysis") {
                    const auto current = variation->nodes.find(variation->current_node_id);
                    if (current == variation->nodes.end())
                        throw Error(ErrorCode::Corruption,
                                    "variation cursor does not reference a node");
                    return json_response(
                        200, engine_result_json(jobs_.analyze_variation_position(
                                 current->second.fen)));
                }
            }
            if (parts.size() == 5 && parts[3] == "moves" && request.method == "GET") {
                const std::size_t ply = static_cast<std::size_t>(parse_id(parts[4]));
                if (ply >= game->imported.game.plies.size()) {
                    throw Error(ErrorCode::NotFound, "move does not exist");
                }
                const auto& move = game->imported.game.plies[ply];
                return json_response(200, json::Value::Object{
                                              {"ply", ply},
                                              {"san", move.san},
                                              {"uci", chess::uci(move.move)},
                                              {"fen_before", move.fen_before},
                                              {"fen_after", move.fen_after},
                                          });
            }
        }
        if (parts == std::vector<std::string>{"api", "jobs"} && request.method == "GET") {
            json::Value::Array jobs;
            for (const auto& job : jobs_.list())
                jobs.push_back(app::to_json(job));
            return json_response(200, json::Value::Object{{"jobs", std::move(jobs)},
                                                           {"paused", jobs_.paused()}});
        }
        if (parts == std::vector<std::string>{"api", "jobs", "pause"} &&
            request.method == "POST") {
            jobs_.pause();
            return json_response(200, json::Value::Object{{"paused", true}});
        }
        if (parts == std::vector<std::string>{"api", "jobs", "resume"} &&
            request.method == "POST") {
            jobs_.resume();
            return json_response(200, json::Value::Object{{"paused", false}});
        }
        if (parts.size() == 3 && parts[0] == "api" && parts[1] == "jobs") {
            const std::uint64_t id = parse_id(parts[2]);
            if (request.method == "GET") {
                const auto job = jobs_.get(id);
                if (!job)
                    throw Error(ErrorCode::NotFound, "job does not exist");
                return json_response(200, app::to_json(*job));
            }
            if (request.method == "DELETE") {
                if (!jobs_.cancel(id))
                    throw Error(ErrorCode::NotFound, "active job does not exist");
                return json_response(200, app::to_json(*jobs_.get(id)));
            }
        }
        return error_response(404, "route does not exist");
    } catch (const Error& error) {
        if (request.path.starts_with("/api/chesscom/") ||
            request.path.starts_with("/api/import/resolve") ||
            request.path.starts_with("/api/import/resolutions/"))
            return ingest_error(status_for(error.code()), error.what(), "invalid_request");
        return error_response(status_for(error.code()), error.what(), error.code());
    } catch (const std::exception& error) {
        log(LogLevel::Error, "api", error.what());
        return error_response(500, "internal server error");
    }
}

HttpServer::HttpServer(Api& api, app::JobManager& jobs, ServerOptions options,
                       app::IngestManager* ingest)
    : api_(api), jobs_(jobs), ingest_(ingest), options_(std::move(options)) {
    in_addr parsed_address{};
    if (inet_pton(AF_INET, options_.bind_address.c_str(), &parsed_address) != 1) {
        throw Error(ErrorCode::InvalidArgument,
                    "bind address must be a valid IPv4 address");
    }
    for (const auto& host : options_.trusted_hosts) {
        if (!valid_trusted_host(host))
            throw Error(ErrorCode::InvalidArgument, "invalid trusted host: " + host);
    }
    for (const auto& origin : options_.allowed_origins) {
        if (!valid_configured_origin(origin))
            throw Error(ErrorCode::InvalidArgument, "invalid allowed origin: " + origin);
    }
    jobs_.set_observer([this](const app::AnalysisJob& job) {
        broadcast(
            json::dump(json::Value::Object{{"type", "job_update"}, {"job", app::to_json(job)}}));
    });
    if (ingest_) {
        ingest_->set_observer([this](const json::Value& update) {
            broadcast(json::dump(update));
        });
    }
}

HttpServer::~HttpServer() {
    jobs_.set_observer({});
    if (ingest_)
        ingest_->set_observer({});
    remove_scoped_observers();
    stop();
    std::vector<std::thread> client_threads;
    {
        std::lock_guard lock(client_threads_mutex_);
        client_threads.swap(client_threads_);
    }
    for (auto& thread : client_threads)
        if (thread.joinable())
            thread.join();
}

void HttpServer::run() {
    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw Error(ErrorCode::IoError, "failed to create HTTP socket");
    int unpublished = -1;
    if (!listen_fd_.compare_exchange_strong(unpublished, listen_fd,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
        close(listen_fd);
        throw Error(ErrorCode::IoError, "HTTP server is already running");
    }
    const auto close_listener = [&] {
        int published = listen_fd;
        if (listen_fd_.compare_exchange_strong(
                published, -1, std::memory_order_acq_rel, std::memory_order_acquire))
            close(listen_fd);
    };
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != 1) {
        close_listener();
        throw Error(ErrorCode::InvalidArgument, "invalid HTTP bind address");
    }
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = std::strerror(errno);
        close_listener();
        throw Error(ErrorCode::IoError,
                    "failed to bind " + options_.bind_address + ":" +
                        std::to_string(options_.port) + ": " + message);
    }
    sockaddr_in bound_address{};
    socklen_t bound_size = sizeof(bound_address);
    if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound_address), &bound_size) != 0) {
        close_listener();
        throw Error(ErrorCode::IoError, "failed to read bound HTTP port");
    }
    if (listen(listen_fd, 32) != 0) {
        close_listener();
        throw Error(ErrorCode::IoError, "failed to listen on HTTP socket");
    }
    const std::uint16_t actual_port = ntohs(bound_address.sin_port);
    bound_port_.store(actual_port, std::memory_order_release);
    log(LogLevel::Info, "http", "listening on http://" + options_.bind_address + ":" +
                                 std::to_string(actual_port));
    while (!stopped_.load(std::memory_order_acquire)) {
        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (stopped_.load(std::memory_order_acquire) || errno == EBADF || errno == EINVAL)
                break;
            if (errno == EINTR)
                continue;
            log(LogLevel::Warning, "http", std::string("accept failed: ") + std::strerror(errno));
            continue;
        }
        std::lock_guard lock(client_threads_mutex_);
        client_threads_.emplace_back([this, client] { handle_client(client); });
    }
    close_listener();
}

void HttpServer::stop() noexcept {
    stopped_.store(true, std::memory_order_release);
    const int listen_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    if (listen_fd >= 0) {
        static_cast<void>(shutdown(listen_fd, SHUT_RDWR));
        close(listen_fd);
    }
    std::lock_guard lock(clients_mutex_);
    for (const auto& client : websocket_clients_)
        static_cast<void>(shutdown(client.fd, SHUT_RDWR));
}

void HttpServer::handle_client(int client_fd) {
    try {
        const auto request = read_request(client_fd);
        if (!request) {
            close(client_fd);
            return;
        }
        const auto host = request->headers.find("host");
        const auto origin = request->headers.find("origin");
        if (host == request->headers.end() || !host_allowed(host->second) ||
            (origin != request->headers.end() && !origin_allowed(origin->second))) {
            constexpr std::string_view forbidden =
                "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            static_cast<void>(send_all(client_fd, forbidden.data(), forbidden.size()));
            close(client_fd);
            return;
        }
        const auto upgrade = request->headers.find("upgrade");
        if (request->path == "/ws" && upgrade != request->headers.end() &&
            lowercase(upgrade->second) == "websocket") {
            handle_websocket(client_fd, *request);
            return;
        }
        Response response;
        if (request->method == "OPTIONS" && request->path.starts_with("/api/")) {
            response = Response{204, {}, {}};
            response.headers.insert_or_assign("Access-Control-Allow-Methods",
                                              "GET, POST, DELETE, OPTIONS");
            response.headers.insert_or_assign(
                "Access-Control-Allow-Headers",
                "Authorization, Content-Type, Idempotency-Key");
            response.headers.insert_or_assign("Access-Control-Max-Age", "600");
        } else {
            response = request->path.starts_with("/api/") ? api_.handle(*request)
                                                          : static_file(request->path);
        }
        if (origin != request->headers.end()) {
            response.headers.insert_or_assign("Access-Control-Allow-Origin", origin->second);
            response.headers.insert_or_assign("Vary", "Origin");
        }
        response.headers.insert_or_assign("Content-Length", std::to_string(response.body.size()));
        response.headers.insert_or_assign("Connection", "close");
        response.headers.insert_or_assign("X-Content-Type-Options", "nosniff");
        response.headers.insert_or_assign("X-Frame-Options", "DENY");
        response.headers.insert_or_assign("Referrer-Policy", "no-referrer");
        response.headers.insert_or_assign(
            "Permissions-Policy", "camera=(), microphone=(), geolocation=()");
        response.headers.insert_or_assign(
            "Content-Security-Policy",
            "default-src 'self'; base-uri 'none'; object-src 'none'; form-action 'self'; "
            "frame-ancestors 'none'; connect-src 'self' ws://127.0.0.1:*");
        std::ostringstream head;
        head << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n";
        for (const auto& [key, value] : response.headers)
            head << key << ": " << value << "\r\n";
        head << "\r\n";
        const std::string header = head.str();
        send_all(client_fd, header.data(), header.size());
        send_all(client_fd, response.body.data(), response.body.size());
    } catch (const std::exception& error) {
        const Response response = error_response(400, error.what());
        const std::string head =
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(response.body.size()) + "\r\nConnection: close\r\n\r\n";
        send_all(client_fd, head.data(), head.size());
        send_all(client_fd, response.body.data(), response.body.size());
    }
    close(client_fd);
}

void HttpServer::handle_websocket(int client_fd, const Request& request) {
    const auto send_http_response = [&](const Response& response) {
        std::ostringstream head;
        head << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status)
             << "\r\n";
        for (const auto& [key, value] : response.headers)
            head << key << ": " << value << "\r\n";
        head << "Content-Length: " << response.body.size()
             << "\r\nConnection: close\r\n\r\n";
        const std::string encoded = head.str();
        static_cast<void>(send_all(client_fd, encoded.data(), encoded.size()));
        static_cast<void>(send_all(client_fd, response.body.data(), response.body.size()));
    };

    Request authenticated_request = request;
    std::string selected_protocol;
    const auto authorization = request.headers.find("authorization");
    const auto protocols = request.headers.find("sec-websocket-protocol");
    if (authorization == request.headers.end() && protocols != request.headers.end()) {
        if (const auto token = websocket_bearer_token(protocols->second)) {
            authenticated_request.headers.insert_or_assign(
                "authorization", "Bearer " + *token);
            selected_protocol = std::string(websocket_auth_protocol);
        }
    }

    std::optional<app::OwnerId> owner;
    if (const auto denied = api_.authorize(authenticated_request, &owner)) {
        const Response response = *denied;
        send_http_response(response);
        close(client_fd);
        return;
    }

    if (api_.has_scoped_authorization() && !owner) {
        send_http_response(auth_response(401, "authentication is required", "auth_required"));
        close(client_fd);
        return;
    }

    app::JobManager* event_jobs = &jobs_;
    if (owner) {
        const auto scope = api_.scope_for_owner(*owner);
        if (!scope) {
            send_http_response(json_response(
                503, json::Value::Object{{"error", "owner-scoped event streaming is unavailable"},
                                         {"code", "events_unavailable"}}));
            close(client_fd);
            return;
        }
        event_jobs = scope->jobs;
    }

    const auto origin = request.headers.find("origin");
    if (origin == request.headers.end() || !origin_allowed(origin->second)) {
        constexpr std::string_view forbidden =
            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        static_cast<void>(send_all(client_fd, forbidden.data(), forbidden.size()));
        close(client_fd);
        return;
    }
    timeval send_timeout{1, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    const auto key = request.headers.find("sec-websocket-key");
    if (key == request.headers.end()) {
        close(client_fd);
        return;
    }
    const std::string accept = websocket_accept(key->second);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                 accept +
                                 (selected_protocol.empty()
                                      ? "\r\n\r\n"
                                      : "\r\nSec-WebSocket-Protocol: " + selected_protocol +
                                            "\r\n\r\n");
    if (!send_all(client_fd, response.data(), response.size())) {
        close(client_fd);
        return;
    }
    if (owner)
        subscribe_to_owner_events(*event_jobs, *owner);
    {
        std::lock_guard lock(clients_mutex_);
        websocket_clients_.push_back(WebSocketClient{client_fd, owner});
    }
    json::Value::Array jobs;
    for (const auto& job : event_jobs->list())
        jobs.push_back(app::to_json(job));
    const std::string snapshot = websocket_frame(
        json::dump(json::Value::Object{{"type", "jobs_snapshot"}, {"jobs", std::move(jobs)}}));
    if (!send_all(client_fd, snapshot.data(), snapshot.size())) {
        std::lock_guard lock(clients_mutex_);
        std::erase_if(websocket_clients_,
                      [&](const WebSocketClient& client) { return client.fd == client_fd; });
        close(client_fd);
        return;
    }
    if (ingest_ && !owner) {
        const std::string ingest_snapshot = websocket_frame(json::dump(json::Value::Object{
            {"type", "ingest_snapshot"}, {"ingest", ingest_->snapshot()}}));
        if (!send_all(client_fd, ingest_snapshot.data(), ingest_snapshot.size())) {
            std::lock_guard lock(clients_mutex_);
            std::erase_if(websocket_clients_,
                          [&](const WebSocketClient& client) { return client.fd == client_fd; });
            close(client_fd);
            return;
        }
    }
    std::array<char, 1024> buffer{};
    while (!stopped_.load(std::memory_order_acquire)) {
        pollfd descriptor{client_fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 1000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            break;
        if (ready == 0)
            continue;
        const ssize_t count = recv(client_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0)
            break;
        if ((static_cast<unsigned char>(buffer[0]) & 0x0fU) == 0x08U)
            break;
    }
    {
        std::lock_guard lock(clients_mutex_);
        std::erase_if(websocket_clients_,
                      [&](const WebSocketClient& client) { return client.fd == client_fd; });
    }
    close(client_fd);
}

void HttpServer::broadcast(std::string_view message) {
    if (api_.has_scoped_authorization())
        return;
    const std::string frame = websocket_frame(message);
    std::lock_guard lock(clients_mutex_);
    std::erase_if(websocket_clients_,
                  [&](const WebSocketClient& client) {
                      return !client.owner &&
                             !send_all(client.fd, frame.data(), frame.size());
                  });
}

void HttpServer::broadcast_to_owner(const app::OwnerId& owner, std::string_view message) {
    const std::string frame = websocket_frame(message);
    std::lock_guard lock(clients_mutex_);
    std::erase_if(websocket_clients_, [&](const WebSocketClient& client) {
        if (!client.owner || *client.owner != owner)
            return false;
        return !send_all(client.fd, frame.data(), frame.size());
    });
}

void HttpServer::subscribe_to_owner_events(app::JobManager& jobs, const app::OwnerId& owner) {
    std::lock_guard lock(scoped_observers_mutex_);
    if (scoped_observers_.contains(&jobs))
        return;
    const app::JobManager::ObserverId observer_id = jobs.add_observer(
        [this, owner](const app::AnalysisJob& job) {
            broadcast_to_owner(
                owner, json::dump(json::Value::Object{{"type", "job_update"},
                                                       {"job", app::to_json(job)}}));
        });
    if (observer_id != 0)
        scoped_observers_.emplace(&jobs, observer_id);
}

void HttpServer::remove_scoped_observers() noexcept {
    std::map<app::JobManager*, app::JobManager::ObserverId> observers;
    {
        std::lock_guard lock(scoped_observers_mutex_);
        observers.swap(scoped_observers_);
    }
    for (const auto& [jobs, observer_id] : observers) {
        if (jobs)
            jobs->remove_observer(observer_id);
    }
}

bool HttpServer::valid_websocket_origin(std::string_view origin) {
    return valid_loopback_origin(origin);
}

bool HttpServer::host_allowed(std::string_view host) const {
    if (valid_loopback_authority(host))
        return true;
    return std::any_of(options_.trusted_hosts.begin(), options_.trusted_hosts.end(),
                       [&](const std::string& allowed) {
                           return matches_authority(host, allowed);
                       });
}

bool HttpServer::origin_allowed(std::string_view origin) const {
    if (valid_loopback_origin(origin))
        return true;
    return std::find(options_.allowed_origins.begin(), options_.allowed_origins.end(), origin) !=
           options_.allowed_origins.end();
}

Response HttpServer::static_file(std::string_view request_path) const {
    const std::size_t query = request_path.find('?');
    request_path = request_path.substr(0, query);
    if (request_path.find("..") != std::string_view::npos ||
        request_path.find('\\') != std::string_view::npos) {
        return error_response(400, "invalid static-file path");
    }
    const std::string relative =
        request_path == "/" ? "index.html" : std::string(request_path.substr(1));
    const std::filesystem::path path = options_.web_root / relative;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return error_response(404, "file does not exist");
    const auto length = input.tellg();
    std::string body(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!body.empty())
        input.read(body.data(), length);
    return Response{200, {{"Content-Type", mime_type(path)}}, std::move(body)};
}

} // namespace pct::service
