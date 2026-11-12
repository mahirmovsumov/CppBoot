#include <gtest/gtest.h>

#include <cppboot/base/version.hpp>
#include <cppboot/cli/cli.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "generators.hpp"
#include "resource.hpp"
#include "package_files.hpp"

using cppboot::ErrorCode;
using cppboot::cli::createProject;
using cppboot::cli::readProjectName;
using cppboot::cli::validateProjectName;

class CliTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-cli-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    std::string readFile(const std::filesystem::path& path) const {
        std::ifstream input{path, std::ios::binary};
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }

    std::filesystem::path directory_;
};

TEST_F(CliTest, AcceptsValidProjectNames) {
    EXPECT_TRUE(validateProjectName("hello"));
    EXPECT_TRUE(validateProjectName("hello-world_2"));
    EXPECT_TRUE(validateProjectName("Banking"));
}

TEST_F(CliTest, RejectsInvalidProjectNames) {
    for (const char* name : {"", "2fast", "-flag", "has space", "../escape", "a/b", "a.b"}) {
        EXPECT_FALSE(validateProjectName(name)) << name;
    }
}

TEST_F(CliTest, RejectsReservedProjectNames) {
    for (const char* name : {"test", "all", "install", "con", "NUL", "Com1"}) {
        EXPECT_FALSE(validateProjectName(name)) << name;
    }
}

TEST_F(CliTest, CreatesAProjectUsingItsName) {
    ASSERT_TRUE(createProject("demo-app", directory_));

    const auto root = directory_ / "demo-app";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "main.cpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("project(demo-app "), std::string::npos);
    EXPECT_EQ(readFile(root / "CMakeLists.txt").find("@NAME@"), std::string::npos);

    const auto name = readProjectName(root);
    ASSERT_TRUE(name) << name.error().describe();
    EXPECT_EQ(name.value(), "demo-app");
}

TEST_F(CliTest, PlainProjectHasNoWebParts) {
    ASSERT_TRUE(createProject("plain", directory_));

    const auto root = directory_ / "plain";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "greeting.hpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_EQ(readFile(root / "CMakeLists.txt").find("cppboot::web"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("server:"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("@"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "db"));
}

TEST_F(CliTest, SqliteProjectHasAMigrationAndSettings) {
    ASSERT_TRUE(createProject("store", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Sqlite}));

    const auto root = directory_ / "store";
    EXPECT_NE(readFile(root / "cppboot.yaml").find("url: sqlite:store.db"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("port: 8080"), std::string::npos);
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::data"), std::string::npos);
    EXPECT_NE(readFile(root / "src" / "users.hpp").find("CPPBOOT_ROW"), std::string::npos);
    EXPECT_NE(readFile(root / "db" / "migrations" / "001_create_users.sql").find("INTEGER PRIMARY"),
              std::string::npos);
}

TEST_F(CliTest, PostgresqlProjectUsesItsOwnSettingsAndColumnType) {
    ASSERT_TRUE(createProject("store", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Postgresql}));

    const auto root = directory_ / "store";
    EXPECT_NE(readFile(root / "cppboot.yaml").find("localhost:5432/store"), std::string::npos);
    EXPECT_NE(readFile(root / "db" / "migrations" / "001_create_users.sql").find("BIGSERIAL"),
              std::string::npos);
}

TEST_F(CliTest, SecuredProjectHasSignInAndNoSecret) {
    ASSERT_TRUE(createProject("safe", directory_, {.web = true, .security = true}));

    const auto root = directory_ / "safe";
    EXPECT_NE(readFile(root / "src" / "auth.hpp").find("CPPBOOT_PUBLIC"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::security"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("issuer: safe"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("secret"), std::string::npos);
}

TEST_F(CliTest, SecurityOptionNeedsWeb) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--security"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, ActuatorProjectAsksForItAndHasServerSettings) {
    ASSERT_TRUE(createProject("watched", directory_, {.web = true, .actuator = true}));

    const auto root = directory_ / "watched";
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot_generate_components(watched ACTUATOR "),
              std::string::npos);
}

TEST_F(CliTest, ActuatorAloneKeepsThePlainSampleWithAServer) {
    ASSERT_TRUE(createProject("watched", directory_, {.actuator = true}));

    const auto root = directory_ / "watched";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "greeting.hpp"));
    EXPECT_NE(readFile(root / "cppboot.yaml").find("port: 8080"), std::string::npos);
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("ACTUATOR"), std::string::npos);
}

