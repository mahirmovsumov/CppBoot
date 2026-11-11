#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <chrono>
#include <cppboot/config/config.hpp>
#include <cppboot/security/guard.hpp>
#include <cppboot/security/password.hpp>
#include <cppboot/security/token.hpp>
#include <filesystem>
#include <cppboot/base/json.hpp>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../framework/security/src/base64url.hpp"

using namespace cppboot::security;
using cppboot::Exception;
using detail::base64UrlDecode;
using detail::base64UrlEncode;

namespace {

constexpr const char* kSecret = "a-test-secret-that-is-long-enough-for-hs256";

using TimePoint = std::chrono::system_clock::time_point;

TimePoint at(std::int64_t seconds) {
    return TimePoint{std::chrono::seconds{seconds}};
}

TokenOptions makeOptions() {
    TokenOptions options;
    options.secret = kSecret;
    options.issuer = "test-issuer";
    options.lifetime = std::chrono::seconds{100};
    return options;
}

struct Parts {
    std::string header;
    std::string payload;
    std::string signature;
};

Parts split(const std::string& token) {
    const auto first = token.find('.');
    const auto last = token.rfind('.');
    return {token.substr(0, first), token.substr(first + 1, last - first - 1),
            token.substr(last + 1)};
}

/// Builds a correctly signed token with any contents, as someone holding the secret could. It tests
/// that a good signature alone isn't enough, so that bad claims are still rejected.
std::string signedToken(const std::string& header, const std::string& payload,
                        const std::string& secret = kSecret) {
    const std::string message = base64UrlEncode(header) + "." + base64UrlEncode(payload);

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(),
         &size);

    return message + "." +
           base64UrlEncode(std::string_view{reinterpret_cast<const char*>(digest.data()), size});
}

constexpr const char* kGoodHeader = R"({"alg":"HS256","typ":"JWT"})";

// Signed with .NET's HMACSHA256 rather than OpenSSL, which shows that other implementations agree.
// Payload: {"iss":"test-issuer","sub":"ada","exp":2000,"roles":["admin"]}
constexpr const char* kTokenFromAnotherImplementation = 
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJ0ZXN0LWlzc3VlciIsInN1YiI6ImFkYSIsImV4cCI6MjAwMCwicm9sZXMiOlsiYWRtaW4iXX0.JQtY0NYhWQGa2nSYzJ0SD92UHcGEAxZTsmgOwDRdKFQ";

}  // namespace

TEST(TokenStandardTest, AcceptsATokenSignedByAnotherImplementation) {
    const TokenService tokens{makeOptions(), [] { return at(1000); }};

    const auto principal = tokens.verify(kTokenFromAnotherImplementation);
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->subject, "ada");
    EXPECT_EQ(principal->roles, std::vector<std::string>{"admin"});
}

TEST(TokenStandardTest, MatchesTheFormatOfOtherLibraries) {
    TokenService tokens{makeOptions(), [] { return at(1000); }};
    const Parts parts = split(tokens.issue("ada", {"admin"}));

    const auto header = cppboot::Json::parse(*base64UrlDecode(parts.header));
    EXPECT_EQ(header["alg"], "HS256");
    EXPECT_EQ(header["typ"], "JWT");

    const auto payload = cppboot::Json::parse(*base64UrlDecode(parts.payload));
    EXPECT_EQ(payload["sub"], "ada");
    EXPECT_EQ(payload["iss"], "test-issuer");
    EXPECT_EQ(payload["iat"], 1000);
    EXPECT_EQ(payload["exp"], 1100);
    EXPECT_EQ(payload["roles"], cppboot::Json{std::vector<std::string>{"admin"}});

    // HMAC-SHA256 gives 32 bytes.
    EXPECT_EQ(base64UrlDecode(parts.signature)->size(), 32U);
}

class TokenTest : public ::testing::Test {
protected:
    TokenService tokens_{makeOptions(), [this] { return now_; }};
    TimePoint now_ = at(1000);
};

TEST_F(TokenTest, RoundTrips) {
    const auto principal = tokens_.verify(tokens_.issue("ada", {"admin", "owner"}));
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->subject, "ada");
    EXPECT_EQ(principal->roles, (std::vector<std::string>{"admin", "owner"}));
    EXPECT_TRUE(principal->hasRole("owner"));
    EXPECT_FALSE(principal->hasRole("other"));
    EXPECT_TRUE(principal->hasAnyRole({"other", "admin"}));
    EXPECT_FALSE(principal->hasAnyRole({"other", "another"}));
}

