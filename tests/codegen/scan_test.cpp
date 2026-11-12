#include <gtest/gtest.h>

#include <cppboot/codegen/scan.hpp>
#include <string>
#include <vector>

using namespace cppboot::codegen;

TEST(ScanTest, SavedScansReadBackTheSame) {
    ScannedHeader scanned;
    scanned.header = "D:/project/src/users.hpp";
    Declarations& declarations = scanned.declarations;
    declarations.components.push_back(
        Component{"app::Users", "users.hpp:4", {"app::Store"}, {"cppboot::config::Config"}});
    declarations.json_types.push_back(
        MappedStruct{"app::User", "users.hpp:9", {{"id", false}, {"nickname", true}}});
    declarations.row_types.push_back(MappedStruct{"app::Row", "users.hpp:12", {{"id", false}}});

    Route route;
    route.method = "Post";
    route.path = "/api/login";
    route.function = "login";
    route.location = "users.hpp:20";
    route.parameters.push_back(RouteParameter{ParameterSource::Body, "credentials", "app::Credentials"});
    route.parameters.push_back(RouteParameter{ParameterSource::Principal, "who", "cppboot::security::Principal"});
    route.return_kind = ReturnKind::Response;
    route.is_public = true;
    route.roles = {"admin", "owner"};
    route.rate_limit = RouteRateLimit{10, 60};
    declarations.controllers.push_back(Controller{"app::Api", {route}});
    declarations.middleware.push_back(Middleware{"app::Log", "users.hpp:30", -5});
    declarations.warnings.push_back("users.hpp:40: warning: something");

    const auto read = scannedHeaderFromJson(toJson(scanned));
    ASSERT_TRUE(read) << read.error().message();
    const Declarations& back = read.value().declarations;

    EXPECT_EQ(read.value().header, scanned.header);
    ASSERT_EQ(back.components.size(), 1U);
    EXPECT_EQ(back.components[0].interfaces, (std::vector<std::string>{"app::Store"}));
    EXPECT_EQ(back.components[0].dependencies,
              (std::vector<std::string>{"cppboot::config::Config"}));
    EXPECT_TRUE(back.json_types[0].fields[1].is_optional);
    EXPECT_EQ(back.row_types[0].type, "app::Row");

    const Route& read_route = back.controllers.at(0).routes.at(0);
    EXPECT_EQ(read_route.path, "/api/login");
    EXPECT_EQ(read_route.parameters.at(0).source, ParameterSource::Body);
    EXPECT_EQ(read_route.parameters.at(1).source, ParameterSource::Principal);
    EXPECT_EQ(read_route.return_kind, ReturnKind::Response);
    EXPECT_TRUE(read_route.is_public);
    EXPECT_EQ(read_route.roles, (std::vector<std::string>{"admin", "owner"}));
    ASSERT_TRUE(read_route.rate_limit.has_value());
    EXPECT_EQ(read_route.rate_limit->requests, 10U);
    EXPECT_EQ(back.middleware.at(0).order, -5);
    EXPECT_EQ(back.warnings, declarations.warnings);

    // The same rendering from either one is what makes the saved scans safe to use.
    EXPECT_EQ(renderRegistration(back, {"a.hpp"}), renderRegistration(declarations, {"a.hpp"}));
}

TEST(ScanTest, ARouteWithNoRateLimitStaysWithout) {
    ScannedHeader scanned;
    scanned.header = "a.hpp";
    Route route;
    route.method = "Get";
    route.path = "/";
    scanned.declarations.controllers.push_back(Controller{"C", {route}});

    const auto read = scannedHeaderFromJson(toJson(scanned));
    ASSERT_TRUE(read);
    EXPECT_FALSE(read.value().declarations.controllers[0].routes[0].rate_limit.has_value());
}

TEST(ScanTest, ADamagedScanIsAnErrorThatSaysWhatToDo) {
    const auto read = scannedHeaderFromJson("{\"header\": 3");
    ASSERT_FALSE(read);
    EXPECT_NE(read.error().message().find("build again"), std::string::npos);
}

TEST(ScanTest, KeepsOnlyArgumentsThatChangeWhatAHeaderDeclares) {
    const std::vector<std::string> arguments = {
        "-IC:/include",  "-I",          "D:/other",      "-DFEATURE=1",  "-D",
        "SECOND",        "-UTHIRD",     "-isystem",      "/usr/x",       "-include",
        "forced.hpp",    "-std=c++20",  "-Wall",         "-Werror",      "-O2",
        "-fsanitize=address",           "-Wno-unused",   "-march=native", "-g",
    };
    EXPECT_EQ(usableCompilerArguments(arguments),
              (std::vector<std::string>{"-IC:/include", "-I", "D:/other", "-DFEATURE=1", "-D",
                                        "SECOND", "-UTHIRD", "-isystem", "/usr/x", "-include",
                                        "forced.hpp", "-std=c++20"}));
}

