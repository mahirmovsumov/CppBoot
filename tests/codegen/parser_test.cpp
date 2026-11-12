#include <gtest/gtest.h>

#include <algorithm>
#include <cppboot/codegen/parser.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cppboot::codegen;
using cppboot::ErrorCode;

namespace {

constexpr const char* kPreamble = R"(
#include <memory>
#include <optional>
#include <string>
#include <vector>
#define SERVICE [[clang::annotate("cppboot:service")]]
#define JSON [[clang::annotate("cppboot:json")]]
#define ROW [[clang::annotate("cppboot:row")]]
#define CONTROLLER(path) [[clang::annotate("cppboot:controller:" path)]]
#define GET(path) [[clang::annotate("cppboot:route:Get:" path)]]
#define POST(path) [[clang::annotate("cppboot:route:Post:" path)]]
#define PUBLIC_ROUTE [[clang::annotate("cppboot:public")]]
#define ROLE(role) [[clang::annotate("cppboot:role:" role)]]
#define MIDDLEWARE(order) [[clang::annotate("cppboot:middleware:" #order)]]
#define RATE_LIMIT(limit) [[clang::annotate("cppboot:rate_limit:" limit)]]
#define UNCHECKED(reason) [[clang::annotate("cppboot:unchecked:" reason)]]
#define CONFIG(prefix) [[clang::annotate("cppboot:config:" prefix)]]
#define REQUIRED [[clang::annotate("cppboot:required")]]
#define NOT_EMPTY [[clang::annotate("cppboot:not_empty")]]
#define LENGTH(min, max) [[clang::annotate("cppboot:length:" #min ":" #max)]]
#define MIN(min) [[clang::annotate("cppboot:min:" #min)]]
#define MAX(max) [[clang::annotate("cppboot:max:" #max)]]
#define RANGE(min, max) [[clang::annotate("cppboot:range:" #min ":" #max)]]
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string_view>
namespace cppboot::web {
class Request {};
class Response {};
}
namespace cppboot {
class Module { public: virtual ~Module() = default; virtual void start() = 0; };
}
namespace cppboot::security {
struct Principal {};
}
)";

}  // namespace

class ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-parser-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    auto parse(const std::string& source, const std::string& name = "input.hpp") const {
        const auto path = directory_ / name;
        std::ofstream{path, std::ios::binary} << kPreamble << source;
        return parseDeclarations(path, {"-I" + directory_.string()});
    }

    /// Returns the warnings for this source, which must parse.
    std::string warnings(const std::string& source) const {
        const auto result = parse(source);
        EXPECT_TRUE(result) << (result ? "" : result.error().message());
        std::string text;
        if (result) {
            for (const std::string& warning : result.value().warnings) {
                text += warning + "\n";
            }
        }
        return text;
    }

    void expectError(const std::string& source, const std::string& expected) const {
        const auto result = parse(source);
        ASSERT_FALSE(result) << source;
        EXPECT_NE(result.error().message().find(expected), std::string::npos)
            << result.error().message();
    }

    Route parseOneRoute(const std::string& function, const std::string& base = "/users") const {
        const auto result =
            parse("class CONTROLLER(\"" + base + "\") C { public: " + function + " };");
        EXPECT_TRUE(result) << (result ? "" : result.error().message());
        if (!result || result.value().controllers.empty() ||
            result.value().controllers[0].routes.empty()) {
            return Route{};
        }
        return result.value().controllers[0].routes[0];
    }

    std::filesystem::path directory_;
};

