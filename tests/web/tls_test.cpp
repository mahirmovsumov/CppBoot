#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <cppboot/web/server_module.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot;
using namespace cppboot::web;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using asio::ip::tcp;

constexpr std::string_view kLoopback = "127.0.0.1";

using KeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

KeyPointer makeKey() {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free};
    EVP_PKEY_keygen_init(context.get());
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(context.get(), &key);
    return KeyPointer{key, EVP_PKEY_free};
}

void writeKey(EVP_PKEY* key, const std::filesystem::path& path, const char* password = nullptr) {
    const std::unique_ptr<BIO, decltype(&BIO_free)> file{BIO_new_file(path.string().c_str(), "w"),
                                                         BIO_free};
    ASSERT_NE(file, nullptr);
    const EVP_CIPHER* const cipher = password != nullptr ? EVP_aes_128_cbc() : nullptr;
    ASSERT_EQ(PEM_write_bio_PrivateKey(file.get(), key, cipher, nullptr, 0, nullptr,
                                       const_cast<char*>(password)),
              1);
}

/// Writes a certificate for localhost and 127.0.0.1, signed by its own key.
void writeCertificate(EVP_PKEY* key, const std::filesystem::path& path, long valid_from_seconds,
                      long valid_to_seconds) {
    const std::unique_ptr<X509, decltype(&X509_free)> certificate{X509_new(), X509_free};
    X509* const raw = certificate.get();
    X509_set_version(raw, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(raw), 1);
    X509_gmtime_adj(X509_getm_notBefore(raw), valid_from_seconds);
    X509_gmtime_adj(X509_getm_notAfter(raw), valid_to_seconds);
    X509_set_pubkey(raw, key);

    X509_NAME* const name = X509_get_subject_name(raw);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(raw, name);

    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, raw, raw, nullptr, nullptr, 0);
    X509_EXTENSION* const names = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name,
                                                      "DNS:localhost,IP:127.0.0.1");
    ASSERT_NE(names, nullptr);
    X509_add_ext(raw, names, -1);
    X509_EXTENSION_free(names);

    ASSERT_GT(X509_sign(raw, key, EVP_sha256()), 0);

    const std::unique_ptr<BIO, decltype(&BIO_free)> file{BIO_new_file(path.string().c_str(), "w"),
                                                         BIO_free};
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(PEM_write_bio_X509(file.get(), raw), 1);
}

/// A synchronous HTTPS client that checks the server's certificate, as a real client would.
class TlsClient {
public:
    using Configure = std::function<void(SSL_CTX*)>;

    TlsClient(std::uint16_t port, const std::filesystem::path& trusted_certificate,
              const Configure& configure = {})
        : context_(ssl::context::tls_client), port_(port) {
        context_.load_verify_file(trusted_certificate.string());
        context_.set_verify_mode(ssl::verify_peer);
        if (configure) {
            configure(context_.native_handle());
        }
        stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(io_, context_);
        SSL_set_tlsext_host_name(stream_->native_handle(), "localhost");
        stream_->set_verify_callback(ssl::host_name_verification("localhost"));
    }

    /// Returns false if the handshake failed.
    bool connect() {
        auto& transport = beast::get_lowest_layer(*stream_);
        transport.expires_after(std::chrono::seconds{20});
        transport.connect(tcp::endpoint{asio::ip::make_address(kLoopback), port_});

        boost::system::error_code error;
        stream_->handshake(ssl::stream_base::client, error);
        return !error;
    }

    std::string negotiatedVersion() const { return SSL_get_version(stream_->native_handle()); }

