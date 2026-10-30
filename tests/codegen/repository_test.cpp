#include <gtest/gtest.h>

#include <cppboot/boot.hpp>
#include <cppboot/web/router.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "cppboot_components.generated.hpp"

using namespace cppboot;
using namespace cppboot::web;

namespace {

namespace fs = std::filesystem;

template <typename T>
std::shared_ptr<T> borrow(const T& value) {
    return std::shared_ptr<T>{const_cast<T*>(&value), [](T*) {}};
}

}  // namespace

// Runs everything the generator wires up: settings, database, migrations, repository and routes.
class RepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() / (std::string{"cppboot-repo-test-"} + info->name());
        fs::remove_all(directory_);
        fs::create_directories(directory_ / "migrations");
        std::ofstream{directory_ / "migrations" / "001_create_products.sql"}
            << "CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
               "note TEXT, price DOUBLE PRECISION NOT NULL, "
               "in_stock BOOLEAN NOT NULL DEFAULT TRUE);\n";

        config::ConfigSources sources;
        sources.directory = directory_;
        sources.defaults = {
            {"cppboot.logging.level", "off"},
            {"cppboot.database.url", "sqlite:" + (directory_ / "test.db").generic_string()},
            {"cppboot.database.migrations", (directory_ / "migrations").generic_string()},
        };
        config_ = std::make_unique<config::Config>(config::Config::load(sources).value());
        logger_ = std::make_unique<logging::Logger>(logging::Logger::create({}).value());

        container_ = std::make_unique<di::Container>();
        container_->registerInstance(borrow(*config_));
        container_->registerInstance(borrow(*logger_));
        cppboot::generated::registerComponents(*container_);

        const auto valid = container_->validate();
        ASSERT_TRUE(valid) << valid.error().message();

        context_ = std::make_unique<ApplicationContext>(
            *config_, *logger_, [] {}, [](std::string_view, std::string_view) {});
        const auto modules = container_->resolveAll<Module>();
        ASSERT_EQ(modules.size(), 2U);
        EXPECT_EQ(modules[0]->name(), "database");
        EXPECT_EQ(modules[1]->name(), "web");

        const auto started = modules[0]->start(*context_);
        ASSERT_TRUE(started) << started.error().message();

        router_ = container_->resolve<Router>();
    }

    void TearDown() override {
        router_.reset();
        context_.reset();
        container_.reset();
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    Response send(Method method, std::string_view target, std::string body = {}) const {
        return router_->handle(Request::create(method, target, {}, std::move(body)).value());
    }

    fs::path directory_;
    std::unique_ptr<config::Config> config_;
    std::unique_ptr<logging::Logger> logger_;
    std::unique_ptr<di::Container> container_;
    std::unique_ptr<ApplicationContext> context_;
    std::shared_ptr<Router> router_;
};

TEST_F(RepositoryTest, CreatesAndReadsThroughTheWholeStack) {
    const Response created =
        send(Method::Post, "/products", R"({"name":"Tea","note":"green","price":2.5})");
    ASSERT_EQ(created.status(), 201) << created.body();
    EXPECT_EQ(created.body(),
              R"({"id":1,"name":"Tea","note":"green","price":2.5,"in_stock":true})");

    const Response found = send(Method::Get, "/products/1");
    EXPECT_EQ(found.status(), 200);
    EXPECT_EQ(found.body(), created.body());
}

TEST_F(RepositoryTest, OptionalColumnBecomesNull) {
    const Response created = send(Method::Post, "/products", R"({"name":"Tea","price":1})");
    ASSERT_EQ(created.status(), 201) << created.body();
    EXPECT_NE(created.body().find(R"("note":null)"), std::string::npos);
}

TEST_F(RepositoryTest, ListsEveryRow) {
    send(Method::Post, "/products", R"({"name":"A","price":1})");
    send(Method::Post, "/products", R"({"name":"B","price":2})");

    const Json list = Json::parse(send(Method::Get, "/products").body());
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[1]["name"], "B");
}

TEST_F(RepositoryTest, MissingRowIsNotFound) {
    EXPECT_EQ(send(Method::Get, "/products/99").status(), 404);
}

TEST_F(RepositoryTest, TransactionKeepsAllOrNothing) {
    const Response good =
        send(Method::Post, "/products/batch", R"([{"name":"A","price":1},{"name":"B","price":2}])");
    EXPECT_EQ(good.status(), 200) << good.body();
    EXPECT_EQ(good.body(), "2");

    // C is new, but A already exists, so neither is kept.
    const Response bad =
        send(Method::Post, "/products/batch", R"([{"name":"C","price":1},{"name":"A","price":2}])");
    EXPECT_EQ(bad.status(), 500);
    EXPECT_EQ(Json::parse(send(Method::Get, "/products").body()).size(), 2U);
}

TEST_F(RepositoryTest, DatabaseErrorDetailsDoNotReachTheClient) {
    send(Method::Post, "/products", R"({"name":"A","price":1})");

    const Response duplicate = send(Method::Post, "/products", R"({"name":"A","price":1})");
    EXPECT_EQ(duplicate.status(), 500);
    EXPECT_EQ(duplicate.body().find("UNIQUE"), std::string::npos);
    EXPECT_EQ(duplicate.body().find("products"), std::string::npos);
}