TEST_F(ParserTest, ReadsQualifiedNamesAndDependencies) {
    const auto result = parse(R"(
        namespace app {
        struct Repo { virtual ~Repo() = default; virtual void save() = 0; };
        namespace inner {
        class SERVICE SqlRepo : public Repo { public: void save() override {} };
        }
        class SERVICE Service {
        public:
            Service(std::shared_ptr<Repo> a, const std::shared_ptr<const inner::SqlRepo>& b);
        };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    const auto& components = result.value().components;
    ASSERT_EQ(components.size(), 2U);

    EXPECT_EQ(components[0].type, "app::inner::SqlRepo");
    EXPECT_EQ(components[0].interfaces, std::vector<std::string>{"app::Repo"});
    EXPECT_TRUE(components[0].dependencies.empty());

    EXPECT_EQ(components[1].type, "app::Service");
    EXPECT_EQ(components[1].dependencies,
              (std::vector<std::string>{"app::Repo", "app::inner::SqlRepo"}));
}

TEST_F(ParserTest, SeesThroughTypeAliases) {
    const auto result = parse(R"(
        namespace app {
        class SERVICE Repo {};
        using RepoPointer = std::shared_ptr<Repo>;
        class SERVICE Service { public: explicit Service(RepoPointer repo); };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value().components[1].dependencies, std::vector<std::string>{"app::Repo"});
}

TEST_F(ParserTest, IgnoresClassesThatAreNotMarked) {
    const auto result = parse("class Plain {}; class SERVICE Marked {};");
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().components.size(), 1U);
    EXPECT_EQ(result.value().components[0].type, "Marked");
}

TEST_F(ParserTest, SkipsDeclarationsInIncludedFiles) {
    std::ofstream{directory_ / "other.hpp"} << "class SERVICE Included {};\n";

    const auto result = parse("#include \"other.hpp\"\nclass SERVICE Local {};");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().components.size(), 1U);
    EXPECT_EQ(result.value().components[0].type, "Local");
}

TEST_F(ParserTest, IgnoresCopyAndMoveConstructors) {
    const auto result = parse(R"(
        class SERVICE Copyable {
        public:
            Copyable();
            Copyable(const Copyable&);
            Copyable(Copyable&&);
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_TRUE(result.value().components[0].dependencies.empty());
}

TEST_F(ParserTest, LocationNamesTheFileAndLine) {
    const auto result = parse("\nclass SERVICE Marked {};");
    ASSERT_TRUE(result);
    EXPECT_NE(result.value().components[0].location.find("input.hpp:"), std::string::npos);
}

TEST_F(ParserTest, RejectsAbstractComponents) {
    expectError("class SERVICE Pure { public: virtual void f() = 0; };", "abstract");
}

TEST_F(ParserTest, RejectsMultiplePublicConstructors) {
    expectError(R"(
        class SERVICE Two { public: Two(); explicit Two(std::shared_ptr<int> value); };
    )",
                "more than one public constructor");
}

TEST_F(ParserTest, RejectsPrivateConstructors) {
    expectError("class SERVICE Hidden { Hidden(); };", "no public constructor");
}

TEST_F(ParserTest, RejectsParametersThatAreNotSharedPointers) {
    expectError("class SERVICE Takes { public: explicit Takes(int size); };",
                "Constructor parameter 'size' of Takes");
}

TEST_F(ParserTest, RejectsTemplates) {
    expectError("template <typename T> class SERVICE Box {};", "template");
}

TEST_F(ParserTest, RejectsAnonymousNamespaces) {
    expectError("namespace { class SERVICE Hidden {}; }", "can't name it");
}

TEST_F(ParserTest, ReportsCompileErrors) {
    const auto result = parse("class SERVICE Broken { UnknownType value; };");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
    EXPECT_NE(result.error().message().find("UnknownType"), std::string::npos);
}

TEST_F(ParserTest, ReportsAMissingFile) {
    const auto result = parseDeclarations(directory_ / "absent.hpp", {});
    EXPECT_FALSE(result);
}

TEST_F(ParserTest, ReadsJsonFields) {
    const auto result = parse(R"(
        namespace app {
        struct JSON User {
            int id;
            std::string name;
            std::optional<std::string> nickname;
            std::vector<int> scores;
            static constexpr int kVersion = 1;
        };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().json_types.size(), 1U);

    const MappedStruct& type = result.value().json_types[0];
    EXPECT_EQ(type.type, "app::User");
    ASSERT_EQ(type.fields.size(), 4U);
    EXPECT_EQ(type.fields[0].name, "id");
    EXPECT_FALSE(type.fields[0].is_optional);
    EXPECT_EQ(type.fields[2].name, "nickname");
    EXPECT_TRUE(type.fields[2].is_optional);
}

TEST_F(ParserTest, ReadsRowFieldsAndAllowsBothMarks) {
    const auto result = parse(R"(
        struct JSON ROW User { long id; std::optional<std::string> nickname; };
        struct ROW OnlyRow { int value; };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value().json_types.size(), 1U);
    ASSERT_EQ(result.value().row_types.size(), 2U);
    EXPECT_EQ(result.value().row_types[0].type, "User");
    EXPECT_EQ(result.value().row_types[0].fields[0].name, "id");
    EXPECT_EQ(result.value().row_types[1].type, "OnlyRow");
}

TEST_F(ParserTest, RejectsPrivateRowFields) {
    expectError("class ROW Hidden { int secret; };",
                "Field 'secret' of Hidden must be public for CPPBOOT_ROW");
}

TEST_F(ParserTest, RejectsPrivateJsonFields) {
    expectError("class JSON Hidden { int secret; };", "Field 'secret' of Hidden must be public");
}

TEST_F(ParserTest, RejectsJsonBaseClasses) {
    expectError("struct Base { int a; }; struct JSON Derived : Base { int b; };", "base classes");
}

TEST_F(ParserTest, BindsPathQueryBodyAndRequestParameters) {
    const Route route = parseOneRoute(R"(
        struct Body {};
        GET("/{id}") Body get(int id, const std::string& filter, std::optional<int> page,
                              const Body& body, const cppboot::web::Request& request);
    )");

    EXPECT_EQ(route.method, "Get");
    EXPECT_EQ(route.path, "/users/{id}");
    EXPECT_EQ(route.function, "get");
    EXPECT_EQ(route.return_kind, ReturnKind::Value);
    ASSERT_EQ(route.parameters.size(), 5U);

    EXPECT_EQ(route.parameters[0].source, ParameterSource::Path);
    EXPECT_EQ(route.parameters[0].type, "int");
    EXPECT_EQ(route.parameters[1].source, ParameterSource::Query);
    EXPECT_EQ(route.parameters[1].name, "filter");
    EXPECT_EQ(route.parameters[2].source, ParameterSource::Query);
    EXPECT_EQ(route.parameters[3].source, ParameterSource::Body);
    EXPECT_EQ(route.parameters[3].type, "C::Body");
    EXPECT_EQ(route.parameters[4].source, ParameterSource::Request);
}

TEST_F(ParserTest, WorksOutRouteReturnKinds) {
    EXPECT_EQ(parseOneRoute("POST(\"/\") void a();").return_kind, ReturnKind::Nothing);
    EXPECT_EQ(parseOneRoute("GET(\"/\") cppboot::web::Response a();").return_kind,
              ReturnKind::Response);
    EXPECT_EQ(parseOneRoute("GET(\"/\") std::vector<int> a();").return_kind, ReturnKind::Value);
}

TEST_F(ParserTest, JoinsControllerAndRoutePaths) {
    EXPECT_EQ(parseOneRoute("GET(\"/\") int a();", "/users").path, "/users");
    EXPECT_EQ(parseOneRoute("GET(\"/x\") int a();", "/users/").path, "/users/x");
    EXPECT_EQ(parseOneRoute("GET(\"/\") int a();", "/").path, "/");
    EXPECT_EQ(parseOneRoute("GET(\"/x\") int a();", "").path, "/x");
}

TEST_F(ParserTest, AllowsTwoRoutesOnOneFunction) {
    const auto result = parse(R"(
        class CONTROLLER("/a") C { public: GET("/one") GET("/two") int f(); };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value().controllers[0].routes.size(), 2U);
}

TEST_F(ParserTest, ControllerIsAlsoAComponentWithDependencies) {
    const auto result = parse(R"(
        class SERVICE Service {};
        class CONTROLLER("/a") C {
        public:
            explicit C(std::shared_ptr<Service> service);
            GET("/") int f();
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().components.size(), 2U);
    EXPECT_EQ(result.value().components[1].dependencies, std::vector<std::string>{"Service"});
}

TEST_F(ParserTest, RejectsAPathParameterWithNoFunctionParameter) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/{id}") int f(); };)",
                "no parameter with that name");
}

TEST_F(ParserTest, RejectsAnOptionalPathParameter) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/{id}") int f(std::optional<int> id); };)",
                "Path parameter 'id'");
}