TEST_F(TokenTest, ExpiresAfterItsLifetimeAndTheLeeway) {
    const std::string token = tokens_.issue("ada");

    now_ = at(1000 + 100 + 29);
    EXPECT_TRUE(tokens_.verify(token));

    now_ = at(1000 + 100 + 30);
    EXPECT_FALSE(tokens_.verify(token));
}

TEST_F(TokenTest, RejectsAlteredPayload) {
    Parts parts = split(tokens_.issue("user", {}));
    auto payload = cppboot::Json::parse(*base64UrlDecode(parts.payload));
    payload["sub"] = "admin";
    payload["roles"] = cppboot::Json{std::vector<std::string>{"admin"}};

    const std::string forged =
        parts.header + "." + base64UrlEncode(payload.dump()) + "." + parts.signature;
    EXPECT_FALSE(tokens_.verify(forged));
}

TEST_F(TokenTest, RejectsEveryChangedCharacter) {
    const std::string token = tokens_.issue("ada", {"admin"});
    ASSERT_TRUE(tokens_.verify(token));

    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '.') {
            continue;
        }
        std::string changed = token;
        changed[i] = changed[i] == 'A' ? 'B' : 'A';
        EXPECT_FALSE(tokens_.verify(changed)) << "position " << i;
    }
}

TEST_F(TokenTest, RejectsTheNoneAlgorithm) {
    const Parts parts = split(tokens_.issue("ada", {"admin"}));
    const std::string none = base64UrlEncode(R"({"alg":"none","typ":"JWT"})");

    EXPECT_FALSE(tokens_.verify(none + "." + parts.payload + "."));
    EXPECT_FALSE(tokens_.verify(none + "." + parts.payload + "." + parts.signature));
}

