#include "cppboot/security/token.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <utility>

#include "base64url.hpp"
#include "cppboot/base/exceptions.hpp"

namespace cppboot::security {

namespace {

constexpr std::size_t kMinimumSecretBytes = 32;
constexpr std::string_view kHeader = R"({"alg":"HS256","typ":"JWT"})";

// Allows for clocks that differ slightly between servers.
constexpr double kClockLeewaySeconds = 30;

std::string sign(std::string_view secret, std::string_view message) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    const unsigned char* const result =
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(),
             &size);
    if (result == nullptr) {
        throw Exception(Error{ErrorCode::PlatformError, "Signing a token failed"});
    }
    return std::string{reinterpret_cast<const char*>(digest.data()), size};
}

bool equalInConstantTime(std::string_view left, std::string_view right) {
    // Comparing byte by byte until the first difference would let an attacker measure how many
    // bytes of a forged signature were right.
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::int64_t toSeconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

}  // namespace

bool Principal::hasRole(std::string_view role) const {
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

bool Principal::hasAnyRole(std::initializer_list<std::string_view> wanted) const {
    return std::any_of(wanted.begin(), wanted.end(),
                       [this](std::string_view role) { return hasRole(role); });
}

TokenService::TokenService(TokenOptions options, Clock clock)
    : options_(std::move(options)), clock_(std::move(clock)) {
    if (options_.secret.size() < kMinimumSecretBytes) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "The JWT secret must be at least " +
                                  std::to_string(kMinimumSecretBytes) + " bytes"});
    }
}

std::shared_ptr<TokenService> TokenService::fromSettings(const config::Config& config) {
    TokenOptions options;
    options.secret = config.getString("cppboot.security.jwt.secret");
    if (options.secret.empty()) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Set cppboot.security.jwt.secret to at least 32 random bytes, with "
                              "the environment variable CPPBOOT_SECURITY_JWT_SECRET"});
    }

    options.issuer = config.getString("cppboot.security.jwt.issuer", options.issuer);

    const auto lifetime = config.getInt("cppboot.security.jwt.lifetime_seconds", 3600);
    if (!lifetime) {
        throw Exception(lifetime.error());
    }
    if (lifetime.value() < 1) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Setting cppboot.security.jwt.lifetime_seconds must be positive"});
    }
    options.lifetime = std::chrono::seconds{lifetime.value()};

    return std::make_shared<TokenService>(std::move(options));
}

std::string TokenService::issue(std::string_view subject,
                                const std::vector<std::string>& roles) const {
    const std::int64_t now = toSeconds(clock_());

    const nlohmann::json payload = {
        {"iss", options_.issuer},
        {"sub", subject},
        {"iat", now},
        {"exp", now + options_.lifetime.count()},
        {"roles", roles},
    };

    const std::string message =
        detail::base64UrlEncode(kHeader) + "." + detail::base64UrlEncode(payload.dump());
    return message + "." + detail::base64UrlEncode(sign(options_.secret, message));
}

std::optional<Principal> TokenService::verify(std::string_view token) const {
    const auto first_dot = token.find('.');
    const auto last_dot = token.rfind('.');
    if (first_dot == std::string_view::npos || first_dot == last_dot ||
        token.find('.', first_dot + 1) != last_dot) {
        return std::nullopt;
    }

    const std::string_view message = token.substr(0, last_dot);
    const auto signature = detail::base64UrlDecode(token.substr(last_dot + 1));
    if (!signature || !equalInConstantTime(*signature, sign(options_.secret, message))) {
        return std::nullopt;
    }

    const auto header_text = detail::base64UrlDecode(token.substr(0, first_dot));
    const auto payload_text =
        detail::base64UrlDecode(token.substr(first_dot + 1, last_dot - first_dot - 1));
    if (!header_text || !payload_text) {
        return std::nullopt;
    }

    // Nothing is parsed before the signature passes, so an attacker's JSON never gets read.
    const auto header = nlohmann::json::parse(*header_text, nullptr, false);
    const auto payload = nlohmann::json::parse(*payload_text, nullptr, false);
    if (!header.is_object() || !payload.is_object()) {
        return std::nullopt;
    }

    // Only HS256 is accepted, whatever the token asks for. This rules out "none", and attacks that
    // switch algorithms.
    const auto algorithm = header.find("alg");
    if (algorithm == header.end() || !algorithm->is_string() || *algorithm != "HS256") {
        return std::nullopt;
    }

    const auto issuer = payload.find("iss");
    if (issuer == payload.end() || !issuer->is_string() || *issuer != options_.issuer) {
        return std::nullopt;
    }

    // JWT times can have fractions, so they're compared as doubles.
    const auto now = static_cast<double>(toSeconds(clock_()));

    // A token with no expiry would be valid forever, so it isn't accepted.
    const auto expiry = payload.find("exp");
    if (expiry == payload.end() || !expiry->is_number() ||
        now >= expiry->get<double>() + kClockLeewaySeconds) {
        return std::nullopt;
    }

    const auto not_before = payload.find("nbf");
    if (not_before != payload.end() &&
        (!not_before->is_number() || now + kClockLeewaySeconds < not_before->get<double>())) {
        return std::nullopt;
    }

    const auto subject = payload.find("sub");
    if (subject == payload.end() || !subject->is_string() ||
        subject->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }

    Principal principal;
    principal.subject = subject->get<std::string>();

    if (const auto roles = payload.find("roles"); roles != payload.end()) {
        if (!roles->is_array()) {
            return std::nullopt;
        }
        for (const auto& role : *roles) {
            if (!role.is_string()) {
                return std::nullopt;
            }
            principal.roles.push_back(role.get<std::string>());
        }
    }
    return principal;
}

}  // namespace cppboot::security