    http::response<http::string_body> send(http::verb method, const std::string& target,
                                           const std::string& body = {},
                                           bool keep_alive = true) {
        http::request<http::string_body> request{method, target, 11};
        request.set(http::field::host, "localhost");
        request.body() = body;
        request.keep_alive(keep_alive);
        request.prepare_payload();

        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds{20});
        http::write(*stream_, request);
        return read();
    }

    http::response<http::string_body> read() {
        http::response_parser<http::string_body> parser;
        parser.body_limit(16 * 1024 * 1024);
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds{20});
        http::read(*stream_, buffer_, parser);
        return parser.release();
    }

    /// Returns the error from reading after the server has finished.
    boost::system::error_code readUntilClosed() {
        std::array<char, 256> bytes{};
        boost::system::error_code error;
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds{20});
        while (!error) {
            stream_->read_some(asio::buffer(bytes), error);
        }
        return error;
    }

private:
    asio::io_context io_;
    ssl::context context_;
    std::uint16_t port_;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
    beast::flat_buffer buffer_;
};

/// Opens a connection that never starts a handshake, as a slow or hostile client might.
class SilentClient {
public:
    explicit SilentClient(std::uint16_t port) : socket_(io_) {
        socket_.connect(tcp::endpoint{asio::ip::make_address(kLoopback), port});
    }

    void sendRaw(std::string_view text) { asio::write(socket_, asio::buffer(text)); }

    /// Returns how long the server took to close the connection.
    std::chrono::milliseconds timeUntilClosed() {
        const auto start = std::chrono::steady_clock::now();
        std::array<char, 256> bytes{};
        boost::system::error_code error;
        while (!error) {
            socket_.read_some(asio::buffer(bytes), error);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    }

private:
    asio::io_context io_;
    tcp::socket socket_;
};

void limitToVersion(SSL_CTX* context, int version) {
    SSL_CTX_set_min_proto_version(context, version);
    SSL_CTX_set_max_proto_version(context, version);
}

}  // namespace

class TlsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ =
            std::filesystem::temp_directory_path() / (std::string{"cppboot-tls-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        certificate_ = directory_ / "cert.pem";
        private_key_ = directory_ / "key.pem";

        key_ = makeKey();
        writeKey(key_.get(), private_key_);
        writeCertificate(key_.get(), certificate_, -60, 3600);

        router_.get("/hello", [this](const Request& request) {
            calls_.fetch_add(1);
            return Response::text("hello from " + request.clientAddress());
        });
        router_.post("/echo", [](const Request& request) { return Response::text(request.body()); });
    }

    void TearDown() override {
        server_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    ServerOptions tlsOptions() const {
        ServerOptions options;
        options.port = 0;
        options.worker_threads = 2;
        options.tls = TlsOptions{certificate_.string(), private_key_.string()};
        return options;
    }

    std::uint16_t startServer(ServerOptions options) {
        server_ = std::make_unique<Server>(router_, std::move(options));
        const auto started = server_->start();
        EXPECT_TRUE(started) << (started ? "" : started.error().message());
        return server_->port();
    }

    TlsClient client(std::uint16_t port, const TlsClient::Configure& configure = {}) const {
        return TlsClient{port, certificate_, configure};
    }

    /// Returns why the server refused to start with these options.
    std::string startError(ServerOptions options) {
        Server server{router_, std::move(options)};
        const auto started = server.start();
        EXPECT_FALSE(started);
        return started ? "" : started.error().message();
    }

    std::filesystem::path directory_;
    std::filesystem::path certificate_;
    std::filesystem::path private_key_;
    KeyPointer key_{nullptr, EVP_PKEY_free};
    std::atomic<int> calls_{0};
    Router router_;
    std::unique_ptr<Server> server_;
};

TEST_F(TlsTest, ServesARequestToAClientThatChecksTheCertificate) {
    auto https = client(startServer(tlsOptions()));
    ASSERT_TRUE(https.connect());
    EXPECT_EQ(https.negotiatedVersion(), "TLSv1.3");

    const auto response = https.send(http::verb::get, "/hello");
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body(), "hello from 127.0.0.1");
}

TEST_F(TlsTest, KeepsAConnectionOpenAcrossRequests) {
    auto https = client(startServer(tlsOptions()));
    ASSERT_TRUE(https.connect());
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(https.send(http::verb::get, "/hello").result_int(), 200);
    }
    EXPECT_EQ(calls_.load(), 50);
}