TEST_F(ParserTest, RejectsTwoBodyParameters) {
    expectError(R"(
        struct B {};
        class CONTROLLER("/a") C { public: POST("/") int f(const B& one, const B& two); };
    )",
                "only read one parameter");
}

TEST_F(ParserTest, RejectsANonConstReferenceParameter) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") int f(int& value); };)",
                "non-const reference");
}

TEST_F(ParserTest, RejectsAPrivateRouteFunction) {
    expectError(R"(class CONTROLLER("/a") C { GET("/") int f(); };)", "must be public");
}

TEST_F(ParserTest, RejectsAStaticRouteFunction) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") static int f(); };)",
                "can't be static");
}

TEST_F(ParserTest, RejectsAControllerWithNoRoutes) {
    expectError(R"(class CONTROLLER("/a") C { public: int f(); };)", "has no routes");
}

TEST_F(ParserTest, RejectsARouteOutsideAController) {
    expectError(R"(class SERVICE C { public: GET("/") int f(); };)",
                "must be in a class marked with CPPBOOT_CONTROLLER");
}

TEST_F(ParserTest, ReadsDependenciesTakenByReference) {
    const auto result = parse(R"(
        struct Inventory {};
        struct Payment {};
        class SERVICE Orders {
        public:
            Orders(Inventory& inventory, const Payment& payment) : i_(inventory), p_(payment) {}
        private:
            Inventory& i_;
            const Payment& p_;
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value().components[0].dependencies,
              (std::vector<std::string>{"Inventory", "Payment"}));
}

TEST_F(ParserTest, RejectsDependenciesTakenAnyOtherWay) {
    const std::string message = "must be a reference or a std::shared_ptr";
    expectError("struct D {}; class SERVICE S { public: explicit S(D d); };", message);
    expectError("struct D {}; class SERVICE S { public: explicit S(D* d); };", message);
    expectError("struct D {}; class SERVICE S { public: explicit S(D&& d); };", message);
    expectError("class SERVICE S { public: explicit S(int& value); };", message);
    expectError("class SERVICE S { public: explicit S(int value); };", message);
}

TEST_F(ParserTest, RejectsAComponentThatPointsAtARequest) {
    const std::string message = "ends with its request";
    expectError(R"(class SERVICE S { const cppboot::web::Request* last_{nullptr}; };)", message);
    expectError(R"(class SERVICE S {
        public: explicit S(cppboot::web::Response& r) : r_(r) {}
        cppboot::web::Response& r_; };)", "ends with its request");
    expectError(R"(class CONTROLLER("/a") C {
        public: GET("/") int f();
        cppboot::security::Principal* who_{nullptr}; };)", message);
}

