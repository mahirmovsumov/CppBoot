#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cppboot/cppboot.hpp>
#include <cppboot/web/server_module.hpp>
#include <filesystem>
#include <future>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot;
using namespace cppboot::web;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

constexpr std::string_view kLoopback = "127.0.0.1";

/// A small synchronous HTTP client with a read deadline, so that a server bug fails the test
/// rather than hanging it.
class Client {
public:
    explicit Client(std::uint16_t port) : stream_(io_) {
        stream_.expires_after(std::chrono::seconds{20});
        stream_.connect(tcp::endpoint{asio::ip::make_address(kLoopback), port});
    }

    http::response<http::string_body> send(http::verb method, const std::string& target,
                                           const std::string& body = {}) {
        http::request<http::string_body> request{method, target, 11};
        request.set(http::field::host, "localhost");
        request.body() = body;
        request.prepare_payload();

        stream_.expires_after(std::chrono::seconds{20});
        http::write(stream_, request);
        return read(method == http::verb::head);
    }

    http::response<http::string_body> read(bool head = false) {
        http::response_parser<http::string_body> parser;
        parser.skip(head);
        stream_.expires_after(std::chrono::seconds{20});
        http::read(stream_, buffer_, parser);
        return parser.release();
    }

    void sendRaw(std::string_view text) {
        stream_.expires_after(std::chrono::seconds{20});
        asio::write(stream_, asio::buffer(text));
    }

    /// Returns true if the server closes the connection.
    bool closedByServer() {
        boost::system::error_code error;
        stream_.expires_after(std::chrono::seconds{20});
        http::response<http::string_body> response;
        http::read(stream_, buffer_, response, error);
        return error == http::error::end_of_stream ||
               error == asio::error::connection_reset || error == asio::error::eof;
    }

private:
    asio::io_context io_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
};

ServerOptions testOptions() {
    ServerOptions options;
    options.port = 0;
    options.worker_threads = 4;
    return options;
}

}  // namespace

class ServerTest : public ::testing::Test {
protected:
    Server& startServer(ServerOptions options = testOptions()) {
        server_ = std::make_unique<Server>(router_, std::move(options));
        const auto started = server_->start();
        EXPECT_TRUE(started) << (started ? "" : started.error().message());
        return *server_;
    }

    Router router_;
    std::unique_ptr<Server> server_;
};

TEST_F(ServerTest, ServesARequest) {
    router_.get("/users/{id}", [](const Request& request) {
        return Response::json("{\"id\":" + *request.param("id") + "}");
    });
    const auto port = startServer().port();

    Client client{port};
    const auto response = client.send(http::verb::get, "/users/7");
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body(), "{\"id\":7}");
    EXPECT_EQ(response[http::field::content_type], "application/json");
}

class ClientAddressTest : public ServerTest {
protected:
    /// Sends a request with these X-Forwarded-For headers, and returns the address the handler saw.
    std::string addressSeen(const std::vector<std::string>& trusted_proxies,
                            const std::vector<std::string>& forwarded_for) {
        router_.get("/address",
                    [](const Request& request) { return Response::text(request.clientAddress()); });
        ServerOptions options = testOptions();
        options.trusted_proxies = trusted_proxies;
        const auto port = startServer(std::move(options)).port();

        std::string text = "GET /address HTTP/1.1\r\nHost: localhost\r\n";
        for (const std::string& value : forwarded_for) {
            text += "X-Forwarded-For: " + value + "\r\n";
        }
        text += "\r\n";

        Client client{port};
        client.sendRaw(text);
        return client.read().body();
    }
};

TEST_F(ClientAddressTest, IsThePeerWithNoProxy) {
    EXPECT_EQ(addressSeen({}, {}), "127.0.0.1");
}

TEST_F(ClientAddressTest, IgnoresForwardedForFromAnUntrustedPeer) {
    EXPECT_EQ(addressSeen({}, {"203.0.113.9"}), "127.0.0.1");
}