TEST_F(TlsTest, KeepsAcceptingMoreConnectionsThanItHasWaitingAccepts) {
    const auto port = startServer(tlsOptions());

    // One after another, each closed by the client without a clean TLS shutdown.
    for (int i = 0; i < 200; ++i) {
        auto https = client(port);
        ASSERT_TRUE(https.connect()) << "connection " << i;
        ASSERT_EQ(https.send(http::verb::get, "/hello").result_int(), 200) << "connection " << i;
    }

    // All open at once.
    std::vector<std::unique_ptr<TlsClient>> open;
    for (int i = 0; i < 200; ++i) {
        open.push_back(std::make_unique<TlsClient>(port, certificate_));
        ASSERT_TRUE(open.back()->connect()) << "open connection " << i;
    }
    for (const auto& https : open) {
        ASSERT_EQ(https->send(http::verb::get, "/hello").result_int(), 200);
    }
}

TEST_F(TlsTest, CarriesBodiesLargerThanOneTlsRecord) {
    ServerOptions options = tlsOptions();
    options.max_body_bytes = 4 * 1024 * 1024;
    auto https = client(startServer(std::move(options)));
    ASSERT_TRUE(https.connect());

    const std::string body(3 * 1024 * 1024, 'x');
    const auto response = https.send(http::verb::post, "/echo", body);
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body() == body, true);
}

TEST_F(TlsTest, EnforcesTheBodyLimit) {
    ServerOptions options = tlsOptions();
    options.max_body_bytes = 10;
    auto https = client(startServer(std::move(options)));
    ASSERT_TRUE(https.connect());
    EXPECT_EQ(https.send(http::verb::post, "/echo", std::string(100, 'x')).result_int(), 413);
}

TEST_F(TlsTest, ClosesCleanlySoThatAClientCanTrustTheResponseIsComplete) {
    auto https = client(startServer(tlsOptions()));
    ASSERT_TRUE(https.connect());

    ASSERT_EQ(https.send(http::verb::get, "/hello", {}, false).result_int(), 200);

    // Without close_notify, this would be a truncation error, which is how an attacker cutting a
    // response short would look.
    EXPECT_EQ(https.readUntilClosed(), asio::error::eof);
}