TEST_F(ParserTest, UncheckedAllowsARiskyFieldButNeedsAReason) {
    EXPECT_TRUE(parse(R"(class SERVICE S {
        UNCHECKED("Cleared before the request ends") const cppboot::web::Request* last_{nullptr};
    };)"));
    expectError(R"(class SERVICE S { UNCHECKED("") const cppboot::web::Request* last_{nullptr}; };)",
                "needs a reason");
}

TEST_F(ParserTest, OutsideAComponentARequestPointerIsFine) {
    EXPECT_TRUE(parse("struct Helper { const cppboot::web::Request* request; };"));
}

TEST_F(ParserTest, WarnsAboutViewsStoredInAComponent) {
    EXPECT_NE(warnings("class SERVICE S { std::string_view name_; };").find("name_"),
              std::string::npos);
    EXPECT_EQ(warnings(R"(class SERVICE S { UNCHECKED("A literal") std::string_view name_; };)"), "");
    EXPECT_EQ(warnings("class SERVICE S { std::string name_; };"), "");
}

TEST_F(ParserTest, WarnsAboutRoutesThatReturnAView) {
    EXPECT_NE(warnings(R"(class CONTROLLER("/a") C { public: GET("/") std::string_view f(); };)")
                  .find("returns a view"),
              std::string::npos);
    EXPECT_EQ(warnings(R"(class CONTROLLER("/a") C { public: GET("/") std::string f(); };)"), "");
}

TEST_F(ParserTest, WarnsAboutACopyOfADependency) {
    EXPECT_NE(warnings(R"(struct D {};
        class SERVICE S { public: explicit S(D& d) : copy_(d) {} private: D copy_; };)")
                  .find("is a copy of D"),
              std::string::npos);
    EXPECT_EQ(warnings(R"(struct D {};
        class SERVICE S { public: explicit S(D& d) : d_(d) {} private: D& d_; };)"), "");
}

TEST_F(ParserTest, WarnsAboutStateThatThreadsCanChangeWithNoLock) {
    const std::string message = "can change, with no lock";
    EXPECT_NE(warnings(R"(class SERVICE S {
        public: void add(int v) { v_.push_back(v); }
        private: std::vector<int> v_; };)").find(message), std::string::npos);
    EXPECT_NE(warnings(R"(class SERVICE S {
        public: void hit() { ++count_; }
        private: int count_{0}; };)").find(message), std::string::npos);
    EXPECT_NE(warnings(R"(class SERVICE S {
        public: int get() const { return ++count_; }
        private: mutable int count_{0}; };)").find(message), std::string::npos);
    EXPECT_NE(warnings(R"(class CONTROLLER("/a") C {
        public: GET("/") int f() { return ++n_; }
        private: int n_{0}; };)").find(message), std::string::npos);
}

TEST_F(ParserTest, DoesNotWarnAboutStateThatLooksSafe) {
    // A lock in the class.
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: void add(int v);
        private: std::mutex mutex_; std::vector<int> v_; };)"), "");
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: void add(int v);
        private: mutable std::shared_mutex mutex_; std::map<int, int> v_; };)"), "");
    // Atomics protect themselves.
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: void hit();
        private: std::atomic<int> count_{0}; };)"), "");
    // Const functions can't change it, and neither can anything else.
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: int get() const { return value_; }
        private: int value_{0}; };)"), "");
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: void hit();
        private: const int limit_{0}; };)"), "");
    // A module's start and stop run before and after requests.
    EXPECT_EQ(warnings(R"(class SERVICE S : public cppboot::Module {
        public: void start() override { port_ = 1; }
        private: int port_{0}; };)"), "");
    // An explanation.
    EXPECT_EQ(warnings(R"(class SERVICE S {
        public: void add(int v);
        private: UNCHECKED("Only called during startup") std::vector<int> v_; };)"), "");
    // A class that isn't a component isn't shared.
    EXPECT_EQ(warnings(R"(class Plain {
        public: void add(int v);
        private: std::vector<int> v_; };)"), "");
}

TEST_F(ParserTest, ListsEveryFileAHeaderUses) {
    const auto path = directory_ / "uses.hpp";
    std::ofstream{directory_ / "helper.hpp"} << "#pragma once\nstruct Helper {};\n";
    std::ofstream{path, std::ios::binary} << kPreamble << "#include \"helper.hpp\"\nclass SERVICE S {};\n";

    std::vector<std::filesystem::path> files;
    const auto result = parseDeclarations(path, {"-I" + directory_.string()}, &files);
    ASSERT_TRUE(result) << result.error().message();

    const auto has = [&](const std::string& name) {
        return std::any_of(files.begin(), files.end(), [&](const std::filesystem::path& file) {
            return file.filename() == name;
        });
    };
    EXPECT_TRUE(has("uses.hpp"));
    EXPECT_TRUE(has("helper.hpp"));
    EXPECT_TRUE(has("memory"));
}

