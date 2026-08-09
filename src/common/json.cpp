#include "pct/common/json.hpp"

#include "pct/common/error.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace pct::json {
namespace {

[[noreturn]] void type_error(std::string_view expected) {
    throw Error(ErrorCode::ParseError, "JSON value is not " + std::string(expected));
}

class Parser {
  public:
    explicit Parser(std::string_view input) : input_(input) {}

    Value parse_document() {
        skip_space();
        Value result = parse_value();
        skip_space();
        if (offset_ != input_.size())
            fail("unexpected trailing data");
        return result;
    }

  private:
    static constexpr std::size_t max_depth = 64;
    std::string_view input_;
    std::size_t offset_{0};

    [[noreturn]] void fail(std::string_view message) const {
        throw Error(ErrorCode::ParseError, "invalid JSON at byte " + std::to_string(offset_) +
                                               ": " + std::string(message));
    }

    void skip_space() {
        while (offset_ < input_.size() && (input_[offset_] == ' ' || input_[offset_] == '\n' ||
                                           input_[offset_] == '\r' || input_[offset_] == '\t')) {
            ++offset_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (offset_ < input_.size() && input_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    Value parse_value(std::size_t depth = 0) {
        skip_space();
        if (offset_ >= input_.size())
            fail("expected a value");
        if (depth > max_depth)
            fail("nesting depth exceeds the supported limit");
        switch (input_[offset_]) {
        case 'n':
            return literal("null", Value{});
        case 't':
            return literal("true", Value(true));
        case 'f':
            return literal("false", Value(false));
        case '"':
            return Value(parse_string());
        case '[':
            return parse_array(depth);
        case '{':
            return parse_object(depth);
        default:
            if (input_[offset_] == '-' || (input_[offset_] >= '0' && input_[offset_] <= '9')) {
                return parse_number();
            }
            fail("unexpected character");
        }
    }

    Value literal(std::string_view word, Value value) {
        if (input_.substr(offset_, word.size()) != word)
            fail("invalid literal");
        offset_ += word.size();
        return value;
    }

    static void append_utf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    std::string parse_string() {
        if (!consume('"'))
            fail("expected string");
        std::string result;
        while (offset_ < input_.size()) {
            const char character = input_[offset_++];
            if (character == '"')
                return result;
            if (static_cast<unsigned char>(character) < 0x20U)
                fail("control character in string");
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (offset_ >= input_.size())
                fail("unterminated escape");
            switch (input_[offset_++]) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                if (offset_ + 4 > input_.size())
                    fail("short Unicode escape");
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[offset_++];
                    codepoint <<= 4U;
                    if (digit >= '0' && digit <= '9')
                        codepoint |= static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f')
                        codepoint |= static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F')
                        codepoint |= static_cast<unsigned>(digit - 'A' + 10);
                    else
                        fail("invalid Unicode escape");
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    Value parse_number() {
        const std::size_t start = offset_;
        if (input_[offset_] == '-')
            ++offset_;
        if (offset_ >= input_.size())
            fail("short number");
        if (input_[offset_] == '0') {
            ++offset_;
        } else {
            if (input_[offset_] < '1' || input_[offset_] > '9')
                fail("invalid number");
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9')
                ++offset_;
        }
        if (offset_ < input_.size() && input_[offset_] == '.') {
            ++offset_;
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("invalid fractional number");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9')
                ++offset_;
        }
        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-'))
                ++offset_;
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("invalid exponent");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9')
                ++offset_;
        }
        double value = 0;
        const auto text = input_.substr(start, offset_ - start);
        std::istringstream stream(std::string{text});
        stream.imbue(std::locale::classic());
        stream >> value;
        if (stream.fail() || stream.rdbuf()->sgetc() != std::char_traits<char>::eof() ||
            !std::isfinite(value)) {
            fail("number is outside the supported range");
        }
        return Value(value);
    }

    Value parse_array(std::size_t depth) {
        consume('[');
        Value::Array values;
        if (consume(']'))
            return Value(std::move(values));
        while (true) {
            values.push_back(parse_value(depth + 1));
            if (consume(']'))
                return Value(std::move(values));
            if (!consume(','))
                fail("expected comma in array");
        }
    }

    Value parse_object(std::size_t depth) {
        consume('{');
        Value::Object values;
        if (consume('}'))
            return Value(std::move(values));
        while (true) {
            skip_space();
            if (offset_ >= input_.size() || input_[offset_] != '"')
                fail("expected object key");
            std::string key = parse_string();
            if (!consume(':'))
                fail("expected colon after object key");
            if (!values.emplace(std::move(key), parse_value(depth + 1)).second)
                fail("duplicate object key");
            if (consume('}'))
                return Value(std::move(values));
            if (!consume(','))
                fail("expected comma in object");
        }
    }
};

void dump_string(std::ostringstream& output, std::string_view value) {
    output << '"';
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
}

void dump_value(std::ostringstream& output, const Value& value) {
    if (value.is_null())
        output << "null";
    else if (value.is_bool())
        output << (value.as_bool() ? "true" : "false");
    else if (value.is_number()) {
        const double number = value.as_number();
        if (number == std::floor(number) && std::abs(number) <= 9007199254740991.0) {
            output << std::fixed << std::setprecision(0) << number << std::defaultfloat;
        } else {
            output << std::setprecision(15) << number;
        }
    } else if (value.is_string())
        dump_string(output, value.as_string());
    else if (value.is_array()) {
        output << '[';
        bool first = true;
        for (const Value& item : value.as_array()) {
            if (!first)
                output << ',';
            first = false;
            dump_value(output, item);
        }
        output << ']';
    } else {
        output << '{';
        bool first = true;
        for (const auto& [key, item] : value.as_object()) {
            if (!first)
                output << ',';
            first = false;
            dump_string(output, key);
            output << ':';
            dump_value(output, item);
        }
        output << '}';
    }
}

} // namespace

bool Value::as_bool() const {
    if (!is_bool())
        type_error("a boolean");
    return std::get<bool>(value_);
}

double Value::as_number() const {
    if (!is_number())
        type_error("a number");
    return std::get<double>(value_);
}

int Value::as_int() const {
    const double value = as_number();
    if (value != std::floor(value) || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        type_error("an integer");
    }
    return static_cast<int>(value);
}

std::size_t Value::as_size() const {
    const double value = as_number();
    if (value != std::floor(value) || value < 0 ||
        value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        type_error("a non-negative integer");
    }
    return static_cast<std::size_t>(value);
}

const std::string& Value::as_string() const {
    if (!is_string())
        type_error("a string");
    return std::get<std::string>(value_);
}

const Value::Array& Value::as_array() const {
    if (!is_array())
        type_error("an array");
    return std::get<Array>(value_);
}

const Value::Object& Value::as_object() const {
    if (!is_object())
        type_error("an object");
    return std::get<Object>(value_);
}

Value::Array& Value::as_array() {
    if (!is_array())
        type_error("an array");
    return std::get<Array>(value_);
}

Value::Object& Value::as_object() {
    if (!is_object())
        type_error("an object");
    return std::get<Object>(value_);
}

const Value& Value::at(std::string_view key) const {
    const auto& object = as_object();
    const auto found = object.find(std::string(key));
    if (found == object.end())
        throw Error(ErrorCode::ParseError, "JSON object is missing key " + std::string(key));
    return found->second;
}

const Value& Value::get(std::string_view key, const Value& fallback) const {
    const auto& object = as_object();
    const auto found = object.find(std::string(key));
    return found == object.end() ? fallback : found->second;
}

Value parse(std::string_view input) {
    return Parser(input).parse_document();
}

std::string dump(const Value& value) {
    std::ostringstream output;
    dump_value(output, value);
    return output.str();
}

} // namespace pct::json