TEST_F(ClientAddressTest, IgnoresForwardedForWhenThePeerIsNotInTheList) {
    EXPECT_EQ(addressSeen({"10.0.0.1", "192.0.2.0/24"}, {"203.0.113.9"}), "127.0.0.1");
}

TEST_F(ClientAddressTest, ReadsForwardedForFromATrustedProxy) {
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {"203.0.113.9"}), "203.0.113.9");
}

TEST_F(ClientAddressTest, TrustsAProxyInARange) {
    EXPECT_EQ(addressSeen({"127.0.0.0/8"}, {"203.0.113.9"}), "203.0.113.9");
}

TEST_F(ClientAddressTest, TheClientCantForgeTheStartOfTheList) {
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {"1.1.1.1, 2.2.2.2, 203.0.113.9"}), "203.0.113.9");
}

TEST_F(ClientAddressTest, SkipsEveryTrustedProxyFromTheEnd) {
    EXPECT_EQ(addressSeen({"127.0.0.1", "10.0.0.0/8"}, {"1.1.1.1, 203.0.113.9, 10.0.0.2, 10.0.0.1"}),
              "203.0.113.9");
}

TEST_F(ClientAddressTest, ReadsSeveralHeadersInOrder) {
    EXPECT_EQ(addressSeen({"127.0.0.1", "10.0.0.1"}, {"1.1.1.1", "203.0.113.9, 10.0.0.1"}),
              "203.0.113.9");
}

TEST_F(ClientAddressTest, UsesTheLastAddressItCanReadWhenAnEntryIsInvalid) {
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {"not-an-address"}), "127.0.0.1");
    server_.reset();
    router_ = Router{};
    EXPECT_EQ(addressSeen({"127.0.0.1", "10.0.0.1"}, {"forged, 10.0.0.1"}), "10.0.0.1");
}

TEST_F(ClientAddressTest, UsesThePeerWhenTheHeaderIsEmpty) {
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {" , "}), "127.0.0.1");
}

TEST_F(ClientAddressTest, WritesIpv6InOneForm) {
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {"::ffff:203.0.113.9"}), "203.0.113.9");
    server_.reset();
    router_ = Router{};
    EXPECT_EQ(addressSeen({"127.0.0.1"}, {"2001:DB8::1"}), "2001:db8::1");
}

TEST_F(ServerTest, RejectsRequestsThatWaitedTooLongForAWorker) {
    std::promise<void> entered;
    std::promise<void> release;
    std::atomic<int> quick_calls{0};
    router_.get("/block", [&](const Request&) {
        entered.set_value();
        release.get_future().wait();
        return Response::text("done");
    });
    router_.get("/quick", [&](const Request&) {
        quick_calls.fetch_add(1);
        return Response::text("quick");
    });

    std::mutex log_mutex;
    std::vector<std::string> logged;
    ServerOptions options = testOptions();
    options.worker_threads = 1;
    options.queue_timeout = std::chrono::milliseconds{100};
    Server server{router_, std::move(options), [&](std::string_view message) {
                      const std::lock_guard lock{log_mutex};
                      logged.emplace_back(message);
                  }};
    ASSERT_TRUE(server.start());

    Client blocker{server.port()};
    auto blocked = std::async(std::launch::async,
                              [&] { return blocker.send(http::verb::get, "/block"); });
    entered.get_future().wait();

    // These wait behind the blocked handler for longer than they're allowed.
    Client first{server.port()};
    Client second{server.port()};
    auto first_response = std::async(std::launch::async,
                                     [&] { return first.send(http::verb::get, "/quick"); });
    auto second_response = std::async(std::launch::async,
                                      [&] { return second.send(http::verb::get, "/quick"); });
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    release.set_value();

    EXPECT_EQ(blocked.get().result_int(), 200);
    for (auto* response : {&first_response, &second_response}) {
        const auto rejected = response->get();
        EXPECT_EQ(rejected.result_int(), 503);
        EXPECT_EQ(rejected[http::field::retry_after], "1");
    }
    EXPECT_EQ(quick_calls.load(), 0);

    // The connection and the server both still work, and the backlog has gone.
    EXPECT_EQ(first.send(http::verb::get, "/quick").result_int(), 200);
    EXPECT_EQ(quick_calls.load(), 1);

    server.stop();
    const std::lock_guard lock{log_mutex};
    ASSERT_FALSE(logged.empty());
    EXPECT_NE(logged.front().find("Too busy"), std::string::npos);
}