TEST_F(ParserTest, DefinitionsChangeWhatIsFound) {
    const std::string source = "#ifdef WITH_EXTRA\nclass SERVICE Extra {};\n#endif\nclass SERVICE Always {};";
    const auto path = directory_ / "conditional.hpp";
    std::ofstream{path, std::ios::binary} << kPreamble << source;

    const auto without = parseDeclarations(path, {});
    ASSERT_TRUE(without);
    EXPECT_EQ(without.value().components.size(), 1U);

    const auto with = parseDeclarations(path, {"-DWITH_EXTRA"});
    ASSERT_TRUE(with);
    EXPECT_EQ(with.value().components.size(), 2U);
}

TEST_F(ParserTest, ReadsRateLimits) {
    const Route route = parseOneRoute(R"(RATE_LIMIT("5/minute") GET("/") int f();)");
    ASSERT_TRUE(route.rate_limit.has_value());
    EXPECT_EQ(route.rate_limit->requests, 5U);
    EXPECT_EQ(route.rate_limit->window_seconds, 60U);

    EXPECT_EQ(parseOneRoute(R"(RATE_LIMIT("1/second") GET("/") int f();)").rate_limit->window_seconds,
              1U);
    EXPECT_EQ(parseOneRoute(R"(RATE_LIMIT("1/hour") GET("/") int f();)").rate_limit->window_seconds,
              3600U);
    EXPECT_FALSE(parseOneRoute(R"(GET("/") int f();)").rate_limit.has_value());
}

TEST_F(ParserTest, FunctionRateLimitReplacesControllerRateLimit) {
    const auto result = parse(R"(
        class CONTROLLER("/a") RATE_LIMIT("5/minute") C {
        public:
            GET("/x") int x();
            RATE_LIMIT("1/hour") GET("/y") int y();
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    const auto& routes = result.value().controllers[0].routes;
    EXPECT_EQ(routes[0].rate_limit->requests, 5U);
    EXPECT_EQ(routes[1].rate_limit->window_seconds, 3600U);
}

TEST_F(ParserTest, RejectsRateLimitsThatCannotBeRead) {
    for (const char* limit : {"5", "/minute", "0/minute", "-1/minute", "5/day", "5/Minute",
                              "5 /minute", "5/minute ", "1000001/second", ""}) {
        expectError(std::string{"class CONTROLLER(\"/a\") C { public: RATE_LIMIT(\""} + limit +
                        "\") GET(\"/\") int f(); };",
                    "must look like");
    }
}

TEST_F(ParserTest, RejectsTwoRateLimitsOnOneRoute) {
    expectError(R"(class CONTROLLER("/a") C {
        public: RATE_LIMIT("1/minute") RATE_LIMIT("2/minute") GET("/") int f(); };)",
                "more than one rate limit");
}

TEST_F(ParserTest, RejectsRateLimitsThatWouldDoNothing) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") int f();
                                                       RATE_LIMIT("1/minute") int g(); };)",
                "isn't a route");
    expectError(R"(class SERVICE RATE_LIMIT("1/minute") S {};)", "must be on a class marked");
    expectError(R"(class S { public: RATE_LIMIT("1/minute") int f(); };)",
                "must be in a class marked");
}

TEST_F(ParserTest, RoutesAreNotPublicUnlessMarked) {
    const Route route = parseOneRoute(R"(GET("/") int f();)");
    EXPECT_FALSE(route.is_public);
    EXPECT_TRUE(route.roles.empty());
}

TEST_F(ParserTest, ReadsSecurityMarksOnAFunction) {
    EXPECT_TRUE(parseOneRoute(R"(PUBLIC_ROUTE GET("/") int f();)").is_public);
    EXPECT_EQ(parseOneRoute(R"(ROLE("admin") ROLE("editor") GET("/") int f();)").roles,
              (std::vector<std::string>{"admin", "editor"}));
}

TEST_F(ParserTest, ControllerMarksApplyToItsRoutes) {
    const auto roles = parse(R"(
        class CONTROLLER("/a") ROLE("admin") C { public: GET("/") int f(); };
    )");
    ASSERT_TRUE(roles) << roles.error().message();
    EXPECT_EQ(roles.value().controllers[0].routes[0].roles, std::vector<std::string>{"admin"});

    const auto open = parse(R"(
        class CONTROLLER("/a") PUBLIC_ROUTE C { public: GET("/") int f(); };
    )");
    ASSERT_TRUE(open) << open.error().message();
    EXPECT_TRUE(open.value().controllers[0].routes[0].is_public);
}

