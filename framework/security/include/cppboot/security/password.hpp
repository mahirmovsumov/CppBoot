#pragma once

#include <string>
#include <string_view>

namespace cppboot::security {

/// Returns a salted scrypt hash to store in place of a password. It's slow and uses about 32 MB on
/// purpose, because that's what makes guessing passwords expensive. Failure throws
/// cppboot::Exception.
std::string hashPassword(std::string_view password);

/// Returns whether a password matches a stored hash. An invalid hash returns false.
bool verifyPassword(std::string_view password, std::string_view stored_hash);

}  // namespace cppboot::security
