#include "tls.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <boost/asio/ssl.hpp>
#include <exception>
#include <string>

namespace cppboot::web {

namespace {

namespace ssl = boost::asio::ssl;

// For TLS 1.2, which also has weak choices. All of these have forward secrecy and authenticated
// encryption. TLS 1.3 only has good choices, so its OpenSSL defaults stay in place.
constexpr const char* kTls12Ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

Error tlsError(const std::string& message) {
    return Error{ErrorCode::InvalidArgument, message};
}

}  // namespace

Result<std::unique_ptr<ssl::context>> makeTlsContext(
    const TlsOptions& options, const std::function<void(std::string_view)>& log_error) {
    int min_version = 0;
    if (options.min_version == "1.2") {
        min_version = TLS1_2_VERSION;
    } else if (options.min_version == "1.3") {
        min_version = TLS1_3_VERSION;
    } else {
        return tlsError("Setting cppboot.server.tls.min_version must be 1.2 or 1.3");
    }

    std::unique_ptr<ssl::context> context;
    try {
        context = std::make_unique<ssl::context>(ssl::context::tls_server);
    } catch (const std::exception& exception) {
        return tlsError(std::string{"Cannot set up TLS: "} + exception.what());
    }
    SSL_CTX* const native = context->native_handle();

    if (SSL_CTX_set_min_proto_version(native, min_version) != 1 ||
        SSL_CTX_set_cipher_list(native, kTls12Ciphers) != 1) {
        return tlsError("Cannot apply TLS settings, so this OpenSSL may be too old");
    }
    SSL_CTX_set_options(native, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
                                    SSL_OP_CIPHER_SERVER_PREFERENCE);
    // Frees buffers on idle connections, which is most of what an open connection would hold.
    SSL_CTX_set_mode(native, SSL_MODE_RELEASE_BUFFERS);
    // Clients can still resume sessions with tickets, which the server doesn't have to store.
    SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_OFF);

    boost::system::error_code error;

    // Without this, OpenSSL asks for a key's password on the terminal, which would hang a service.
    context->set_password_callback(
        [](std::size_t, ssl::context::password_purpose) { return std::string{}; }, error);
    if (error) {
        return tlsError("Cannot set up TLS: " + error.message());
    }

    context->use_certificate_chain_file(options.certificate_file, error);
    if (error) {
        return tlsError("Cannot load the TLS certificate " + options.certificate_file + ": " +
                        error.message());
    }

    context->use_private_key_file(options.private_key_file, ssl::context::pem, error);
    if (error) {
        return tlsError("Cannot load the TLS private key " + options.private_key_file + ": " +
                        error.message() +
                        ". It must be in PEM format with no password, and match the certificate");
    }
    if (SSL_CTX_check_private_key(native) != 1) {
        return tlsError("The TLS private key " + options.private_key_file +
                        " doesn't match the certificate " + options.certificate_file);
    }

    if (X509* const certificate = SSL_CTX_get0_certificate(native);
        certificate != nullptr && log_error &&
        X509_cmp_current_time(X509_get0_notAfter(certificate)) < 0) {
        log_error("The TLS certificate " + options.certificate_file +
                  " has expired, so clients will refuse to connect");
    }
    return context;
}

}  // namespace cppboot::web