TEST(ReadServerOptionsTest, ReadsTheQueueTimeout) {
    config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    EXPECT_EQ(readServerOptions(config::Config::load(sources).value()).value().queue_timeout,
              std::chrono::seconds{5});

    sources.defaults = {{"cppboot.server.queue_timeout_seconds", "2"}};
    EXPECT_EQ(readServerOptions(config::Config::load(sources).value()).value().queue_timeout,
              std::chrono::seconds{2});

    sources.defaults = {{"cppboot.server.queue_timeout_seconds", "0"}};
    EXPECT_FALSE(readServerOptions(config::Config::load(sources).value()));
}

TEST_F(ServerTest, RefusesToStartWithAnInvalidTrustedProxy) {
    for (const char* proxy : {"not-an-address", "10.0.0.1/33", "10.0.0.1/x", "10.0.0.1/", "::1/129",
                              "0.0.0.0/0", "::/0", "10.0.0.1/-1"}) {
        ServerOptions options = testOptions();
        options.trusted_proxies = {proxy};
        Server server{router_, std::move(options)};
        const auto started = server.start();
        ASSERT_FALSE(started) << proxy;
        EXPECT_NE(started.error().message().find("Invalid trusted proxy"), std::string::npos);
    }
}

TEST(ReadServerOptionsTest, SplitsTrustedProxies) {
    config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = {{"cppboot.server.trusted_proxies", " 10.0.0.1, 10.1.0.0/16 ,,::1 "}};
    const auto options = readServerOptions(config::Config::load(sources).value());
    ASSERT_TRUE(options);
    EXPECT_EQ(options.value().trusted_proxies,
              (std::vector<std::string>{"10.0.0.1", "10.1.0.0/16", "::1"}));
}

TEST_F(ServerTest, PassesBodyAndHeadersToHandlers) {
    router_.post("/echo", [](const Request& request) {
        return Response::text(request.body() + "|" + request.header("Host").value_or(""), 201);
    });
    const auto port = startServer().port();

    Client client{port};
    const auto response = client.send(http::verb::post, "/echo", "hello");
    EXPECT_EQ(response.result_int(), 201);
    EXPECT_EQ(response.body(), "hello|localhost");
}

TEST_F(ServerTest, ReusesAConnection) {
    std::atomic<int> count{0};
    router_.get("/", [&](const Request&) { return Response::text(std::to_string(++count)); });
    const auto port = startServer().port();

    Client client{port};
    EXPECT_EQ(client.send(http::verb::get, "/").body(), "1");
    EXPECT_EQ(client.send(http::verb::get, "/").body(), "2");
    EXPECT_EQ(client.send(http::verb::get, "/").body(), "3");
}

TEST_F(ServerTest, HeadReportsSizeWithoutABody) {
    router_.get("/", [](const Request&) { return Response::text("hello"); });
    const auto port = startServer().port();

    Client client{port};
    const auto response = client.send(http::verb::head, "/");
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response[http::field::content_length], "5");
    EXPECT_TRUE(response.body().empty());

    // The connection must still be in step for the next request.
    EXPECT_EQ(client.send(http::verb::get, "/").body(), "hello");
}