TEST_F(CliTest, EveryOptionCanBeCombined) {
    ASSERT_TRUE(createProject("banking-api", directory_,
                              {.web = true,
                               .database = cppboot::cli::ProjectDatabase::Postgresql,
                               .security = true,
                               .actuator = true}));

    const auto root = directory_ / "banking-api";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "auth.hpp"));
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_TRUE(std::filesystem::exists(root / "db" / "migrations" / "001_create_users.sql"));

    const std::string cmake = readFile(root / "CMakeLists.txt");
    EXPECT_NE(cmake.find("cppboot::data"), std::string::npos);
    EXPECT_NE(cmake.find("cppboot::security"), std::string::npos);
    EXPECT_NE(cmake.find("ACTUATOR"), std::string::npos);

    const std::string settings = readFile(root / "cppboot.yaml");
    EXPECT_NE(settings.find("server:"), std::string::npos);
    EXPECT_NE(settings.find("database:"), std::string::npos);
    EXPECT_NE(settings.find("security:"), std::string::npos);
    // A PostgreSQL URL contains @ before its host, so this looks for whole placeholders.
    for (const char* placeholder : {"@NAME@", "@SETTINGS@", "@LIBRARIES@", "@GENERATE_OPTIONS@"}) {
        EXPECT_EQ(settings.find(placeholder), std::string::npos) << placeholder;
        EXPECT_EQ(cmake.find(placeholder), std::string::npos) << placeholder;
    }
}

TEST_F(CliTest, DatabaseOptionsNeedWebAndCannotBeCombined) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--sqlite"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--web", "--sqlite", "--postgresql"}, out, err),
              cppboot::cli::kExitUsage);
}

TEST_F(CliTest, WebProjectHasAControllerAndServerSettings) {
    ASSERT_TRUE(createProject("web-app", directory_, {.web = true}));

    const auto root = directory_ / "web-app";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "greeting.hpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::web"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("port: 8080"), std::string::npos);
}

TEST_F(CliTest, CreateNeedsExactlyOneName) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "--web"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"create", "one", "two"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, CreateLeavesAnExistingFolderUntouched) {
    const auto root = directory_ / "taken";
    std::filesystem::create_directories(root);
    std::ofstream{root / "keep.txt"} << "mine";

    const auto result = createProject("taken", directory_);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(readFile(root / "keep.txt"), "mine");
    EXPECT_FALSE(std::filesystem::exists(root / "CMakeLists.txt"));
}

TEST_F(CliTest, ReadingAFolderWithNoProjectIsNotFound) {
    const auto name = readProjectName(directory_);
    ASSERT_FALSE(name);
    EXPECT_EQ(name.error().code(), ErrorCode::NotFound);
}

TEST_F(CliTest, ProjectNameFromYamlIsValidated) {
    std::ofstream{directory_ / "CMakeLists.txt"} << "";
    std::ofstream{directory_ / "cppboot.yaml"} << "cppboot:\n  application:\n    name: ../evil\n";

    EXPECT_FALSE(readProjectName(directory_));
}

TEST_F(CliTest, NoArgumentsPrintsUsageAndFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({}, out, err), cppboot::cli::kExitUsage);
    EXPECT_NE(err.str().find("Usage:"), std::string::npos);
}

TEST_F(CliTest, UnknownCommandFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"launch"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, UnknownSanitizerFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"build", "--sanitize=memory"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"run", "--sanitize="}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, BuildTakesNoOtherArguments) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"build", "extra"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"build", "--sanitize=address", "extra"}, out, err),
              cppboot::cli::kExitUsage);
}

TEST_F(CliTest, ServiceAccountNamesAreLowerCaseAndShortEnough) {
    EXPECT_EQ(cppboot::cli::serviceAccountName("My-Api").value(), "my-api");
    EXPECT_TRUE(cppboot::cli::serviceAccountName(std::string(32, 'a')));
    EXPECT_FALSE(cppboot::cli::serviceAccountName(std::string(33, 'a')));
}