TEST_F(ParserTest, FunctionMarksReplaceControllerMarks) {
    const auto result = parse(R"(
        class CONTROLLER("/a") ROLE("admin") C {
        public:
            PUBLIC_ROUTE GET("/open") int open();
            ROLE("editor") GET("/edit") int edit();
        };
        class CONTROLLER("/b") PUBLIC_ROUTE D {
        public:
            ROLE("admin") GET("/secret") int secret();
        };
    )");
    ASSERT_TRUE(result) << result.error().message();

    const auto& first = result.value().controllers[0].routes;
    EXPECT_TRUE(first[0].is_public);
    EXPECT_TRUE(first[0].roles.empty());
    EXPECT_EQ(first[1].roles, std::vector<std::string>{"editor"});

    // A role on a function inside a public controller must still protect that function.
    const Route& secret = result.value().controllers[1].routes[0];
    EXPECT_FALSE(secret.is_public);
    EXPECT_EQ(secret.roles, std::vector<std::string>{"admin"});
}

TEST_F(ParserTest, BindsThePrincipal) {
    const Route route =
        parseOneRoute(R"(GET("/") int f(const cppboot::security::Principal& who);)");
    ASSERT_EQ(route.parameters.size(), 1U);
    EXPECT_EQ(route.parameters[0].source, ParameterSource::Principal);
}

TEST_F(ParserTest, RejectsAPrincipalOnAPublicRoute) {
    expectError(R"(class CONTROLLER("/a") C { public:
        PUBLIC_ROUTE GET("/") int f(const cppboot::security::Principal& who); };)",
                "can't take a Principal");
}

TEST_F(ParserTest, RejectsBothPublicAndRole) {
    expectError(R"(class CONTROLLER("/a") C { public: PUBLIC_ROUTE ROLE("x") GET("/") int f(); };)",
                "both CPPBOOT_PUBLIC and CPPBOOT_ROLE");
    expectError(R"(class CONTROLLER("/a") PUBLIC_ROUTE ROLE("x") C { public: GET("/") int f(); };)",
                "both CPPBOOT_PUBLIC and CPPBOOT_ROLE");
}

TEST_F(ParserTest, RejectsASecurityMarkThatWouldDoNothing) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") int f(); ROLE("x") int g(); };)",
                "isn't a route");
    expectError(R"(class SERVICE C { public: ROLE("x") int g(); };)",
                "must be in a class marked with CPPBOOT_CONTROLLER");
    expectError(R"(class SERVICE ROLE("x") C {};)",
                "must be on a class marked with CPPBOOT_CONTROLLER");
}

TEST_F(ParserTest, RejectsAnEmptyRole) {
    expectError(R"(class CONTROLLER("/a") C { public: ROLE("") GET("/") int f(); };)",
                "empty role");
}

