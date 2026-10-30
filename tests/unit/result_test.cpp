#include <gtest/gtest.h>

#include <cppboot/base/result.hpp>
#include <memory>
#include <string>

using cppboot::Error;
using cppboot::ErrorCode;
using cppboot::Exception;
using cppboot::Result;

TEST(ResultTest, HoldsValue) {
    Result<int> result{42};
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 42);
    EXPECT_THROW(static_cast<void>(result.error()), Exception);
}

TEST(ResultTest, HoldsError) {
    Result<int> result{Error{ErrorCode::NotFound, "missing"}};
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(result.error().message(), "missing");
    EXPECT_THROW(static_cast<void>(result.value()), Exception);
}

TEST(ResultTest, MovesOutMoveOnlyValues) {
    Result<std::unique_ptr<int>> result{std::make_unique<int>(7)};
    const std::unique_ptr<int> value = std::move(result).value();
    EXPECT_EQ(*value, 7);
}

TEST(ResultTest, VoidResultReportsSuccessAndFailure) {
    const Result<void> success;
    EXPECT_TRUE(success);
    EXPECT_NO_THROW(success.value());

    const Result<void> failure{Error{ErrorCode::IoError, "disk"}};
    EXPECT_FALSE(failure);
    EXPECT_EQ(failure.error().code(), ErrorCode::IoError);
    EXPECT_THROW(failure.value(), Exception);
}

TEST(ErrorTest, DescribeIncludesCodeMessageAndFile) {
    const std::string text = Error{ErrorCode::ParseError, "bad input"}.describe();
    EXPECT_NE(text.find("ParseError"), std::string::npos);
    EXPECT_NE(text.find("bad input"), std::string::npos);
    EXPECT_NE(text.find("result_test.cpp"), std::string::npos);
}
