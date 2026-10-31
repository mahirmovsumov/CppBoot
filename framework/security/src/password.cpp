#include "cppboot/security/password.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "base64url.hpp"
#include "cppboot/base/exceptions.hpp"

namespace cppboot::security {

namespace {

// N = 2^15 uses about 32 MB and takes around 100 ms, as OWASP suggests for scrypt.
constexpr int kLogN = 15;
constexpr int kBlockSize = 8;
constexpr int kParallelism = 1;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
constexpr std::string_view kPrefix = "scrypt";

// A stored hash is data, so limits stop a bad one from using all of the server's memory.
constexpr int kMaximumLogN = 20;
constexpr int kMaximumBlockSize = 32;
constexpr int kMaximumParallelism = 16;

std::optional<std::string> deriveKey(std::string_view password, std::string_view salt, int log_n,
                                     int block_size, int parallelism, std::size_t size) {
    const std::uint64_t n = std::uint64_t{1} << log_n;
    const auto r = static_cast<std::uint64_t>(block_size);
    const auto p = static_cast<std::uint64_t>(parallelism);
    const std::uint64_t maximum_memory = 2 * (128 * n * r + 128 * r * p);

    std::string key(size, '\0');
    const int status = EVP_PBE_scrypt(
        password.data(), password.size(), reinterpret_cast<const unsigned char*>(salt.data()),
        salt.size(), n, r, p, maximum_memory, reinterpret_cast<unsigned char*>(key.data()), size);
    if (status != 1) {
        return std::nullopt;
    }
    return key;
}

std::optional<int> parseInt(std::string_view text) {
    int value = 0;
    const char* const end = text.data() + text.size();
    const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string hashPassword(std::string_view password) {
    std::array<unsigned char, kSaltBytes> salt_bytes{};
    if (RAND_bytes(salt_bytes.data(), static_cast<int>(salt_bytes.size())) != 1) {
        throw Exception(Error{ErrorCode::PlatformError, "Generating a salt failed"});
    }
    const std::string salt{reinterpret_cast<const char*>(salt_bytes.data()), salt_bytes.size()};

    const auto key = deriveKey(password, salt, kLogN, kBlockSize, kParallelism, kHashBytes);
    if (!key) {
        throw Exception(Error{ErrorCode::PlatformError, "Hashing a password failed"});
    }

    return std::string{kPrefix} + "$" + std::to_string(kLogN) + "$" + std::to_string(kBlockSize) +
           "$" + std::to_string(kParallelism) + "$" + detail::base64UrlEncode(salt) + "$" +
           detail::base64UrlEncode(*key);
}

bool verifyPassword(std::string_view password, std::string_view stored_hash) {
    std::vector<std::string_view> parts;
    std::string_view rest = stored_hash;
    while (true) {
        const auto separator = rest.find('$');
        parts.push_back(rest.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(separator + 1);
    }
    if (parts.size() != 6 || parts[0] != kPrefix) {
        return false;
    }

    const auto log_n = parseInt(parts[1]);
    const auto block_size = parseInt(parts[2]);
    const auto parallelism = parseInt(parts[3]);
    const auto salt = detail::base64UrlDecode(parts[4]);
    const auto expected = detail::base64UrlDecode(parts[5]);
    if (!log_n || !block_size || !parallelism || !salt || !expected || expected->empty() ||
        *log_n < 1 || *log_n > kMaximumLogN || *block_size < 1 ||
        *block_size > kMaximumBlockSize || *parallelism < 1 || *parallelism > kMaximumParallelism) {
        return false;
    }

    const auto actual =
        deriveKey(password, *salt, *log_n, *block_size, *parallelism, expected->size());
    return actual && CRYPTO_memcmp(actual->data(), expected->data(), expected->size()) == 0;
}

}  // namespace cppboot::security