TEST_F(TlsTest, PlainHttpNeverReachesAHandler) {
    SilentClient plain{startServer(tlsOptions())};
    plain.sendRaw("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
    static_cast<void>(plain.timeUntilClosed());
    EXPECT_EQ(calls_.load(), 0);
}

TEST_F(TlsTest, GarbageDoesNotStopTheServer) {
    const auto port = startServer(tlsOptions());
    for (int i = 0; i < 20; ++i) {
        SilentClient garbage{port};
        garbage.sendRaw(std::string(1000, static_cast<char>(i * 13)));
        static_cast<void>(garbage.timeUntilClosed());
    }

    auto https = client(port);
    ASSERT_TRUE(https.connect());
    EXPECT_EQ(https.send(http::verb::get, "/hello").result_int(), 200);
}

TEST_F(TlsTest, AcceptsTls12ByDefault) {
    auto https = client(startServer(tlsOptions()), [](SSL_CTX* context) {
        limitToVersion(context, TLS1_2_VERSION);
    });
    ASSERT_TRUE(https.connect());
    EXPECT_EQ(https.negotiatedVersion(), "TLSv1.2");
}

TEST_F(TlsTest, RefusesVersionsOlderThanTls12) {
    const auto port = startServer(tlsOptions());
    for (const int version : {TLS1_VERSION, TLS1_1_VERSION}) {
        auto https = client(port, [version](SSL_CTX* context) {
            // Security level 0 stops the client's own OpenSSL from refusing these first.
            SSL_CTX_set_security_level(context, 0);
            limitToVersion(context, version);
        });
        EXPECT_FALSE(https.connect()) << version;
    }
}

TEST_F(TlsTest, MinimumVersionCanBeRaised) {
    ServerOptions options = tlsOptions();
    options.tls->min_version = "1.3";
    const auto port = startServer(std::move(options));

    auto old_client = client(
        port, [](SSL_CTX* context) { limitToVersion(context, TLS1_2_VERSION); });
    EXPECT_FALSE(old_client.connect());

    auto new_client = client(port);
    EXPECT_TRUE(new_client.connect());
}

TEST_F(TlsTest, RefusesOldCiphers) {
    auto https = client(startServer(tlsOptions()), [](SSL_CTX* context) {
        limitToVersion(context, TLS1_2_VERSION);
        // CBC ciphers that the server's key could use, so that only its cipher list refuses them.
        SSL_CTX_set_cipher_list(context, "ECDHE-ECDSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA256");
    });
    EXPECT_FALSE(https.connect());
}

TEST_F(TlsTest, AClientThatNeverFinishesAHandshakeIsDisconnected) {
    ServerOptions options = tlsOptions();
    options.request_timeout = std::chrono::milliseconds{300};
    SilentClient silent{startServer(std::move(options))};
    EXPECT_LT(silent.timeUntilClosed(), std::chrono::seconds{10});
}

TEST_F(TlsTest, SilentClientsDontHoldUpOthers) {
    const auto port = startServer(tlsOptions());
    std::vector<std::unique_ptr<SilentClient>> silent;
    for (int i = 0; i < 50; ++i) {
        silent.push_back(std::make_unique<SilentClient>(port));
    }

    const auto start = std::chrono::steady_clock::now();
    auto https = client(port);
    ASSERT_TRUE(https.connect());
    EXPECT_EQ(https.send(http::verb::get, "/hello").result_int(), 200);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
}

TEST_F(TlsTest, StopDoesNotWaitForIdleOrSilentClients) {
    const auto port = startServer(tlsOptions());
    auto idle = client(port);
    ASSERT_TRUE(idle.connect());
    ASSERT_EQ(idle.send(http::verb::get, "/hello").result_int(), 200);
    SilentClient silent{port};

    const auto start = std::chrono::steady_clock::now();
    server_->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
}

TEST_F(TlsTest, StopLetsARunningRequestFinish) {
    std::promise<void> entered;
    std::promise<void> release;
    router_.get("/slow", [&](const Request&) {
        entered.set_value();
        release.get_future().wait();
        return Response::text("finished");
    });
    const auto port = startServer(tlsOptions());

    auto https = client(port);
    ASSERT_TRUE(https.connect());
    auto response = std::async(std::launch::async,
                               [&] { return https.send(http::verb::get, "/slow"); });
    entered.get_future().wait();

    std::jthread stopper{[this] { server_->stop(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    release.set_value();

    const auto finished = response.get();
    EXPECT_EQ(finished.result_int(), 200);
    EXPECT_EQ(finished.body(), "finished");
    EXPECT_FALSE(finished.keep_alive());
}

TEST_F(TlsTest, LogsAnExpiredCertificateButStillStarts) {
    writeCertificate(key_.get(), certificate_, -7200, -3600);

    std::mutex mutex;
    std::vector<std::string> logged;
    Server server{router_, tlsOptions(), [&](std::string_view message) {
                      const std::lock_guard lock{mutex};
                      logged.emplace_back(message);
                  }};
    ASSERT_TRUE(server.start());
    server.stop();

    ASSERT_EQ(logged.size(), 1U);
    EXPECT_NE(logged[0].find("has expired"), std::string::npos);
}

TEST_F(TlsTest, RefusesToStartWithAMissingCertificate) {
    ServerOptions options = tlsOptions();
    options.tls->certificate_file = (directory_ / "absent.pem").string();
    EXPECT_NE(startError(std::move(options)).find("Cannot load the TLS certificate"),
              std::string::npos);
}

TEST_F(TlsTest, RefusesToStartWithAMissingKey) {
    ServerOptions options = tlsOptions();
    options.tls->private_key_file = (directory_ / "absent.pem").string();
    EXPECT_NE(startError(std::move(options)).find("Cannot load the TLS private key"),
              std::string::npos);
}

TEST_F(TlsTest, RefusesToStartWithFilesThatAreNotPem) {
    const auto garbage = directory_ / "garbage.pem";
    std::ofstream{garbage} << "this isn't a certificate";

    ServerOptions bad_certificate = tlsOptions();
    bad_certificate.tls->certificate_file = garbage.string();
    EXPECT_FALSE(startError(std::move(bad_certificate)).empty());

    ServerOptions bad_key = tlsOptions();
    bad_key.tls->private_key_file = garbage.string();
    EXPECT_FALSE(startError(std::move(bad_key)).empty());
}

TEST_F(TlsTest, RefusesToStartWithAKeyFromAnotherCertificate) {
    const auto other_path = directory_ / "other.pem";
    const KeyPointer other = makeKey();
    writeKey(other.get(), other_path);

    ServerOptions options = tlsOptions();
    options.tls->private_key_file = other_path.string();
    EXPECT_NE(startError(std::move(options)).find("private key"), std::string::npos);
}

TEST_F(TlsTest, RefusesAKeyWithAPasswordInsteadOfWaitingForOneToBeTyped) {
    const auto locked_path = directory_ / "locked.pem";
    writeKey(key_.get(), locked_path, "a password");

    ServerOptions options = tlsOptions();
    options.tls->private_key_file = locked_path.string();
    auto error = std::async(std::launch::async,
                            [&] { return startError(std::move(options)); });
    ASSERT_EQ(error.wait_for(std::chrono::seconds{30}), std::future_status::ready);
    EXPECT_NE(error.get().find("no password"), std::string::npos);
}

TEST_F(TlsTest, RefusesAnUnknownMinimumVersion) {
    for (const char* version : {"1.1", "1.0", "1.4", "", "tls1.2"}) {
        ServerOptions options = tlsOptions();
        options.tls->min_version = version;
        EXPECT_NE(startError(std::move(options)).find("must be 1.2 or 1.3"), std::string::npos)
            << version;
    }
}

TEST_F(TlsTest, ErrorsDontContainTheKey) {
    std::stringstream key_text;
    key_text << std::ifstream{private_key_}.rdbuf();
    const std::string key_body = key_text.str().substr(40, 40);

    ServerOptions options = tlsOptions();
    options.tls->certificate_file = private_key_.string();
    EXPECT_EQ(startError(std::move(options)).find(key_body), std::string::npos);
}

class TlsSettingsTest : public ::testing::Test {
protected:
    static Result<ServerOptions> read(std::map<std::string, std::string> settings) {
        config::ConfigSources sources;
        sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
        sources.defaults = std::move(settings);
        return readServerOptions(config::Config::load(sources).value());
    }
};

TEST_F(TlsSettingsTest, PlainHttpWhenNothingIsSet) {
    const auto options = read({});
    ASSERT_TRUE(options);
    EXPECT_FALSE(options.value().tls.has_value());
}

TEST_F(TlsSettingsTest, ReadsBothFilesAndTheVersion) {
    const auto options = read({{"cppboot.server.tls.certificate_file", "cert.pem"},
                               {"cppboot.server.tls.private_key_file", "key.pem"},
                               {"cppboot.server.tls.min_version", "1.3"}});
    ASSERT_TRUE(options);
    ASSERT_TRUE(options.value().tls.has_value());
    EXPECT_EQ(options.value().tls->certificate_file, "cert.pem");
    EXPECT_EQ(options.value().tls->private_key_file, "key.pem");
    EXPECT_EQ(options.value().tls->min_version, "1.3");
}

TEST_F(TlsSettingsTest, HalfASetupStopsStartupRatherThanServingPlainHttp) {
    EXPECT_FALSE(read({{"cppboot.server.tls.certificate_file", "cert.pem"}}));
    EXPECT_FALSE(read({{"cppboot.server.tls.private_key_file", "key.pem"}}));
    EXPECT_FALSE(read({{"cppboot.server.tls.min_version", "1.3"}}));
}
