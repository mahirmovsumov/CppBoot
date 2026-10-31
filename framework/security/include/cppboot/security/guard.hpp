#pragma once

#include <optional>

#include "cppboot/security/token.hpp"
#include "cppboot/web/http.hpp"

namespace cppboot::security {

/// Returns who a request's "Authorization: Bearer" token identifies, or nothing if it has no valid
/// token. Generated route code calls this for every route not marked CPPBOOT_PUBLIC.
std::optional<Principal> authenticate(const TokenService& tokens, const web::Request& request);

/// A 401 response, which never says why a token was rejected, because that would help an attacker.
web::Response unauthorizedResponse();

/// A 403 response, for a valid user without a required role.
web::Response forbiddenResponse();

}  // namespace cppboot::security
