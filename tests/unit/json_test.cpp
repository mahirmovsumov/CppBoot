#include <gtest/gtest.h>

#include <cppboot/base/json.hpp>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

using cppboot::Json;
using cppboot::JsonError;

namespace {

std::string rejected(std::string_view text) {
    const auto result = Json::tryParse(text);
    EXPECT_FALSE(result) << "accepted: " << text;
    return result ? std::string{} : result.error().message();
}

}  // namespace

TEST(JsonTest, ReadsEveryKindOfValue) {
    const Json json = Json::parse(R"( {"a": null, "b": true, "c": -12, "d": 1.5e2, "e": "x",
                                       "f": [1, [2]], "g": {}} )");
    EXPECT_TRUE(json.at("a").isNull());
    EXPECT_TRUE(json.at("b").asBool());
    EXPECT_EQ(json.at("c").asInt64(), -12);
    EXPECT_EQ(json.at("d").asDouble(), 150.0);
    EXPECT_FALSE(json.at("d").isInteger());
    EXPECT_EQ(json.at("e").asString(), "x");
    EXPECT_EQ(json.at("f").at(1).at(0).asInt64(), 2);
    EXPECT_TRUE(json.at("g").isObject());
    EXPECT_EQ(json.size(), 7u);
}

TEST(JsonTest, WritesCompactlyInTheOrderAdded) {
    Json json = Json::object();
    json["zebra"] = 1;
    json["apple"] = "two";
    json["list"] = std::vector<int>{1, 2};
    json["fraction"] = 0.5;
    json["whole_double"] = 2.0;
    json["none"] = nullptr;
    EXPECT_EQ(json.dump(),
              R"({"zebra":1,"apple":"two","list":[1,2],"fraction":0.5,"whole_double":2.0,"none":null})");
}

TEST(JsonTest, RoundTripsLargeNumbersExactly) {
    const std::uint64_t biggest = std::numeric_limits<std::uint64_t>::max();
    const std::int64_t smallest = std::numeric_limits<std::int64_t>::min();
    const Json json = Json::parse(Json{std::vector<Json>{Json{biggest}, Json{smallest}}}.dump());
    EXPECT_EQ(json.at(0).asUint64(), biggest);
    EXPECT_EQ(json.at(1).asInt64(), smallest);
    EXPECT_EQ(json.dump(), "[18446744073709551615,-9223372036854775808]");
}

TEST(JsonTest, EscapesTextAndReplacesInvalidUtf8) {
    const Json json{std::string{"q\"\\\n\x01 \xC3\xA9 \xFF end"}};
    EXPECT_EQ(json.dump(), "\"q\\\"\\\\\\n\\u0001 \xC3\xA9 \xEF\xBF\xBD end\"");
}

TEST(JsonTest, ReadsEscapesIncludingSurrogatePairs) {
    EXPECT_EQ(Json::parse(R"("\u00e9\ud83d\ude00\/\t")").asString(), "\xC3\xA9\xF0\x9F\x98\x80/\t");
}

TEST(JsonTest, RejectsWhatTheStandardDoesNotAllow) {
    for (const char* text :
         {"", " ", "nul", "[1,]", "{\"a\":1,}", "{'a':1}", "01", "1.", ".5", "+1", "1e", "NaN",
          "Infinity", "[1 2]", "{\"a\" 1}", "\"\x01\"", "\"\\x41\"", "\"open", "[", "{",
          "1 2", "tru", "\"\\ud800\"", "\"\\udc00\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"",
          "\"\xF4\x90\x80\x80\"", "1e999"}) {
        EXPECT_FALSE(Json::tryParse(text)) << text;
    }
}

TEST(JsonTest, RejectsARepeatedKey) {
    EXPECT_NE(rejected(R"({"role":"user","role":"admin"})").find("appears twice"), std::string::npos);
}

TEST(JsonTest, LimitsNesting) {
    EXPECT_TRUE(Json::tryParse(std::string(100, '[') + std::string(100, ']')));
    EXPECT_NE(rejected(std::string(100000, '[')).find("nested too deeply"), std::string::npos);
}

