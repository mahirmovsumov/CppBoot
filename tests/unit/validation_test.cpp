#include <gtest/gtest.h>

#include <cppboot/base/validation.hpp>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace cppboot::validation;

TEST(ValidationFunctionsTest, RangesIncludeTheirLimits) {
    EXPECT_TRUE(inRange(1, 1, 100));
    EXPECT_TRUE(inRange(100, 1, 100));
    EXPECT_FALSE(inRange(0, 1, 100));
    EXPECT_FALSE(inRange(101, 1, 100));
    EXPECT_TRUE(atLeast(0.0, 0));
    EXPECT_FALSE(atLeast(-0.001, 0));
    EXPECT_TRUE(atMost(0.5, 0.5));
    EXPECT_FALSE(atMost(0.51, 0.5));
}

TEST(ValidationFunctionsTest, ComparesNumbersOfDifferentTypesByValue) {
    // Converting -1 to unsigned would make it the largest value, and wrongly pass.
    EXPECT_FALSE(atLeast(-1, 0U));
    EXPECT_TRUE(atLeast(5U, -1));
    EXPECT_FALSE(atMost(std::numeric_limits<std::uint64_t>::max(), 10));
    EXPECT_TRUE(atMost(std::int8_t{5}, 1e3));
    EXPECT_TRUE(inRange(std::uint8_t{200}, 0, 255));
    EXPECT_FALSE(inRange(2.5F, 3, 4));
}

TEST(ValidationFunctionsTest, AMissingOptionalPasses) {
    const std::optional<int> missing;
    const std::optional<int> low{0};
    EXPECT_TRUE(inRange(missing, 1, 5));
    EXPECT_FALSE(inRange(low, 1, 5));
    EXPECT_TRUE(atLeast(missing, 1));
    EXPECT_TRUE(atMost(missing, 1));

    const std::optional<std::string> no_text;
    const std::optional<std::string> empty_text{""};
    EXPECT_TRUE(notEmpty(no_text));
    EXPECT_FALSE(notEmpty(empty_text));
    EXPECT_TRUE(lengthInRange(no_text, 1, 2));
    EXPECT_FALSE(lengthInRange(empty_text, 1, 2));
}

TEST(ValidationFunctionsTest, NotEmptyWorksOnTextAndContainers) {
    EXPECT_TRUE(notEmpty(std::string{"x"}));
    EXPECT_FALSE(notEmpty(std::string{}));
    EXPECT_TRUE(notEmpty(std::vector<int>{1}));
    EXPECT_FALSE(notEmpty(std::vector<int>{}));
    EXPECT_FALSE((notEmpty(std::map<std::string, int>{})));
}

TEST(ValidationFunctionsTest, LengthCountsCharactersInTextAndItemsInContainers) {
    EXPECT_TRUE(lengthInRange(std::string{"abc"}, 3, 3));
    EXPECT_FALSE(lengthInRange(std::string{"abcd"}, 3, 3));
    EXPECT_FALSE(lengthInRange(std::string{}, 1, 3));
    // Three characters, in six bytes and in nine.
    EXPECT_TRUE(lengthInRange(std::string{"\xC3\xA7\xC3\xB6\xC5\x9F"}, 3, 3));
    EXPECT_TRUE(lengthInRange(std::string{"\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC"}, 3, 3));
    EXPECT_TRUE(lengthInRange(std::vector<int>{1, 2}, 1, 2));
    EXPECT_FALSE(lengthInRange(std::vector<int>{1, 2, 3}, 1, 2));
}

// These are usable where a constant is needed, such as in a static_assert on a default value.
static_assert(inRange(5, 1, 10));
static_assert(!atLeast(-1, 0U));
static_assert(lengthInRange(std::string_view{"abc"}, 1, 3));