TEST_F(CliTest, LinuxPackageFilesAreComplete) {
    ASSERT_TRUE(cppboot::cli::writePackageFiles(directory_, "My-Api", true));

    const auto read = [&](const std::string& file) {
        std::ifstream input{directory_ / file, std::ios::binary};
        std::stringstream text;
        text << input.rdbuf();
        return text.str();
    };

    const std::string unit = read("My-Api.service");
    for (const char* expected :
         {"Type=notify", "User=my-api", "ExecStart=/opt/My-Api/bin/My-Api",
          "RestartPreventExitStatus=78", "NoNewPrivileges=yes", "ProtectSystem=strict",
          "CapabilityBoundingSet=\n", "StateDirectory=My-Api"}) {
        EXPECT_NE(unit.find(expected), std::string::npos) << expected;
    }
    EXPECT_EQ(unit.find("User=root"), std::string::npos);
    EXPECT_EQ(unit.find("@"), unit.find("@system-service"));

    const std::string settings = read("My-Api.env.example");
    EXPECT_NE(settings.find("#CPPBOOT_SECURITY_JWT_SECRET=\n"), std::string::npos);
    EXPECT_EQ(settings.find("@NAME@"), std::string::npos);

    // An upgrade must never replace settings that already exist.
    const std::string install = read("install.sh");
    EXPECT_NE(install.find("if [ ! -e \"$settings\" ]"), std::string::npos);
    EXPECT_NE(install.find("-m 0640"), std::string::npos);

    // Removing the service must not delete anyone's data.
    const std::string uninstall = read("uninstall.sh");
    EXPECT_EQ(uninstall.find("rm -rf \"/var/lib"), std::string::npos);
}

TEST_F(CliTest, WindowsPackageHasNoServiceFiles) {
    ASSERT_TRUE(cppboot::cli::writePackageFiles(directory_, "my-api", false));
    EXPECT_TRUE(std::filesystem::exists(directory_ / "README.md"));
    EXPECT_FALSE(std::filesystem::exists(directory_ / "my-api.service"));
    EXPECT_FALSE(std::filesystem::exists(directory_ / "install.sh"));
}

TEST_F(CliTest, PackageTakesNoArguments) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"package", "extra"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, ProjectsHaveTestsThatAreLeftOutOfNormalBuilds) {
    ASSERT_TRUE(createProject("plain", directory_));
    ASSERT_TRUE(createProject("web-app", directory_, {.web = true}));
    ASSERT_TRUE(createProject("store", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Sqlite}));

    for (const char* name : {"plain", "web-app", "store"}) {
        const auto root = directory_ / name;
        EXPECT_TRUE(std::filesystem::exists(root / "tests" / "components_test.cpp")) << name;
        const std::string cmake = readFile(root / "CMakeLists.txt");
        EXPECT_NE(cmake.find(std::string{"add_executable("} + name + "-tests EXCLUDE_FROM_ALL"),
                  std::string::npos)
            << name;
    }
    EXPECT_TRUE(std::filesystem::exists(directory_ / "plain" / "tests" / "greeting_test.cpp"));
    EXPECT_NE(readFile(directory_ / "plain" / ".vscode" / "launch.json").find("build/plain.exe"),
              std::string::npos);
    EXPECT_NE(readFile(directory_ / "plain" / ".vscode" / "tasks.json").find("cppboot dev"),
              std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(directory_ / "web-app" / "tests" / "users_test.cpp"));
    // The store's sample needs a database, so it only has the wiring test.
    EXPECT_FALSE(std::filesystem::exists(directory_ / "store" / "tests" / "users_test.cpp"));
}