TEST_F(ParserTest, ReadsMiddlewareAsAComponent) {
    const auto result = parse(R"(
        class SERVICE Log {};
        class MIDDLEWARE(20) Audit {
        public:
            explicit Audit(std::shared_ptr<Log> log);
            int handle(int request, int next);
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().middleware.size(), 1U);
    EXPECT_EQ(result.value().middleware[0].type, "Audit");
    EXPECT_EQ(result.value().middleware[0].order, 20);
    EXPECT_EQ(result.value().components[1].dependencies, std::vector<std::string>{"Log"});
}

TEST_F(ParserTest, RejectsMiddlewareWithoutAHandleFunction) {
    expectError("class MIDDLEWARE(1) Empty {};", "public function named handle");
    expectError("class MIDDLEWARE(1) Hidden { int handle(int a, int b); };",
                "public function named handle");
}

TEST_F(ParserTest, RejectsMiddlewareWithABadOrder) {
    expectError("class MIDDLEWARE(early) Bad { public: int handle(int a, int b); };",
                "must be a whole number");
}

TEST_F(ParserTest, RejectsARoutePathWithoutASlash) {
    expectError(R"(class CONTROLLER("/a") C { public: GET("x") int f(); };)", "must start with '/'");
}

TEST_F(ParserTest, RejectsABasePathWithoutASlash) {
    expectError(R"(class CONTROLLER("a") C { public: GET("/") int f(); };)", "must start with '/'");
}

TEST_F(ParserTest, ReadsChecksOnJsonFields) {
    const auto result = parse(R"(
        struct JSON Order {
            NOT_EMPTY LENGTH(1, 50) std::string product;
            RANGE(1, 100) int quantity;
            MIN(0) std::optional<double> discount;
            MAX(-0.5) float change;
            LENGTH(0, 3) std::vector<int> tags;
            std::string note;
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    const std::vector<StructField>& fields = result.value().json_types.at(0).fields;

    ASSERT_EQ(fields[0].checks.size(), 2U);
    EXPECT_EQ(fields[0].checks[0].kind, CheckKind::NotEmpty);
    EXPECT_EQ(fields[0].checks[1].kind, CheckKind::Length);
    EXPECT_EQ(fields[0].checks[1].shape, ValueShape::Text);
    EXPECT_EQ(fields[0].checks[1].min, "1");
    EXPECT_EQ(fields[0].checks[1].max, "50");

    ASSERT_EQ(fields[1].checks.size(), 1U);
    EXPECT_EQ(fields[1].checks[0].kind, CheckKind::Range);
    EXPECT_EQ(fields[1].checks[0].shape, ValueShape::Number);

    // A std::optional is checked as the value inside it.
    EXPECT_EQ(fields[2].checks.at(0).kind, CheckKind::Min);
    EXPECT_EQ(fields[2].checks.at(0).min, "0");
    EXPECT_EQ(fields[3].checks.at(0).max, "-0.5");
    EXPECT_EQ(fields[4].checks.at(0).shape, ValueShape::Items);
    EXPECT_TRUE(fields[5].checks.empty());
}

TEST_F(ParserTest, RejectsChecksThatDoNotFitTheField) {
    expectError("struct JSON A { RANGE(1, 5) std::string name; };",
                "Field 'name' of A isn't a number, so CPPBOOT_RANGE doesn't apply");
    expectError("struct JSON A { MIN(1) std::vector<int> items; };", "isn't a number");
    expectError("struct JSON A { NOT_EMPTY int count; };",
                "is a number, so CPPBOOT_NOT_EMPTY doesn't apply");
    expectError("struct JSON A { LENGTH(1, 5) double rate; };",
                "is a number, so CPPBOOT_LENGTH doesn't apply");
    expectError("struct JSON A { NOT_EMPTY bool flag; };", "can't be checked");
    expectError("struct B { int x; }; struct JSON A { NOT_EMPTY B inner; };", "can't be checked");
}

TEST_F(ParserTest, RejectsLimitsThatAreNotNumbersWrittenOut) {
    expectError("const int kMost = 5; struct JSON A { RANGE(1, kMost) int n; };",
                "CPPBOOT_RANGE on Field 'n' of A needs numbers written out");
    expectError("struct JSON A { RANGE(10, 1) int n; };", "the smaller first");
    expectError("struct JSON A { MIN(1 + 1) int n; };", "needs numbers written out");
    expectError("struct JSON A { MAX(99999999999999999999) int n; };", "needs numbers written out");
    expectError("struct JSON A { LENGTH(-1, 5) std::string s; };", "needs two whole numbers");
    expectError("struct JSON A { LENGTH(1, 2.5) std::string s; };", "needs two whole numbers");
    expectError("struct JSON A { LENGTH(5, 1) std::string s; };", "the smaller first");
}

TEST_F(ParserTest, RejectsRepeatedOrOverlappingChecks) {
    expectError("struct JSON A { MIN(1) MIN(2) int n; };", "the same check more than once");
    expectError("struct JSON A { RANGE(1, 5) MAX(4) int n; };",
                "CPPBOOT_RANGE as well as CPPBOOT_MIN or CPPBOOT_MAX");
}

TEST_F(ParserTest, RejectsChecksThatNothingWouldRun) {
    expectError("struct ROW Row { RANGE(1, 5) int n; };",
                "checks only run when a struct is read from JSON or from settings");
    expectError("struct JSON A { REQUIRED std::string name; };", "already required");

    // On a struct that's also read from JSON, they're fine.
    const auto both = parse("struct JSON ROW Both { RANGE(1, 5) int n; };");
    ASSERT_TRUE(both) << both.error().message();
    EXPECT_EQ(both.value().json_types.at(0).fields.at(0).checks.size(), 1U);
}

TEST_F(ParserTest, ReadsChecksOnPathAndQueryParameters) {
    const Route route = parseOneRoute(
        R"(GET("/{code}") int f(LENGTH(3, 3) std::string code, RANGE(1, 50) int size,
                                MIN(1) std::optional<int> page, int plain);)");
    ASSERT_EQ(route.parameters.size(), 4U);
    EXPECT_EQ(route.parameters[0].source, ParameterSource::Path);
    EXPECT_EQ(route.parameters[0].checks.at(0).kind, CheckKind::Length);
    EXPECT_EQ(route.parameters[1].checks.at(0).kind, CheckKind::Range);
    EXPECT_EQ(route.parameters[2].checks.at(0).kind, CheckKind::Min);
    EXPECT_TRUE(route.parameters[3].checks.empty());
}

TEST_F(ParserTest, RejectsChecksOnOtherParameters) {
    expectError(R"(struct JSON Body { int n; };
                   class CONTROLLER("/a") C { public: POST("/") int f(NOT_EMPTY const Body& body); };)",
                "only path and query parameters can have one");
    expectError(R"(class CONTROLLER("/a") C { public:
                       GET("/") int f(NOT_EMPTY const cppboot::web::Request& request); };)",
                "only path and query parameters can have one");
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") int f(REQUIRED int n); };)",
                "CPPBOOT_REQUIRED, which is for settings");
    expectError(R"(class CONTROLLER("/a") C { public: GET("/") int f(NOT_EMPTY int n); };)",
                "Query parameter 'n' is a number");
}

