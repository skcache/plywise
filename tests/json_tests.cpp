#include "test.hpp"

#include "pct/common/json.hpp"

using namespace pct::json;

TEST_CASE("JSON parser reads structured request data") {
    const Value value =
        parse(R"json({"url":"https://chess.com","enabled":true,"items":[1,2,3]})json");
    CHECK_EQ(value.at("url").as_string(), "https://chess.com");
    CHECK(value.at("enabled").as_bool());
    CHECK_EQ(value.at("items").as_array().size(), 3ULL);
}

TEST_CASE("JSON dump round trips strings and arrays") {
    const Value original(Value::Object{
        {"message", "line one\n\"line two\""},
        {"values", Value::Array{1, 2, 3}},
    });
    const std::string encoded = dump(original);
    const Value decoded = parse(encoded);
    CHECK_EQ(decoded.at("message").as_string(), "line one\n\"line two\"");
    CHECK_EQ(decoded.at("values").as_array().size(), 3ULL);
}

TEST_CASE("JSON parser reads decimal and exponent numbers") {
    const Value parsed = parse("[0.5,-12.25,6.02e2]");
    const auto& values = parsed.as_array();
    CHECK_EQ(values[0].as_number(), 0.5);
    CHECK_EQ(values[1].as_number(), -12.25);
    CHECK_EQ(values[2].as_number(), 602.0);
    CHECK_THROWS(parse("1e9999"));
}

TEST_CASE("JSON parser rejects malformed and duplicate data") {
    CHECK_THROWS(parse("{\"a\":1,}"));
    CHECK_THROWS(parse("{\"a\":1,\"a\":2}"));
    CHECK_THROWS(parse("[1 2]"));
}

TEST_CASE("JSON parser rejects excessive nesting before recursion can exhaust the stack") {
    std::string nested;
    for (int depth = 0; depth < 80; ++depth)
        nested += "[";
    nested += "0";
    for (int depth = 0; depth < 80; ++depth)
        nested += "]";
    CHECK_THROWS(parse(nested));
}
