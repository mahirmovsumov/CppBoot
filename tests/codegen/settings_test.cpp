#include <gtest/gtest.h>

#include <chrono>
#include <cppboot/config/config.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/web/router.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "cppboot_components.generated.hpp"

using namespace cppboot::web;
using cppboot::ErrorCode;
using cppboot::Exception;
using cppboot::config::Config;
using cppboot::di::Container;

namespace {

using Settings = std::map<std::string, std::string>;

/// Registers the generated components with these settings, as an application would at startup.
class App {
public:
    explicit App(Settings settings, Settings environment = {}) {
        cppboot::config::ConfigSources sources;
        sources.defaults = std::move(settings);
        sources.environment = std::move(environment);
        // A folder with no settings file, so that only the settings above apply.
        sources.directory = std::filesystem::temp_directory_path() / "cppboot-settings-test-none";
        container_.registerInstance(std::make_shared<Config>(Config::load(sources).value()));
        cppboot::generated::registerComponents(container_);
    }

    Container& container() { return container_; }

    Response send(Method method, std::string_view target, std::string body = {}) {
        return container_.resolve<Router>()->handle(
            Request::create(method, target, {}, std::move(body)).value());
    }

private:
    Container container_;
};

/// The message of the error that these settings stop startup with.
std::string startupError(Settings settings) {
    try {
        App app{std::move(settings)};
    } catch (const Exception& exception) {
        EXPECT_EQ(exception.error().code(), ErrorCode::InvalidConfiguration);
        return exception.error().message();
    }
    ADD_FAILURE() << "the settings were accepted";
    return {};
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(SettingsTest, KeepsEachFieldsValueWhenItsSettingIsNotSet) {
    App app{{{"shop.name", "Corner"}}};
    const auto shop = app.container().resolve<shop::ShopSettings>();
    EXPECT_EQ(shop->name, "Corner");
    EXPECT_EQ(shop->port, 8080);
    EXPECT_TRUE(shop->open);
    EXPECT_EQ(shop->tax, 0.2);
    EXPECT_FALSE(shop->motto);
    EXPECT_TRUE(shop->hosts.empty());
    EXPECT_EQ(shop->timeout, std::chrono::seconds{30});
    EXPECT_EQ(shop->retry.attempts, 3);
    EXPECT_EQ(shop->retry.delay, std::chrono::milliseconds{200});
    EXPECT_EQ(app.container().resolve<shop::MailSettings>()->port, 25);
}

TEST(SettingsTest, ReadsEveryKindOfField) {
    App app{{{"shop.name", "Corner"},
             {"shop.port", "9090"},
             {"shop.open", "no"},
             {"shop.tax", "0.25"},
             {"shop.motto", "Fresh"},
             {"shop.hosts.0", "a.example"},
             {"shop.hosts.1", "b.example"},
             {"shop.timeout", "2m"},
             {"shop.retry.attempts", "5"},
             {"shop.retry.delay", "1s"},
             {"shop.level", "255"},
             {"shop.mail.host", "mail.example"},
             {"shop.mail.port", "587"}}};

    const auto shop = app.container().resolve<shop::ShopSettings>();
    EXPECT_EQ(shop->port, 9090);
    EXPECT_FALSE(shop->open);
    EXPECT_EQ(shop->tax, 0.25);
    EXPECT_EQ(shop->motto, "Fresh");
    EXPECT_EQ(shop->hosts, (std::vector<std::string>{"a.example", "b.example"}));
    EXPECT_EQ(shop->timeout, std::chrono::minutes{2});
    EXPECT_EQ(shop->retry.attempts, 5);
    EXPECT_EQ(shop->retry.delay, std::chrono::seconds{1});
    EXPECT_EQ(shop->level, 255);

    const auto mail = app.container().resolve<shop::MailSettings>();
    EXPECT_EQ(mail->host, "mail.example");
    EXPECT_EQ(mail->port, 587);
}

TEST(SettingsTest, EnvironmentVariablesSetFieldsAndListsWithCommas) {
    App app{{{"shop.name", "Corner"}},
            {{"CPPBOOT_SHOP_RETRY_ATTEMPTS", "7"}, {"CPPBOOT_SHOP_HOSTS", "a.example, b.example"}}};
    const auto shop = app.container().resolve<shop::ShopSettings>();
    EXPECT_EQ(shop->retry.attempts, 7);
    EXPECT_EQ(shop->hosts, (std::vector<std::string>{"a.example", "b.example"}));
}

TEST(SettingsTest, ComponentsTakeSettingsAsDependencies) {
    App app{{{"shop.name", "Corner"}, {"shop.mail.host", "mail.example"}}};
    EXPECT_TRUE(app.container().validate());
    EXPECT_EQ(app.container().resolve<shop::Greeter>()->greet(),
              "Welcome to Corner via mail.example");
}

TEST(SettingsTest, AMissingRequiredSettingStopsStartupAndNamesItsVariable) {
    const std::string error = startupError({});
    EXPECT_TRUE(contains(error, "Invalid settings for shop::ShopSettings:")) << error;
    EXPECT_TRUE(contains(error, "shop.name: must be set, in a settings file or with the "
                                "environment variable CPPBOOT_SHOP_NAME"))
        << error;
}

TEST(SettingsTest, ReportsEveryProblemAtOnce) {
    const std::string error = startupError({{"shop.name", ""},
                                            {"shop.port", "70000"},
                                            {"shop.open", "maybe"},
                                            {"shop.tax", "0.9"},
                                            {"shop.timeout", "30"},
                                            {"shop.retry.attempts", "0"},
                                            {"shop.retry.delay", "soon"},
                                            {"shop.level", "256"},
                                            {"shop.hosts", "a,b,c,d"}});
    for (const char* expected :
         {"shop.name: must not be empty", "shop.port: must be from 1 to 65535",
          "shop.open: must be true or false, but is 'maybe'", "shop.tax: must be from 0 to 0.5",
          "shop.timeout: must be a time with its unit, such as 500ms, 30s, 5m or 2h, but is '30'",
          "shop.retry.attempts: must be from 1 to 10", "shop.retry.delay: must be a time",
          "shop.level: must be a whole number from 0 to 255, but is '256'",
          "shop.hosts: must have from 0 to 3 items"}) {
        EXPECT_TRUE(contains(error, expected)) << expected << "\nin:\n" << error;
    }
}

TEST(SettingsTest, AWrongValueIsReportedOnceAndNotAlsoByItsCheck) {
    const std::string error = startupError({{"shop.name", "Corner"}, {"shop.port", "http"}});
    EXPECT_TRUE(contains(error, "shop.port: must be a whole number")) << error;
    EXPECT_FALSE(contains(error, "shop.port: must be from")) << error;
}

TEST(SettingsTest, ASettingThatNoFieldReadsStopsStartup) {
    const std::string error = startupError({{"shop.name", "Corner"}, {"shop.prot", "9090"}});
    EXPECT_TRUE(contains(error, "shop.prot: isn't a setting that shop::ShopSettings reads. Did "
                                "you mean shop.port?"))
        << error;

    // Each struct checks its own prefix.
    const std::string nested = startupError({{"shop.name", "Corner"}, {"shop.mail.hots", "x"}});
    EXPECT_TRUE(contains(nested, "Invalid settings for shop::MailSettings:")) << nested;
    EXPECT_TRUE(contains(nested, "shop.mail.hots")) << nested;
    EXPECT_FALSE(contains(nested, "shop::ShopSettings")) << nested;
}

TEST(SettingsTest, ASecretIsNotRepeatedInTheError) {
    const std::string error = startupError({{"shop.name", "Corner"}, {"shop.pin_secret", "hunter2"}});
    EXPECT_TRUE(contains(error, "shop.pin_secret: must be a whole number")) << error;
    EXPECT_FALSE(contains(error, "hunter2")) << error;
}

TEST(ValidationTest, AValidBodyPasses) {
    App app{{{"shop.name", "Corner"}}};
    const Response response = app.send(
        Method::Post, "/orders", R"({"product":"tea","quantity":2,"discount":0.5,"tags":["a"]})");
    EXPECT_EQ(response.status(), 200) << response.body();

    // A std::optional that isn't there has nothing to check.
    EXPECT_EQ(app.send(Method::Post, "/orders", R"({"product":"tea","quantity":1,"tags":["a"]})")
                  .status(),
              200);
}

TEST(ValidationTest, EachFailedCheckIsA400NamingTheField) {
    App app{{{"shop.name", "Corner"}}};
    const auto rejected = [&](const char* body) {
        const Response response = app.send(Method::Post, "/orders", body);
        EXPECT_EQ(response.status(), 400) << body;
        return response.body();
    };

    EXPECT_TRUE(contains(rejected(R"({"product":"","quantity":1,"tags":["a"]})"),
                         "Invalid request body: field 'product': must not be empty"));
    EXPECT_TRUE(contains(rejected(R"({"product":"a-long-name","quantity":1,"tags":["a"]})"),
                         "field 'product': must have from 1 to 8 characters"));
    EXPECT_TRUE(contains(rejected(R"({"product":"tea","quantity":0,"tags":["a"]})"),
                         "field 'quantity': must be from 1 to 100"));
    EXPECT_TRUE(contains(rejected(R"({"product":"tea","quantity":101,"tags":["a"]})"),
                         "field 'quantity': must be from 1 to 100"));
    EXPECT_TRUE(contains(rejected(R"({"product":"tea","quantity":1,"discount":-1,"tags":["a"]})"),
                         "field 'discount': must be at least 0"));
    EXPECT_TRUE(contains(rejected(R"({"product":"tea","quantity":1,"tags":[]})"),
                         "field 'tags': must have from 1 to 2 items"));
}

TEST(ValidationTest, LengthCountsCharactersNotBytes) {
    App app{{{"shop.name", "Corner"}}};
    // Eight characters, in sixteen bytes.
    const std::string body =
        R"({"product":"çöşğüəıÇ","quantity":1,"tags":["a"]})";
    EXPECT_EQ(app.send(Method::Post, "/orders", body).status(), 200);
}

TEST(ValidationTest, ChecksInANestedStructNameBothFields) {
    App app{{{"shop.name", "Corner"}}};
    const Response response = app.send(
        Method::Post, "/orders/baskets", R"({"first":{"product":"tea","quantity":0,"tags":["a"]}})");
    EXPECT_EQ(response.status(), 400);
    EXPECT_TRUE(contains(response.body(), "field 'first': field 'quantity': must be from 1 to 100"))
        << response.body();
}

TEST(ValidationTest, PathAndQueryParametersAreChecked) {
    App app{{{"shop.name", "Corner"}}};
    EXPECT_EQ(app.send(Method::Get, "/orders?size=10&page=2").body(), "\"10/2\"");
    EXPECT_EQ(app.send(Method::Get, "/orders?size=10").body(), "\"10/1\"");

    Response response = app.send(Method::Get, "/orders?size=51");
    EXPECT_EQ(response.status(), 400);
    EXPECT_TRUE(contains(response.body(), "Query parameter 'size' must be from 1 to 50"))
        << response.body();

    response = app.send(Method::Get, "/orders?size=10&page=0");
    EXPECT_EQ(response.status(), 400);
    EXPECT_TRUE(contains(response.body(), "Query parameter 'page' must be at least 1"));

    EXPECT_EQ(app.send(Method::Get, "/orders/abc").status(), 200);
    response = app.send(Method::Get, "/orders/abcd");
    EXPECT_EQ(response.status(), 400);
    EXPECT_TRUE(contains(response.body(), "Path parameter 'code' must have exactly 3 characters"))
        << response.body();
}
