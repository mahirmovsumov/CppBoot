#include <gtest/gtest.h>

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
namespace cppboot::web {
class Request {};
class Response {};
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