TEST(JsonTest, ConvertsStandardTypes) {
    const std::map<std::string, std::vector<std::optional<int>>> value{{"a", {1, std::nullopt}}};
    const Json json{value};
    EXPECT_EQ(json.dump(), R"({"a":[1,null]})");
    EXPECT_EQ((json.get<std::map<std::string, std::vector<std::optional<int>>>>()), value);
}

TEST(JsonTest, NumberConversionsCheckRange) {
    EXPECT_EQ(Json::parse("127").get<std::int8_t>(), 127);
    EXPECT_THROW(static_cast<void>(Json::parse("128").get<std::int8_t>()), JsonError);
    EXPECT_THROW(static_cast<void>(Json::parse("-1").get<unsigned>()), JsonError);
    EXPECT_THROW(static_cast<void>(Json::parse("1.5").get<int>()), JsonError);
    EXPECT_EQ(Json::parse("3").get<double>(), 3.0);
}

TEST(JsonTest, WrongTypesSayWhatWasExpected) {
    try {
        static_cast<void>(Json::parse("[1]").get<std::vector<std::string>>());
        FAIL() << "accepted";
    } catch (const JsonError& error) {
        EXPECT_STREQ(error.what(), "item 0: expected a string, but found a number");
    }
}

TEST(JsonTest, ComparesValuesNotSpelling) {
    EXPECT_EQ(Json::parse("1"), Json::parse("1.0"));
    EXPECT_EQ(Json::parse(R"({"a":1,"b":2})"), Json::parse(R"({"b":2,"a":1})"));
    EXPECT_NE(Json::parse("[1,2]"), Json::parse("[2,1]"));
    EXPECT_EQ(Json::parse(R"("x")"), "x");
    EXPECT_NE(Json::parse("-1"), Json{std::numeric_limits<std::uint64_t>::max()});
}

TEST(JsonTest, NotANumberIsWrittenAsNull) {
    EXPECT_EQ(Json{std::numeric_limits<double>::quiet_NaN()}.dump(), "null");
}

TEST(JsonTest, LookupsOnTheWrongTypeThrowOrReturnNothing) {
    const Json number{5};
    EXPECT_EQ(number.find("a"), nullptr);
    EXPECT_FALSE(number.contains("a"));
    EXPECT_THROW(static_cast<void>(number.at("a")), JsonError);
    EXPECT_THROW(static_cast<void>(Json::array().at(0)), JsonError);
    Json text{"x"};
    EXPECT_THROW(text["a"] = 1, JsonError);
}

// A request body is what a client sends, so the parser has to fail cleanly on anything. This
// damages valid documents at random, with a fixed seed so a failure can be repeated. The fuzz job
// in CI goes much further.
TEST(JsonTest, SurvivesDamagedInput) {
    const std::string samples[] = {
        R"({"name":"ada","roles":["admin"],"age":36,"score":-1.5e3,"ok":true,"none":null})",
        R"([{"a":"\u00e9\ud83d\ude00"},[[[]]],{},"\"\\\/\b\f\n\r\t"])",
    };
    const std::string alphabet = "{}[]:,\"\\ueE+-.0123456789truefalsnl \t\n\x80\xC3\xFF";
    std::mt19937 random{4321};
    for (int round = 0; round < 20000; ++round) {
        std::string text = samples[static_cast<std::size_t>(round) % std::size(samples)];
        const int edits = 1 + static_cast<int>(random() % 8);
        for (int e = 0; e < edits && !text.empty(); ++e) {
            const std::size_t at = random() % text.size();
            switch (random() % 3) {
                case 0: text[at] = alphabet[random() % alphabet.size()]; break;
                case 1: text.insert(at, 1, alphabet[random() % alphabet.size()]); break;
                default: text.erase(at, 1); break;
            }
        }
        if (const auto parsed = Json::tryParse(text)) {
            // Anything that reads has to write out as JSON that reads back the same.
            EXPECT_EQ(Json::parse(parsed.value().dump()), parsed.value()) << text;
        }
    }
}
