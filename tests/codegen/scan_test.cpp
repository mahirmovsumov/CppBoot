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