TEST_F(ParserTest, ReadsASettingsStructWithNestedStructs) {
    const auto result = parse(R"(
        #include <chrono>
        #include <cstdint>
        namespace app {
        struct Retry {
            RANGE(1, 10) int attempts{3};
            std::chrono::milliseconds delay{200};
        };
        struct Limits { Retry retry; std::uint16_t port{80}; };
        struct CONFIG("shop.mail") MailSettings {
            REQUIRED NOT_EMPTY std::string host;
            bool secure{true};
            double rate{0.5};
            std::optional<std::string> user;
            std::vector<std::string> copies;
            std::chrono::seconds timeout{30};
            Limits limits;
            static constexpr int kVersion = 1;
        };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().config_types.size(), 1U);

    const ConfigStruct& settings = result.value().config_types[0];
    EXPECT_EQ(settings.type, "app::MailSettings");
    EXPECT_EQ(settings.prefix, "shop.mail");

    std::vector<std::string> paths;
    for (const ConfigField& field : settings.fields) {
        paths.push_back(field.path);
    }
    EXPECT_EQ(paths, (std::vector<std::string>{"host", "secure", "rate", "user", "copies", "timeout",
                                               "limits.retry.attempts", "limits.retry.delay",
                                               "limits.port"}));
    EXPECT_TRUE(settings.fields[0].required);
    EXPECT_EQ(settings.fields[0].checks.at(0).kind, CheckKind::NotEmpty);
    EXPECT_FALSE(settings.fields[1].required);
    EXPECT_EQ(settings.fields[6].checks.at(0).kind, CheckKind::Range);

    // It has no constructor to read, so it's listed only as settings.
    EXPECT_TRUE(result.value().components.empty());
}

TEST_F(ParserTest, RejectsABadSettingsPrefix) {
    for (const char* prefix : {"", "Mail", "mail.", ".mail", "a..b", "my app", "mail/x"}) {
        expectError("struct CONFIG(\"" + std::string{prefix} + "\") S { int n; };",
                    "must be lower-case names with dots between them");
    }
    expectError(R"(struct CONFIG("cppboot") S { int n; };)", "can't start with \"cppboot\"");
    expectError(R"(struct CONFIG("cppboot.server") S { int n; };)", "can't start with \"cppboot\"");

    const auto fine = parse(R"(struct CONFIG("my-app.mail_2") S { int n; };)");
    EXPECT_TRUE(fine) << fine.error().message();
}

TEST_F(ParserTest, RejectsSettingsFieldsItCannotFill) {
    expectError(R"(struct CONFIG("a") S { std::map<std::string, int> names; };)",
                "Field 'names' of S can't be read from settings");
    expectError(R"(struct CONFIG("a") S { char letter; };)", "can't be read from settings");
    expectError(R"(struct CONFIG("a") S { const char* text; };)", "can't be read from settings");
    expectError(R"(struct CONFIG("a") S { std::vector<std::vector<int>> grid; };)",
                "can't be read from settings");
    expectError(R"(struct CONFIG("a") S { std::optional<std::vector<int>> maybe; };)",
                "can't be read from settings");
    expectError(R"(struct CONFIG("a") S { const int fixed{1}; };)", "can't be const");
    expectError(R"(class CONFIG("a") S { int hidden; };)", "must be public for CPPBOOT_CONFIG");
    expectError(R"(struct Base { int a; }; struct CONFIG("a") S : Base { int b; };)",
                "can't have base classes");
    expectError(R"(struct CONFIG("a") S {};)", "has no fields to read settings into");
    expectError(R"(struct CONFIG("a") S { static constexpr int kOnly = 1; };)", "has no fields");
}

TEST_F(ParserTest, RejectsSettingsMarksThatContradictOrDoNothing) {
    expectError(R"(struct CONFIG("a") S { REQUIRED std::optional<int> n; };)",
                "is a std::optional, which says it may be missing");
    expectError(R"(struct In { int n; }; struct CONFIG("a") S { REQUIRED In inner; };)",
                "put the marks on the fields of In instead");
    expectError(R"(struct In { int n; }; struct CONFIG("a") S { NOT_EMPTY In inner; };)",
                "put the marks on the fields of In instead");
    expectError(R"(struct CONFIG("a") S { NOT_EMPTY bool flag; };)", "can't be checked");
    expectError(R"(#include <chrono>
                   struct CONFIG("a") S { MIN(1) std::chrono::seconds wait; };)",
                "can't be checked");
    expectError(R"(struct CONFIG("a") SERVICE S { int n; };)",
                "can't be both CPPBOOT_CONFIG and another kind of component");
}

TEST_F(ParserTest, AComponentCanDependOnASettingsStruct) {
    const auto result = parse(R"(
        struct CONFIG("mail") MailSettings { std::string host; };
        class SERVICE Mailer { public: explicit Mailer(const MailSettings& settings); };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value().components.at(0).dependencies,
              std::vector<std::string>{"MailSettings"});
}