TEST_F(CliTest, CreateAsksWhenGivenNoArguments) {
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(directory_);
    // An invalid name and an unknown answer are asked again.
    std::istringstream in{"2bad\nasked\ny\nmysql\nsqlite\n\ny\n"};
    std::ostringstream out;
    std::ostringstream err;
    const int code = cppboot::cli::run({"create"}, in, out, err);
    std::filesystem::current_path(previous);

    ASSERT_EQ(code, 0) << err.str();
    const auto root = directory_ / "asked";
    const std::string cmake = readFile(root / "CMakeLists.txt");
    EXPECT_NE(cmake.find("cppboot::data"), std::string::npos);
    EXPECT_NE(cmake.find("ACTUATOR"), std::string::npos);
    EXPECT_EQ(cmake.find("cppboot::security"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("sqlite:asked.db"), std::string::npos);
    EXPECT_NE(out.str().find("Answer none, sqlite or postgresql"), std::string::npos);
}

TEST_F(CliTest, CreateWithNothingToReadFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, AddWritesAMarkedClass) {
    ASSERT_TRUE(createProject("app", directory_));
    const auto root = directory_ / "app";

    const auto added = cppboot::cli::addComponent(root, "service", "OrderItem");
    ASSERT_TRUE(added) << added.error().describe();
    ASSERT_EQ(added.value().size(), 1u);
    EXPECT_EQ(added.value().front().generic_string(), "src/order_item_service.hpp");
    const std::string header = readFile(root / "src" / "order_item_service.hpp");
    EXPECT_NE(header.find("class CPPBOOT_SERVICE OrderItemService"), std::string::npos);
    EXPECT_EQ(header.find('@'), std::string::npos);

    // A name that already has the suffix keeps it once.
    ASSERT_TRUE(cppboot::cli::addComponent(root, "module", "ReportModule"));
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "report_module.hpp"));
}

TEST_F(CliTest, AddNeverOverwrites) {
    ASSERT_TRUE(createProject("app", directory_));
    const auto root = directory_ / "app";
    ASSERT_TRUE(cppboot::cli::addComponent(root, "service", "Order"));
    std::ofstream{root / "src" / "order_service.hpp", std::ios::binary} << "mine";

    const auto again = cppboot::cli::addComponent(root, "service", "Order");
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(readFile(root / "src" / "order_service.hpp"), "mine");
}

TEST_F(CliTest, AddChecksTheNameKindAndProject) {
    ASSERT_TRUE(createProject("app", directory_));
    const auto root = directory_ / "app";
    for (const char* name : {"", "order", "Order-Item", "Order Item", "../Evil"}) {
        EXPECT_FALSE(cppboot::cli::addComponent(root, "service", name)) << name;
    }
    EXPECT_FALSE(cppboot::cli::addComponent(root, "widget", "Order"));
    // A plain project has no web server, database or actuator.
    EXPECT_FALSE(cppboot::cli::addComponent(root, "controller", "Order"));
    EXPECT_FALSE(cppboot::cli::addComponent(root, "repository", "Order"));
    EXPECT_FALSE(cppboot::cli::addComponent(root, "health", "Queue"));
    EXPECT_FALSE(cppboot::cli::addComponent(directory_, "service", "Order"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "order_controller.hpp"));
}

TEST_F(CliTest, AddRepositoryWritesTheNextMigration) {
    ASSERT_TRUE(createProject("store", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Postgresql}));
    const auto root = directory_ / "store";

    const auto added = cppboot::cli::addComponent(root, "repository", "Order");
    ASSERT_TRUE(added) << added.error().describe();
    ASSERT_EQ(added.value().size(), 2u);
    const auto migration = root / "db" / "migrations" / "002_create_orders.sql";
    ASSERT_TRUE(std::filesystem::exists(migration));
    EXPECT_NE(readFile(migration).find("CREATE TABLE orders"), std::string::npos);
    EXPECT_NE(readFile(migration).find("BIGSERIAL"), std::string::npos);
    EXPECT_NE(readFile(root / "src" / "order_repository.hpp").find("FROM orders"), std::string::npos);

    // The users table already exists, so a User repository would fail at startup.
    EXPECT_FALSE(cppboot::cli::addComponent(root, "repository", "User"));
}