TEST_F(TokenTest, HelperMakesTokensThatPass) {
    // Without this, the tests below could pass only because the helper's tokens are always invalid.
    EXPECT_TRUE(tokens_.verify(signedToken(
        kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000})")));
}

TEST_F(TokenTest, GoodSignatureDoesNotMakeBadClaimsValid) {
    const std::vector<std::pair<std::string, std::string>> bad = {
        {R"({"alg":"none"})", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {R"({"alg":"HS512"})", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {R"({"alg":"hs256"})", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {R"({"typ":"JWT"})", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {R"({"alg":5})", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {"not json", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {"[]", R"({"iss":"test-issuer","sub":"ada","exp":2000})"},
        {kGoodHeader, "not json"},
        {kGoodHeader, "[]"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada"})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":"2000"})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":500})"},
        {kGoodHeader, R"({"iss":"test-issuer","exp":2000})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"","exp":2000})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":5,"exp":2000})"},
        {kGoodHeader, R"({"sub":"ada","exp":2000})"},
        {kGoodHeader, R"({"iss":"other","sub":"ada","exp":2000})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000,"nbf":1500})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000,"nbf":"soon"})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000,"roles":"admin"})"},
        {kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000,"roles":[5]})"},
    };

    for (const auto& [header, payload] : bad) {
        EXPECT_FALSE(tokens_.verify(signedToken(header, payload))) << header << " " << payload;
    }
}

TEST_F(TokenTest, AcceptsAStartTimeThatHasPassed) {
    EXPECT_TRUE(tokens_.verify(signedToken(
        kGoodHeader, R"({"iss":"test-issuer","sub":"ada","exp":2000,"nbf":900})")));
}

TEST_F(TokenTest, RejectsATokenFromAnotherSecret) {
    TokenOptions other = makeOptions();
    other.secret = "another-secret-that-is-also-long-enough-yes";
    const TokenService other_tokens{other, [this] { return now_; }};

    EXPECT_FALSE(tokens_.verify(other_tokens.issue("ada")));
}

TEST_F(TokenTest, RejectsATokenFromAnotherIssuer) {
    TokenOptions other = makeOptions();
    other.issuer = "another-issuer";
    const TokenService other_tokens{other, [this] { return now_; }};

    EXPECT_FALSE(tokens_.verify(other_tokens.issue("ada")));
}

TEST_F(TokenTest, RejectsMalformedTokens) {
    const std::string good = tokens_.issue("ada");
    for (const std::string& bad :
         {std::string{}, std::string{"."}, std::string{".."}, std::string{"..."},
          std::string{"abc"}, std::string{"a.b"}, std::string{"a.b.c"}, std::string{"a.b.c.d"},
          good + ".extra", "." + good, std::string{"!!!.@@@.###"},
          std::string(10000, 'a') + "." + std::string(10000, 'b') + ".c"}) {
        EXPECT_FALSE(tokens_.verify(bad)) << bad.substr(0, 40);
    }
}

TEST_F(TokenTest, RejectsPaddingAndOtherSpellingsOfTheSameSignature) {
    const std::string token = tokens_.issue("ada");
    EXPECT_FALSE(tokens_.verify(token + "="));
    EXPECT_FALSE(tokens_.verify(token + "=="));
}

TEST(TokenOptionsTest, RejectsAShortSecret) {
    TokenOptions options = makeOptions();
    options.secret = std::string(31, 'x');
    EXPECT_THROW((TokenService{options}), Exception);

    options.secret = std::string(32, 'x');
    EXPECT_NO_THROW((TokenService{options}));
}

TEST(TokenOptionsTest, ErrorsDoNotShowTheSecret) {
    TokenOptions options = makeOptions();
    options.secret = "hunter2";
    try {
        TokenService tokens{options};
        FAIL() << "Expected an exception";
    } catch (const Exception& exception) {
        EXPECT_EQ(std::string{exception.what()}.find("hunter2"), std::string::npos);
    }
}

namespace {

cppboot::config::Config configWith(std::map<std::string, std::string> settings) {
    cppboot::config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = std::move(settings);
    return cppboot::config::Config::load(sources).value();
}

}  // namespace

TEST(TokenSettingsTest, ReadsSettings) {
    const auto tokens = TokenService::fromSettings(
        configWith({{"cppboot.security.jwt.secret", kSecret},
                    {"cppboot.security.jwt.issuer", "shop"},
                    {"cppboot.security.jwt.lifetime_seconds", "60"}}));

    const Parts parts = split(tokens->issue("ada"));
    const auto payload = cppboot::Json::parse(*base64UrlDecode(parts.payload));
    EXPECT_EQ(payload["iss"], "shop");
    EXPECT_EQ(payload["exp"].get<std::int64_t>() - payload["iat"].get<std::int64_t>(), 60);
}

TEST(TokenSettingsTest, MissingOrBadSettingsThrow) {
    EXPECT_THROW(static_cast<void>(TokenService::fromSettings(configWith({}))), Exception);
    EXPECT_THROW(static_cast<void>(TokenService::fromSettings(
                     configWith({{"cppboot.security.jwt.secret", "short"}}))),
                 Exception);
    EXPECT_THROW(static_cast<void>(TokenService::fromSettings(
                     configWith({{"cppboot.security.jwt.secret", kSecret},
                                 {"cppboot.security.jwt.lifetime_seconds", "0"}}))),
                 Exception);
}

TEST(TokenSettingsTest, ConfigLogHidesTheSecret) {
    const auto config = configWith({{"cppboot.security.jwt.secret", kSecret}});
    EXPECT_EQ(config.describe().find(kSecret), std::string::npos);
}

TEST(Base64UrlTest, RoundTripsEveryLength) {
    std::string input;
    for (int size = 0; size < 64; ++size) {
        EXPECT_EQ(base64UrlDecode(base64UrlEncode(input)), input);
        input += static_cast<char>(size * 37);
    }
}

TEST(Base64UrlTest, MatchesKnownValues) {
    EXPECT_EQ(base64UrlEncode(""), "");
    EXPECT_EQ(base64UrlEncode("f"), "Zg");
    EXPECT_EQ(base64UrlEncode("fo"), "Zm8");
    EXPECT_EQ(base64UrlEncode("foo"), "Zm9v");
    EXPECT_EQ(base64UrlEncode("\xFB\xFF"), "-_8");
}

TEST(Base64UrlTest, RejectsAnythingButTheExactEncoding) {
    EXPECT_FALSE(base64UrlDecode("Zg=="));
    EXPECT_FALSE(base64UrlDecode("Z"));
    EXPECT_FALSE(base64UrlDecode("Zh"));  // Same first byte as "Zg", with unused bits set.
    EXPECT_FALSE(base64UrlDecode("+/"));
    EXPECT_FALSE(base64UrlDecode("Zg\n"));
}

TEST(PasswordTest, VerifiesTheRightPassword) {
    const std::string hash = hashPassword("correct horse");
    EXPECT_TRUE(verifyPassword("correct horse", hash));
}

TEST(PasswordTest, RejectsTheWrongPassword) {
    const std::string hash = hashPassword("correct horse");
    EXPECT_FALSE(verifyPassword("Correct horse", hash));
    EXPECT_FALSE(verifyPassword("", hash));
    EXPECT_FALSE(verifyPassword("correct horse ", hash));
}

TEST(PasswordTest, HashIsSaltedAndHidesThePassword) {
    const std::string first = hashPassword("correct horse");
    const std::string second = hashPassword("correct horse");
    EXPECT_NE(first, second);
    EXPECT_EQ(first.find("correct"), std::string::npos);
    EXPECT_TRUE(first.starts_with("scrypt$15$8$1$"));
}

TEST(PasswordTest, HandlesUnusualPasswords) {
    for (const std::string& password :
         {std::string{}, std::string(10000, 'x'), std::string{"\xC3\xA9 unicode"},
          std::string("with\0null", 9)}) {
        EXPECT_TRUE(verifyPassword(password, hashPassword(password)));
    }
}

TEST(PasswordTest, InvalidStoredHashesFailSafely) {
    const std::string good = hashPassword("x");
    for (const char* bad :
         {"", "x", "scrypt", "scrypt$", "md5$15$8$1$aaaa$aaaa", "scrypt$15$8$1$aaaa",
          "scrypt$abc$8$1$aaaa$aaaa", "scrypt$15$8$1$!!!!$aaaa", "scrypt$15$8$1$aaaa$",
          "scrypt$0$8$1$aaaa$aaaa", "scrypt$15$0$1$aaaa$aaaa", "scrypt$15$8$0$aaaa$aaaa",
          // Asks for terabytes of memory, which must be refused rather than attempted.
          "scrypt$40$8$1$aaaa$aaaa", "scrypt$15$100000$1$aaaa$aaaa", "scrypt$15$8$100000$aaaa$aaaa"}) {
        EXPECT_FALSE(verifyPassword("x", bad)) << bad;
    }
    EXPECT_TRUE(verifyPassword("x", good));
}

class GuardTest : public ::testing::Test {
protected:
    std::optional<Principal> authenticateWith(const std::string& header_value) const {
        const auto request = cppboot::web::Request::create(
                                 cppboot::web::Method::Get, "/",
                                 cppboot::web::Headers{{"Authorization", header_value}})
                                 .value();
        return authenticate(tokens_, request);
    }

    TokenService tokens_{makeOptions()};
};

TEST_F(GuardTest, AcceptsABearerToken) {
    const auto principal = authenticateWith("Bearer " + tokens_.issue("ada"));
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->subject, "ada");
}

TEST_F(GuardTest, SchemeIgnoresLetterCase) {
    EXPECT_TRUE(authenticateWith("bearer " + tokens_.issue("ada")));
    EXPECT_TRUE(authenticateWith("BEARER " + tokens_.issue("ada")));
}

TEST_F(GuardTest, RejectsOtherHeaders) {
    const std::string token = tokens_.issue("ada");
    EXPECT_FALSE(authenticateWith(token));
    EXPECT_FALSE(authenticateWith("Basic " + token));
    EXPECT_FALSE(authenticateWith("Bearer"));
    EXPECT_FALSE(authenticateWith("Bearer "));
    EXPECT_FALSE(authenticateWith(""));
}

TEST_F(GuardTest, MissingHeaderIsRejected) {
    const auto request = cppboot::web::Request::create(cppboot::web::Method::Get, "/").value();
    EXPECT_FALSE(authenticate(tokens_, request));
}

TEST(GuardResponseTest, ResponsesAreCorrect) {
    const auto unauthorized = unauthorizedResponse();
    EXPECT_EQ(unauthorized.status(), 401);
    EXPECT_EQ(unauthorized.header("WWW-Authenticate"), "Bearer");
    EXPECT_EQ(forbiddenResponse().status(), 403);
}
