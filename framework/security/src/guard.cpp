#include "cppboot/security/guard.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace cppboot::security {

std::optional<Principal> authenticate(const TokenService& tokens, const web::Request& request) {
    const auto header = request.header("Authorization");
    if (!header) {
        return std::nullopt;
    }

    constexpr std::string_view kScheme = "bearer ";
    const std::string_view value = *header;
    if (value.size() <= kScheme.size() ||
        !std::equal(kScheme.begin(), kScheme.end(), value.begin(), [](char expected, char actual) {
            return expected == std::tolower(static_cast<unsigned char>(actual));
        })) {
        return std::nullopt;
    }
    return tokens.verify(value.substr(kScheme.size()));
}

web::Response unauthorizedResponse() {
    web::Response response = web::errorResponse(401, "Sign in first");
    response.setHeader("WWW-Authenticate", "Bearer");
    return response;
}

web::Response forbiddenResponse() {
    return web::errorResponse(403, web::statusReason(403));
}

}  // namespace cppboot::security