TEST_F(CliTest, AddControllerUsesAPluralPath) {
    ASSERT_TRUE(createProject("web-app", directory_, {.web = true, .actuator = true}));
    const auto root = directory_ / "web-app";
    ASSERT_TRUE(cppboot::cli::addComponent(root, "controller", "Invoice"));
    EXPECT_NE(readFile(root / "src" / "invoice_controller.hpp").find("CPPBOOT_CONTROLLER(\"/api/invoices\")"),
              std::string::npos);
    EXPECT_TRUE(cppboot::cli::addComponent(root, "middleware", "Audit"));
    EXPECT_TRUE(cppboot::cli::addComponent(root, "health", "Queue"));
}

TEST_F(CliTest, AddAndTestNeedTheirArguments) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"add", "service"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"test", "--sanitize=memory"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, InfoPrintsTheVersion) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"info"}, out, err), 0);
    EXPECT_NE(out.str().find(std::string{cppboot::kVersionString}), std::string::npos);
}

TEST_F(CliTest, AddConfigWritesASettingsStructNamedForItsPrefix) {
    ASSERT_TRUE(createProject("app", directory_));
    const auto root = directory_ / "app";

    const auto added = cppboot::cli::addComponent(root, "config", "OrderMail");
    ASSERT_TRUE(added) << added.error().describe();
    EXPECT_EQ(added.value().front().generic_string(), "src/order_mail_settings.hpp");

    const std::string header = readFile(root / "src" / "order_mail_settings.hpp");
    EXPECT_NE(header.find("struct CPPBOOT_CONFIG(\"order_mail\") OrderMailSettings {"),
              std::string::npos)
        << header;
    EXPECT_NE(header.find("CPPBOOT_ORDER_MAIL_HOST"), std::string::npos);
    EXPECT_EQ(header.find('@'), std::string::npos);
}

TEST_F(CliTest, AddResourceWritesEverythingItNeeds) {
    ASSERT_TRUE(createProject("shop", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Sqlite}));
    const auto root = directory_ / "shop";

    const auto added = cppboot::cli::addResource(
        root, "OrderItem", {"name:string", "price:double", "count:int", "stock:long", "active:bool",
                            "note:string?"});
    ASSERT_TRUE(added) << added.error().describe();
    ASSERT_EQ(added.value().size(), 3U);
    EXPECT_EQ(added.value()[0].generic_string(), "src/order_item.hpp");
    EXPECT_EQ(added.value()[1].generic_string(), "db/migrations/002_create_order_items.sql");
    EXPECT_EQ(added.value()[2].generic_string(), "tests/order_item_test.cpp");

    const std::string header = readFile(root / "src" / "order_item.hpp");
    for (const char* expected :
         {"struct CPPBOOT_JSON CPPBOOT_ROW OrderItem {", "std::int64_t id{0};",
          "std::string name;", "double price{0};", "int count{0};", "std::int64_t stock{0};",
          "bool active{false};", "std::optional<std::string> note;",
          "struct CPPBOOT_JSON OrderItemInput {", "CPPBOOT_LENGTH(1, 255) std::string name;",
          "CPPBOOT_LENGTH(0, 255) std::optional<std::string> note;",
          "class CPPBOOT_SERVICE OrderItemRepository {",
          "class CPPBOOT_CONTROLLER(\"/api/order-items\") OrderItemController {",
          "INSERT INTO order_items (name, price, count, stock, active, note) VALUES ($1, $2, $3, "
          "$4, $5, $6)",
          "UPDATE order_items SET name = $2, price = $3, count = $4, stock = $5, active = $6, note "
          "= $7 WHERE id = $1",
          "ORDER BY id LIMIT $1 OFFSET $2", "\"No order item has that ID\"",
          "CPPBOOT_RANGE(1, 100) std::optional<std::int64_t> limit"}) {
        EXPECT_NE(header.find(expected), std::string::npos) << expected;
    }

    const std::string migration = readFile(root / "db" / "migrations" / "002_create_order_items.sql");
    EXPECT_NE(migration.find("id INTEGER PRIMARY KEY"), std::string::npos);
    EXPECT_NE(migration.find("price REAL NOT NULL"), std::string::npos);
    EXPECT_NE(migration.find("note TEXT\n"), std::string::npos);

    const std::string test = readFile(root / "tests" / "order_item_test.cpp");
    EXPECT_NE(test.find("TEST_F(OrderItemRepositoryTest, AddsFindsReplacesAndRemoves)"),
              std::string::npos);
    EXPECT_NE(test.find("TEST(OrderItemInputTest, ChecksWhatClientsSend)"), std::string::npos);
    EXPECT_EQ(test.find("GTEST_SKIP"), std::string::npos);
}