TEST_F(ServerTest, SlowHandlerDoesNotBlockOtherRequests) {
    std::latch fast_finished{1};
    router_.get("/slow", [&](const Request&) {
        fast_finished.wait();
        return Response::text("slow");
    });
    router_.get("/fast", [&](const Request&) {
        fast_finished.count_down();
        return Response::text("fast");
    });
    const auto port = startServer().port();

    auto slow = std::async(std::launch::async, [port] {
        Client client{port};
        return client.send(http::verb::get, "/slow").body();
    });

    // /slow can only finish after /fast has run, which needs /slow to not block the server.
    Client client{port};
    EXPECT_EQ(client.send(http::verb::get, "/fast").body(), "fast");
    EXPECT_EQ(slow.get(), "slow");
}

TEST_F(ServerTest, HandlesManyClientsAtOnce) {
    router_.get("/echo/{value}",
                [](const Request& request) { return Response::text(*request.param("value")); });
    const auto port = startServer().port();

    std::atomic<int> correct{0};
    {
        std::vector<std::jthread> clients;
        for (int i = 0; i < 16; ++i) {
            clients.emplace_back([&, i] {
                Client client{port};
                for (int j = 0; j < 20; ++j) {
                    const std::string value = std::to_string(i * 100 + j);
                    if (client.send(http::verb::get, "/echo/" + value).body() == value) {
                        ++correct;
                    }
                }
            });
        }
    }
    EXPECT_EQ(correct.load(), 16 * 20);
}

TEST_F(ServerTest, RejectsAnOversizedBody) {
    router_.post("/", [](const Request&) { return Response{}; });
    ServerOptions options = testOptions();
    options.max_body_bytes = 10;
    const auto port = startServer(std::move(options)).port();

    Client client{port};
    const auto response = client.send(http::verb::post, "/", std::string(100, 'x'));
    EXPECT_EQ(response.result_int(), 413);
}

TEST_F(ServerTest, RejectsMalformedRequests) {
    const auto port = startServer().port();

    Client client{port};
    client.sendRaw("NOT HTTP AT ALL\r\n\r\n");
    EXPECT_EQ(client.read().result_int(), 400);
}

TEST_F(ServerTest, RejectsAnInvalidTarget) {
    const auto port = startServer().port();

    Client client{port};
    EXPECT_EQ(client.send(http::verb::get, "/a%zz").result_int(), 400);
}

TEST_F(ServerTest, RejectsAnUnsupportedMethod) {
    const auto port = startServer().port();

    Client client{port};
    EXPECT_EQ(client.send(http::verb::trace, "/").result_int(), 400);
}

TEST_F(ServerTest, ClosesAConnectionThatStaysSilent) {
    ServerOptions options = testOptions();
    options.request_timeout = std::chrono::milliseconds{100};
    const auto port = startServer(std::move(options)).port();

    Client client{port};
    EXPECT_TRUE(client.closedByServer());
}

TEST_F(ServerTest, ClosesConnectionsBeyondTheLimit) {
    router_.get("/", [](const Request&) { return Response::text("ok"); });
    ServerOptions options = testOptions();
    options.max_connections = 1;
    const auto port = startServer(std::move(options)).port();

    Client first{port};
    EXPECT_EQ(first.send(http::verb::get, "/").body(), "ok");

    Client second{port};
    EXPECT_TRUE(second.closedByServer());
}

TEST_F(ServerTest, FailsToStartOnAPortInUse) {
    const auto port = startServer().port();

    ServerOptions options;
    options.port = port;
    Server second{router_, options};
    const auto started = second.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code(), ErrorCode::IoError);
}

TEST_F(ServerTest, FailsToStartOnAnInvalidAddress) {
    ServerOptions options = testOptions();
    options.host = "not an address";
    Server server{router_, options};
    EXPECT_FALSE(server.start());
}

TEST_F(ServerTest, CannotStartTwice) {
    Server& server = startServer();
    EXPECT_FALSE(server.start());
}

TEST_F(ServerTest, StopWithoutStartIsSafe) {
    Server server{router_, testOptions()};
    server.stop();
    server.stop();
}