TEST(ScanTest, ADanglingOptionWithNoValueIsDropped) {
    EXPECT_EQ(usableCompilerArguments({"-DA", "-include"}), (std::vector<std::string>{"-DA"}));
}

TEST(ScanTest, DepfilesEscapeWhatMakeAndNinjaTreatSpecially) {
    const std::string text = makeDepfile("C:/out/a.json", {"C:/my project/a.hpp", "/x/#odd$.hpp"});
    EXPECT_EQ(text, "C:/out/a.json: \\\n  C:/my\\ project/a.hpp \\\n  /x/\\#odd$$.hpp\n");
}

TEST(ScanTest, ChecksAndSettingsStructsReadBackTheSame) {
    ScannedHeader scanned;
    scanned.header = "shop.hpp";
    Declarations& declarations = scanned.declarations;
    declarations.json_types.push_back(MappedStruct{
        "Order",
        "shop.hpp:3",
        {{"product",
          false,
          {{CheckKind::NotEmpty, ValueShape::Text, "", ""},
           {CheckKind::Length, ValueShape::Text, "1", "50"}}},
         {"quantity", false, {{CheckKind::Range, ValueShape::Number, "1", "100"}}}}});
    declarations.config_types.push_back(ConfigStruct{
        "Shop",
        "shop.hpp:9",
        "shop",
        {{"name", true, {{CheckKind::NotEmpty, ValueShape::Text, "", ""}}},
         {"retry.attempts", false, {{CheckKind::Max, ValueShape::Number, "", "10"}}}}});
    declarations.components.push_back(Component{"Api", "shop.hpp:20", {}, {"Shop"}});

    Route route;
    route.method = "Get";
    route.path = "/orders";
    route.function = "list";
    route.parameters.push_back(RouteParameter{
        ParameterSource::Query, "size", "int", {{CheckKind::Min, ValueShape::Number, "1", ""}}});
    declarations.controllers.push_back(Controller{"Api", {route}});

    const auto read = scannedHeaderFromJson(toJson(scanned));
    ASSERT_TRUE(read) << read.error().message();
    const Declarations& back = read.value().declarations;

    ASSERT_EQ(back.config_types.size(), 1U);
    EXPECT_EQ(back.config_types[0].prefix, "shop");
    EXPECT_TRUE(back.config_types[0].fields.at(0).required);
    EXPECT_EQ(back.config_types[0].fields.at(1).path, "retry.attempts");
    EXPECT_EQ(back.config_types[0].fields.at(1).checks.at(0).max, "10");
    EXPECT_EQ(back.json_types.at(0).fields.at(0).checks.size(), 2U);
    EXPECT_EQ(back.json_types.at(0).fields.at(0).checks.at(1).shape, ValueShape::Text);
    EXPECT_EQ(back.controllers.at(0).routes.at(0).parameters.at(0).checks.at(0).kind, CheckKind::Min);

    EXPECT_EQ(renderRegistration(back, {"shop.hpp"}), renderRegistration(declarations, {"shop.hpp"}));
}

TEST(ScanTest, RepositoriesReadBackTheSame) {
    ScannedHeader scanned;
    scanned.header = "books.hpp";
    Declarations& declarations = scanned.declarations;
    declarations.row_types.push_back(MappedStruct{
        "Book", "books.hpp:3", {{"id", false, {}, ValueKind::Number}, {"title", false, {}, ValueKind::Text}}});

    RepositoryFunction find;
    find.name = "findByTitle";
    find.location = "books.hpp:9";
    find.return_type = "std::vector<Book>";
    find.returns = RepositoryReturn::List;
    find.element_type = "Book";
    find.parameters = {{"title", "const std::string &", ValueKind::Text}};
    RepositoryFunction cheap;
    cheap.name = "cheapest";
    cheap.location = "books.hpp:11";
    cheap.return_type = "std::optional<double>";
    cheap.returns = RepositoryReturn::Optional;
    cheap.element_type = "double";
    cheap.element_kind = ValueKind::Number;
    cheap.is_const = true;
    cheap.query = "SELECT MIN(price) FROM books";
    declarations.repositories.push_back(
        RepositoryInterface{"Books", "books.hpp:7", "books", "Book", {find, cheap}});

    const auto read = scannedHeaderFromJson(toJson(scanned));
    ASSERT_TRUE(read) << read.error().message();
    const Declarations& back = read.value().declarations;
    ASSERT_EQ(back.repositories.size(), 1U);
    EXPECT_EQ(back.repositories[0].table, "books");
    EXPECT_EQ(back.repositories[0].functions.at(0).parameters.at(0).kind, ValueKind::Text);
    EXPECT_EQ(back.repositories[0].functions.at(1).query, "SELECT MIN(price) FROM books");
    EXPECT_TRUE(back.repositories[0].functions.at(1).is_const);
    EXPECT_EQ(back.row_types.at(0).fields.at(1).kind, ValueKind::Text);

    EXPECT_EQ(renderRegistration(back, {"books.hpp"}), renderRegistration(declarations, {"books.hpp"}));
}