TEST_F(CliTest, AddResourceUsesTheProjectsDatabase) {
    ASSERT_TRUE(createProject("shop", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Postgresql}));
    const auto root = directory_ / "shop";
    ASSERT_TRUE(cppboot::cli::addResource(root, "Product", {"price:double", "note:string?"}));

    const std::string migration = readFile(root / "db" / "migrations" / "002_create_products.sql");
    EXPECT_NE(migration.find("id BIGSERIAL PRIMARY KEY"), std::string::npos);
    EXPECT_NE(migration.find("price DOUBLE PRECISION NOT NULL"), std::string::npos);
    // The tests need a PostgreSQL database to change, so they're skipped without one.
    EXPECT_NE(readFile(root / "tests" / "product_test.cpp").find("CPPBOOT_TEST_DATABASE_URL"),
              std::string::npos);
    // No required text field means nothing for a check test to leave empty.
    EXPECT_EQ(readFile(root / "tests" / "product_test.cpp").find("ChecksWhatClientsSend"),
              std::string::npos);
}

TEST_F(CliTest, AddResourceRejectsBadFields) {
    const auto rejected = [](std::vector<std::string> fields, std::string_view expected) {
        const auto result = cppboot::cli::parseResourceFields(fields);
        ASSERT_FALSE(result) << expected;
        EXPECT_NE(result.error().message().find(expected), std::string::npos)
            << result.error().message();
    };
    rejected({}, "at least one field");
    rejected({"name"}, "needs a type, such as name:string");
    rejected({"name:text", "name:int"}, "listed twice");
    rejected({"price:money"}, "isn't a type");
    rejected({"Price:double"}, "isn't a field name");
    rejected({"2fast:int"}, "isn't a field name");
    rejected({"unit-price:double"}, "isn't a field name");
    rejected({"id:long"}, "has an id already");
    rejected({"class:string"}, "reserves");
    rejected({"order:int"}, "reserves");
    rejected({"name:string??"}, "isn't a type");

    const auto fine = cppboot::cli::parseResourceFields({"unit_price:double?", "title:text"});
    ASSERT_TRUE(fine) << fine.error().message();
    EXPECT_TRUE(fine.value()[0].optional);
    EXPECT_EQ(fine.value()[1].type, cppboot::cli::FieldType::Text);
}

TEST_F(CliTest, AddResourceChecksTheProjectAndWritesNothingOnAMistake) {
    ASSERT_TRUE(createProject("plain", directory_, {.web = true}));
    const auto plain = cppboot::cli::addResource(directory_ / "plain", "Product", {"title:string"});
    ASSERT_FALSE(plain);
    EXPECT_NE(plain.error().message().find("needs a web project with a database"), std::string::npos);

    ASSERT_TRUE(createProject("shop", directory_, {.web = true, .database = cppboot::cli::ProjectDatabase::Sqlite}));
    const auto root = directory_ / "shop";
    // The sample already has a User struct and a users table.
    const auto user = cppboot::cli::addResource(root, "User", {"title:string"});
    ASSERT_FALSE(user);
    EXPECT_NE(user.error().message().find("already declares User"), std::string::npos)
        << user.error().message();
    EXPECT_FALSE(cppboot::cli::addResource(root, "product", {"title:string"}));
    EXPECT_FALSE(cppboot::cli::addResource(root, "Product", {"title"}));

    ASSERT_TRUE(cppboot::cli::addResource(root, "Product", {"title:string"}));
    const auto again = cppboot::cli::addResource(root, "Product", {"title:string"});
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().message().find("already exists"), std::string::npos);

    EXPECT_FALSE(std::filesystem::exists(root / "db" / "migrations" / "003_create_products.sql"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "user.hpp"));
}