TEST_F(ServerTest, StopClosesIdleConnections) {
    Server& server = startServer();
    Client client{server.port()};

    server.stop();
    EXPECT_TRUE(client.closedByServer());
}

TEST_F(ServerTest, StopLetsARunningRequestFinish) {
    std::latch handler_started{1};
    std::latch stop_called{1};
    router_.get("/", [&](const Request&) {
        handler_started.count_down();
        stop_called.wait();
        return Response::text("finished");
    });
    Server& server = startServer();

    auto request = std::async(std::launch::async, [port = server.port()] {
        Client client{port};
        return client.send(http::verb::get, "/").body();
    });
    handler_started.wait();

    auto stopping = std::async(std::launch::async, [&] { server.stop(); });
    stop_called.count_down();
    stopping.get();

    EXPECT_EQ(request.get(), "finished");
}

TEST_F(ServerTest, StopForcesOutASlowClientAfterTheTimeout) {
    ServerOptions options = testOptions();
    options.shutdown_timeout = std::chrono::milliseconds{50};
    Server& server = startServer(std::move(options));

    // Part of a request, which keeps the connection reading rather than idle at stop time.
    Client client{server.port()};
    client.sendRaw("GET / HTTP/1.1\r\nHost: x\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const auto before = std::chrono::steady_clock::now();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds{10});
}

TEST(ServerOptionsTest, ReadsSettingsAndChecksRanges) {
    config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = {{"cppboot.server.host", "0.0.0.0"},
                        {"cppboot.server.port", "9000"},
                        {"cppboot.server.request_timeout_seconds", "5"}};

    const auto options = readServerOptions(config::Config::load(sources).value());
    ASSERT_TRUE(options);
    EXPECT_EQ(options.value().host, "0.0.0.0");
    EXPECT_EQ(options.value().port, 9000);
    EXPECT_EQ(options.value().request_timeout, std::chrono::seconds{5});

    sources.defaults["cppboot.server.port"] = "70000";
    EXPECT_FALSE(readServerOptions(config::Config::load(sources).value()));

    sources.defaults["cppboot.server.port"] = "abc";
    EXPECT_FALSE(readServerOptions(config::Config::load(sources).value()));
}

TEST(ServerModuleTest, DefaultHostIsLocalOnly) {
    EXPECT_EQ(ServerOptions{}.host, "127.0.0.1");
}

TEST(ServerModuleTest, RunsAServerWithinAnApplication) {
    const auto router = std::make_shared<Router>();
    router->get("/hello", [](const Request&) { return Response::text("hello"); });

    ApplicationOptions options;
    options.config_directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    options.environment = std::map<std::string, std::string>{};
    options.default_settings = {{"cppboot.logging.level", "off"}, {"cppboot.server.port", "0"}};

    Application app{0, nullptr, options};
    auto module_owner = std::make_unique<ServerModule>(router);
    ServerModule& server_module = *module_owner;
    app.addModule(std::move(module_owner));

    auto running = std::async(std::launch::async, [&] { return app.run(); });
    while (app.state() != LifecycleState::Running) {
        std::this_thread::yield();
    }

    {
        Client client{server_module.port()};
        EXPECT_EQ(client.send(http::verb::get, "/hello").body(), "hello");
    }

    app.requestShutdown();
    EXPECT_EQ(running.get(), kExitSuccess);
}

TEST(ServerModuleTest, PortInUseFailsStartup) {
    const auto router = std::make_shared<Router>();
    Server occupying{*router, testOptions()};
    ASSERT_TRUE(occupying.start());

    ApplicationOptions options;
    options.config_directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    options.environment = std::map<std::string, std::string>{};
    options.default_settings = {{"cppboot.logging.level", "off"},
                                {"cppboot.server.port", std::to_string(occupying.port())}};

    Application app{0, nullptr, options};
    app.emplaceModule<ServerModule>(router);
    EXPECT_EQ(app.run(), kExitFailure);
}
