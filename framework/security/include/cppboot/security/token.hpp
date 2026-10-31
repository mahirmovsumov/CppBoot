#pragma once

#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/config/config.hpp"

namespace cppboot::security {

/// Who a request comes from, as proven by a valid token.
struct Principal {
    std::string subject;
    std::vector<std::string> roles;

    bool hasRole(std::string_view role) const;
    bool hasAnyRole(std::initializer_list<std::string_view> wanted) const;
};

struct TokenOptions {
    /// At least 32 bytes, and it should be random. Anyone who has this can sign in as anyone.
    std::string secret;
    /// Tokens from a different issuer are rejected.
    std::string issuer{"cppboot"};
    std::chrono::seconds lifetime{std::chrono::hours{1}};
};

/// Issues and verifies JSON Web Tokens signed with HS256. It's safe to use from any thread.
class TokenService {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    /// A secret shorter than 32 bytes throws cppboot::Exception.
    explicit TokenService(TokenOptions options, Clock clock = std::chrono::system_clock::now);

    /// Creates a service from settings under cppboot.security.jwt. A missing or short secret throws
    /// cppboot::Exception, so that an application can't start without real protection.
    static std::shared_ptr<TokenService> fromSettings(const config::Config& config);

    /// Returns a signed token for a user who has already proven who they are.
    std::string issue(std::string_view subject, const std::vector<std::string>& roles = {}) const;

    /// Returns who a token identifies, or nothing if it isn't valid. It's invalid if it's malformed,
    /// has the wrong signature, algorithm or issuer, has expired, or isn't valid yet.
    std::optional<Principal> verify(std::string_view token) const;

private:
    TokenOptions options_;
    Clock clock_;
};

}  // namespace cppboot::security
