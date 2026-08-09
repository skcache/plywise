#include "pct/app/postgres_connection.hpp"

#include "pct/common/error.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace pct::app {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim_quotes(std::string value) {
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                              (value.front() == '"' && value.back() == '"')))
        return value.substr(1, value.size() - 2);
    return value;
}

struct ConnectionSecurity {
    std::string host;
    std::string sslmode;
    bool host_explicit{false};
    bool sslmode_explicit{false};
};

std::string uri_parameter(std::string_view query, std::string_view wanted) {
    while (!query.empty()) {
        if (query.front() == '?' || query.front() == '&')
            query.remove_prefix(1);
        const std::size_t separator = query.find('&');
        const std::string_view field = query.substr(0, separator);
        const std::size_t equals = field.find('=');
        if (equals != std::string_view::npos &&
            lowercase(std::string(field.substr(0, equals))) == wanted)
            return std::string(field.substr(equals + 1));
        if (separator == std::string_view::npos)
            break;
        query.remove_prefix(separator + 1);
    }
    return {};
}

ConnectionSecurity parse_uri(std::string_view connection) {
    ConnectionSecurity result;
    const std::size_t scheme_end = connection.find("://");
    if (scheme_end == std::string_view::npos)
        return result;
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t authority_end = connection.find_first_of("/?", authority_start);
    const std::string_view authority = connection.substr(
        authority_start, authority_end == std::string_view::npos
                             ? std::string_view::npos
                             : authority_end - authority_start);
    const std::size_t at = authority.rfind('@');
    std::string_view host_port = at == std::string_view::npos ? authority : authority.substr(at + 1);
    if (!host_port.empty()) {
        if (host_port.front() == '[') {
            const std::size_t close = host_port.find(']');
            if (close != std::string_view::npos)
                result.host = lowercase(std::string(host_port.substr(1, close - 1)));
        } else {
            const std::size_t colon = host_port.rfind(':');
            result.host = lowercase(std::string(
                host_port.substr(0, colon == std::string_view::npos ? host_port.size() : colon)));
        }
        result.host_explicit = !result.host.empty();
    }
    const std::size_t query_start = connection.find('?', authority_start);
    if (query_start != std::string_view::npos) {
        result.sslmode = lowercase(uri_parameter(connection.substr(query_start), "sslmode"));
        result.sslmode_explicit = !result.sslmode.empty();
    }
    return result;
}

ConnectionSecurity parse_keywords(std::string_view connection) {
    ConnectionSecurity result;
    std::string token;
    std::vector<std::string> tokens;
    for (std::size_t offset = 0; offset < connection.size();) {
        while (offset < connection.size() && std::isspace(static_cast<unsigned char>(connection[offset])) != 0)
            ++offset;
        if (offset >= connection.size())
            break;
        const std::size_t start = offset;
        bool quoted = false;
        char quote = '\0';
        while (offset < connection.size()) {
            const char character = connection[offset];
            if (!quoted && std::isspace(static_cast<unsigned char>(character)) != 0)
                break;
            if ((character == '\'' || character == '"') && (offset == start || connection[offset - 1] != '\\')) {
                if (!quoted) {
                    quoted = true;
                    quote = character;
                } else if (quote == character) {
                    quoted = false;
                }
            }
            ++offset;
        }
        tokens.emplace_back(connection.substr(start, offset - start));
    }
    for (const auto& raw : tokens) {
        const std::size_t equals = raw.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = lowercase(raw.substr(0, equals));
        const std::string value = lowercase(trim_quotes(raw.substr(equals + 1)));
        if (key == "host" || key == "hostaddr") {
            result.host = value;
            result.host_explicit = !value.empty();
        } else if (key == "sslmode") {
            result.sslmode = value;
            result.sslmode_explicit = !value.empty();
        }
    }
    return result;
}

bool local_host(std::string_view host) {
    return host.empty() || host.front() == '/' || host == "localhost" ||
           host == "127.0.0.1" || host == "::1";
}

} // namespace

std::string validate_postgres_connection_security(std::string connection_string) {
    if (connection_string.empty())
        throw Error(ErrorCode::InvalidArgument, "PostgreSQL connection is required");
    const ConnectionSecurity parsed = connection_string.find("://") != std::string::npos
                                          ? parse_uri(connection_string)
                                          : parse_keywords(connection_string);
    if (!parsed.host_explicit || local_host(parsed.host))
        return connection_string;
    if (!parsed.sslmode_explicit ||
        (parsed.sslmode != "require" && parsed.sslmode != "verify-ca" &&
         parsed.sslmode != "verify-full"))
        throw Error(ErrorCode::InvalidArgument,
                    "remote PostgreSQL connections must enable TLS with sslmode=require or stronger");
    return connection_string;
}

} // namespace pct::app
