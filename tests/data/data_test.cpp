#include <gtest/gtest.h>

#include <atomic>
#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/data/database_module.hpp>
#include <cppboot/data/migrations.hpp>
#include <cppboot/data/repository.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot::data;
using cppboot::ErrorCode;

namespace {

namespace fs = std::filesystem;

struct Account {
    std::int64_t id{0};
    std::string name;
    std::optional<std::string> nickname;
    double balance{0};
    bool active{false};
};

std::shared_ptr<RowMappers> makeMappers() {
    auto mappers = std::make_shared<RowMappers>();
    mappers->add<Account>([](const Row& row, Account& value) {
        row.read("id", value.id);
        row.read("name", value.name);
        row.read("nickname", value.nickname);
        row.read("balance", value.balance);
        row.read("active", value.active);
    });
    return mappers;
}

std::optional<std::string> postgresqlUrl() {
    const char* const url = std::getenv("CPPBOOT_TEST_POSTGRESQL_URL");
    if (url == nullptr || *url == '\0') {
        return std::nullopt;
    }
    return std::string{url};
}

fs::path testDirectory() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string{"cppboot-data-test-"} + info->name();
    for (char& c : name) {
        if (c == '/') {
            c = '-';
        }
    }
    return fs::temp_directory_path() / name;
}

}  // namespace

// Every test here runs against each database, so that drivers behave the same way.
class DriverTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        directory_ = testDirectory();
        fs::remove_all(directory_);
        fs::create_directories(directory_);

        std::string url;
        if (GetParam() == "sqlite") {
            url = "sqlite:" + (directory_ / "test.db").generic_string();
        } else {
            const auto configured = postgresqlUrl();
            if (!configured) {
                GTEST_SKIP() << "Set CPPBOOT_TEST_POSTGRESQL_URL to run PostgreSQL tests";
            }
            url = *configured;
        }

        PoolOptions options;
        options.max_size = 4;
        database_ = std::make_shared<Database>(connectionFactoryFor(url), options, makeMappers());

        resetTables();
        const bool sqlite = GetParam() == "sqlite";
        database_->execute(std::string{"CREATE TABLE accounts ("} +
                           (sqlite ? "id INTEGER PRIMARY KEY" : "id BIGSERIAL PRIMARY KEY") +
                           ", name TEXT NOT NULL UNIQUE, nickname TEXT, "
                           "balance DOUBLE PRECISION NOT NULL DEFAULT 0, "
                           "active BOOLEAN NOT NULL DEFAULT TRUE)");
    }

    void TearDown() override {
        if (database_) {
            resetTables();
        }
        database_.reset();
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    void resetTables() {
        database_->execute("DROP TABLE IF EXISTS accounts");
        database_->execute("DROP TABLE IF EXISTS items");
        database_->execute("DROP TABLE IF EXISTS cppboot_migrations");
    }

    void writeMigration(const std::string& name, const std::string& sql) const {
        fs::create_directories(directory_ / "migrations");
        std::ofstream{directory_ / "migrations" / name, std::ios::binary} << sql;
    }

    auto migrate() { return runMigrations(*database_, directory_ / "migrations"); }

    std::int64_t countAccounts() {
        return *database_->queryValue<std::int64_t>("SELECT COUNT(*) FROM accounts");
    }

    fs::path directory_;
    std::shared_ptr<Database> database_;
};

INSTANTIATE_TEST_SUITE_P(Databases, DriverTest, ::testing::Values("sqlite", "postgresql"));

TEST_P(DriverTest, InsertsAndReadsValuesOfEveryType) {
    const auto affected =
        database_->execute("INSERT INTO accounts (name, nickname, balance, active) "
                           "VALUES ($1, $2, $3, $4)",
                           "Ada", "A", 12.5, false);
    EXPECT_EQ(affected, 1);

    const Rows rows = database_->query("SELECT * FROM accounts");
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].get<std::string>("name"), "Ada");
    EXPECT_EQ(rows[0].get<std::string>("nickname"), "A");
    EXPECT_DOUBLE_EQ(rows[0].get<double>("balance"), 12.5);
    EXPECT_FALSE(rows[0].get<bool>("active"));
}

TEST_P(DriverTest, NullValuesRoundTrip) {
    database_->execute("INSERT INTO accounts (name, nickname) VALUES ($1, $2)", "Ada",
                       std::optional<std::string>{});
    database_->execute("INSERT INTO accounts (name, nickname) VALUES ($1, $2)", "Bob", nullptr);

    const Rows rows = database_->query("SELECT name, nickname FROM accounts ORDER BY name");
    EXPECT_TRUE(rows[0].isNull("nickname"));
    EXPECT_FALSE(rows[0].get<std::optional<std::string>>("nickname"));
    EXPECT_FALSE(rows[1].get<std::optional<std::string>>("nickname"));
    EXPECT_THROW(static_cast<void>(rows[0].get<std::string>("nickname")), DatabaseError);
}

TEST_P(DriverTest, PlaceholdersCanRepeatAndAppearInAnyOrder) {
    const Rows rows = database_->query("SELECT $2 AS a, $1 AS b, $2 AS c", "one", "two");
    EXPECT_EQ(rows[0].get<std::string>("a"), "two");
    EXPECT_EQ(rows[0].get<std::string>("b"), "one");
    EXPECT_EQ(rows[0].get<std::string>("c"), "two");
}

TEST_P(DriverTest, ValuesCannotInjectSql) {
    const std::string attack = "x'); DROP TABLE accounts; --";
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", attack);

    EXPECT_EQ(countAccounts(), 1);
    EXPECT_EQ(database_->queryValue<std::string>("SELECT name FROM accounts"), attack);
}

TEST_P(DriverTest, HandlesTextThatNeedsCare) {
    const std::string text = "quote ' double \" backslash \\ unicode \xC3\xA9 newline\n";
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", text);
    EXPECT_EQ(database_->queryValue<std::string>("SELECT name FROM accounts"), text);
}

TEST_P(DriverTest, KeepsLargeIntegersExact) {
    constexpr std::int64_t kLarge = 9007199254740993;  // Not exact as a double.
    database_->execute("INSERT INTO accounts (id, name) VALUES ($1, $2)", kLarge, "big");
    EXPECT_EQ(database_->queryValue<std::int64_t>("SELECT id FROM accounts"), kLarge);
}

TEST_P(DriverTest, MapsRowsToStructs) {
    database_->execute("INSERT INTO accounts (name, nickname, balance) VALUES ($1, $2, $3)", "Ada",
                       "A", 1.5);
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", "Bob");

    const auto accounts = database_->queryAs<Account>("SELECT * FROM accounts ORDER BY name");
    ASSERT_EQ(accounts.size(), 2U);
    EXPECT_EQ(accounts[0].name, "Ada");
    EXPECT_EQ(accounts[0].nickname, "A");
    EXPECT_DOUBLE_EQ(accounts[0].balance, 1.5);
    EXPECT_TRUE(accounts[0].active);
    EXPECT_GT(accounts[0].id, 0);
    EXPECT_FALSE(accounts[1].nickname);
}

TEST_P(DriverTest, QueryOneReturnsNothingForNoRows) {
    EXPECT_FALSE(database_->queryOne<Account>("SELECT * FROM accounts WHERE id = $1", 99));
    EXPECT_FALSE(database_->queryValue<std::string>("SELECT name FROM accounts"));
}

TEST_P(DriverTest, ReturningGivesBackTheNewRow) {
    const auto account = database_->queryOne<Account>(
        "INSERT INTO accounts (name) VALUES ($1) RETURNING *", "Ada");
    ASSERT_TRUE(account);
    EXPECT_EQ(account->name, "Ada");
    EXPECT_GT(account->id, 0);
}

TEST_P(DriverTest, ReportsAffectedRows) {
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", "a");
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", "b");

    EXPECT_EQ(database_->execute("UPDATE accounts SET balance = 1"), 2);
    EXPECT_EQ(database_->execute("DELETE FROM accounts WHERE name = $1", "absent"), 0);
    EXPECT_EQ(database_->execute("DELETE FROM accounts WHERE name = $1", "a"), 1);
}

TEST_P(DriverTest, ReportsUniqueViolations) {
    database_->execute("INSERT INTO accounts (name) VALUES ($1)", "Ada");
    try {
        database_->execute("INSERT INTO accounts (name) VALUES ($1)", "Ada");
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::UniqueViolation);
    }
}

TEST_P(DriverTest, ReportsOtherConstraintViolations) {
    try {
        database_->execute("INSERT INTO accounts (name) VALUES ($1)", nullptr);
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::ConstraintViolation);
    }
}

TEST_P(DriverTest, ReportsQueryErrors) {
    try {
        database_->query("SELECT * FROM no_such_table");
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::Query);
    }
}

TEST_P(DriverTest, RejectsTwoStatementsInOneQuery) {
    EXPECT_THROW(database_->query("SELECT 1; SELECT 2"), DatabaseError);
}

TEST_P(DriverTest, MissingColumnIsAMappingError) {
    const Rows rows = database_->query("SELECT 1 AS one");
    try {
        static_cast<void>(rows[0].get<int>("two"));
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::Mapping);
    }
}

TEST_P(DriverTest, UnmappedStructIsAClearError) {
    struct Unmapped {};
    EXPECT_THROW(static_cast<void>(database_->queryOne<Unmapped>("SELECT 1")), DatabaseError);
}

TEST_P(DriverTest, TransactionCommitsWhenTheFunctionReturns) {
    const auto id = database_->transaction([](Transaction& transaction) {
        transaction.execute("INSERT INTO accounts (name) VALUES ($1)", "a");
        return *transaction.queryValue<std::int64_t>(
            "INSERT INTO accounts (name) VALUES ($1) RETURNING id", "b");
    });

    EXPECT_GT(id, 0);
    EXPECT_EQ(countAccounts(), 2);
}

TEST_P(DriverTest, TransactionRollsBackWhenTheFunctionThrows) {
    EXPECT_THROW(database_->transaction([](Transaction& transaction) {
        transaction.execute("INSERT INTO accounts (name) VALUES ($1)", "a");
        throw std::runtime_error("changed my mind");
    }),
                 std::runtime_error);

    EXPECT_EQ(countAccounts(), 0);
}

TEST_P(DriverTest, FailedStatementRollsBackTheWholeTransaction) {
    EXPECT_THROW(database_->transaction([](Transaction& transaction) {
        transaction.execute("INSERT INTO accounts (name) VALUES ($1)", "a");
        transaction.execute("INSERT INTO accounts (name) VALUES ($1)", "a");
    }),
                 DatabaseError);

    EXPECT_EQ(countAccounts(), 0);
}

TEST_P(DriverTest, ConnectionIsReusedAfterATransactionFails) {
    EXPECT_THROW(database_->transaction([](Transaction&) { throw std::runtime_error("no"); }),
                 std::runtime_error);
    for (int i = 0; i < 8; ++i) {
        EXPECT_NO_THROW(database_->query("SELECT 1"));
    }
    EXPECT_LE(database_->pool().openCount(), 4U);
}

TEST_P(DriverTest, UncommittedChangesAreIsolatedUntilCommit) {
    database_->transaction([&](Transaction& transaction) {
        transaction.execute("INSERT INTO accounts (name) VALUES ($1)", "a");
        const auto seen_inside =
            transaction.queryValue<std::int64_t>("SELECT COUNT(*) FROM accounts");
        EXPECT_EQ(seen_inside, 1);
    });
    EXPECT_EQ(countAccounts(), 1);
}

TEST_P(DriverTest, ManyThreadsCanShareTheDatabase) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10;
    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    try {
                        database_->execute("INSERT INTO accounts (name) VALUES ($1)",
                                           std::to_string(t) + "-" + std::to_string(i));
                    } catch (...) {
                        ++failures;
                    }
                }
            });
        }
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(countAccounts(), kThreads * kPerThread);
    EXPECT_LE(database_->pool().openCount(), 4U);
}

TEST_P(DriverTest, MigrationsApplyInNumberOrderAndOnlyOnce) {
    writeMigration("002_add_price.sql", "ALTER TABLE items ADD COLUMN price INTEGER;");
    writeMigration("001_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);\n"
                                           "CREATE INDEX items_id ON items (id);");

    const auto first = migrate();
    ASSERT_TRUE(first) << first.error().message();
    EXPECT_EQ(first.value(), 2U);
    EXPECT_EQ(database_->execute("INSERT INTO items (id, price) VALUES ($1, $2)", 1, 5), 1);

    const auto second = migrate();
    ASSERT_TRUE(second) << second.error().message();
    EXPECT_EQ(second.value(), 0U);
}

TEST_P(DriverTest, NewMigrationsApplyLater) {
    writeMigration("001_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);");
    ASSERT_TRUE(migrate());

    writeMigration("002_add_price.sql", "ALTER TABLE items ADD COLUMN price INTEGER;");
    const auto applied = migrate();
    ASSERT_TRUE(applied) << applied.error().message();
    EXPECT_EQ(applied.value(), 1U);
}

TEST_P(DriverTest, FailedMigrationRollsBackAndKeepsEarlierOnes) {
    writeMigration("001_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);");
    writeMigration("002_broken.sql", "ALTER TABLE items ADD COLUMN a INTEGER;\nNOT VALID SQL;");

    const auto result = migrate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("002_broken.sql"), std::string::npos);

    // The first applied, and the second's ALTER rolled back.
    EXPECT_NO_THROW(database_->query("SELECT id FROM items"));
    EXPECT_THROW(database_->query("SELECT a FROM items"), DatabaseError);

    // Fixing the file lets it apply, because it was never recorded.
    writeMigration("002_broken.sql", "ALTER TABLE items ADD COLUMN a INTEGER;");
    const auto fixed = migrate();
    ASSERT_TRUE(fixed) << fixed.error().message();
    EXPECT_EQ(fixed.value(), 1U);
}

TEST_P(DriverTest, ChangingAnAppliedMigrationFails) {
    writeMigration("001_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);");
    ASSERT_TRUE(migrate());

    writeMigration("001_create_items.sql", "CREATE TABLE items (id BIGINT PRIMARY KEY);");
    const auto result = migrate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("has changed"), std::string::npos);
}

TEST_P(DriverTest, LineEndingsDoNotCountAsAChange) {
    writeMigration("001_create_items.sql", "CREATE TABLE items\n(id INTEGER PRIMARY KEY);\n");
    ASSERT_TRUE(migrate());

    writeMigration("001_create_items.sql", "CREATE TABLE items\r\n(id INTEGER PRIMARY KEY);\r\n");
    EXPECT_TRUE(migrate());
}

TEST_P(DriverTest, MissingAppliedMigrationFails) {
    writeMigration("001_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);");
    ASSERT_TRUE(migrate());

    fs::remove(directory_ / "migrations" / "001_create_items.sql");
    const auto result = migrate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("missing"), std::string::npos);
}

TEST_P(DriverTest, MigrationBelowAnAppliedOneFails) {
    writeMigration("002_create_items.sql", "CREATE TABLE items (id INTEGER PRIMARY KEY);");
    ASSERT_TRUE(migrate());

    writeMigration("001_earlier.sql", "CREATE TABLE earlier (id INTEGER PRIMARY KEY);");
    const auto result = migrate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("numbered below"), std::string::npos);
}

TEST_P(DriverTest, DuplicateMigrationNumbersFail) {
    writeMigration("001_a.sql", "SELECT 1;");
    writeMigration("001_b.sql", "SELECT 1;");
    EXPECT_FALSE(migrate());
}

TEST_P(DriverTest, BadMigrationFileNamesFail) {
    for (const char* name : {"create.sql", "001.sql", "001-create.sql", "0_zero.sql", "001_a.txt"}) {
        fs::remove_all(directory_ / "migrations");
        writeMigration(name, "SELECT 1;");
        EXPECT_FALSE(migrate()) << name;
    }
}

TEST_P(DriverTest, MissingMigrationFolderFails) {
    const auto result = migrate();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(PostgresqlTest, BadCredentialsFailWithoutShowingThePassword) {
    const auto configured = postgresqlUrl();
    if (!configured) {
        GTEST_SKIP() << "Set CPPBOOT_TEST_POSTGRESQL_URL to run PostgreSQL tests";
    }

    Database database{connectionFactoryFor(
                          "postgresql://postgres:wrong-hunter2@127.0.0.1:1/absent?connect_timeout=2"),
                      PoolOptions{}};
    try {
        database.query("SELECT 1");
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::Connection);
        EXPECT_EQ(std::string{error.what()}.find("hunter2"), std::string::npos);
    }
}

TEST(SqliteTest, InMemoryDatabaseWorksThroughSettings) {
    cppboot::config::ConfigSources sources;
    sources.directory = fs::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = {{"cppboot.database.url", "sqlite::memory:"},
                        {"cppboot.database.pool_size", "5"}};
    const auto database = Database::fromSettings(cppboot::config::Config::load(sources).value());

    // This only works if every statement uses the same connection.
    database->execute("CREATE TABLE t (id INTEGER)");
    database->execute("INSERT INTO t VALUES ($1)", 1);
    EXPECT_EQ(database->queryValue<int>("SELECT id FROM t"), 1);
}

TEST(SettingsTest, RequiresAUrlAndChecksValues) {
    cppboot::config::ConfigSources sources;
    sources.directory = fs::temp_directory_path() / "cppboot-absent-directory";
    EXPECT_THROW(Database::fromSettings(cppboot::config::Config::load(sources).value()),
                 cppboot::Exception);

    sources.defaults = {{"cppboot.database.url", "mysql://host/db"}};
    EXPECT_THROW(Database::fromSettings(cppboot::config::Config::load(sources).value()),
                 cppboot::Exception);

    sources.defaults = {{"cppboot.database.url", "sqlite::memory:"},
                        {"cppboot.database.pool_size", "0"}};
    EXPECT_THROW(Database::fromSettings(cppboot::config::Config::load(sources).value()),
                 cppboot::Exception);
}

TEST(SettingsTest, UnsupportedUrlMessageDoesNotShowTheUrl) {
    try {
        connectionFactoryFor("mysql://user:hunter2@host/db");
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(std::string{error.what()}.find("hunter2"), std::string::npos);
    }
}

namespace {

// A connection with no database, to test the pool on its own.
class FakeConnection : public Connection {
public:
    explicit FakeConnection(std::shared_ptr<std::atomic<bool>> healthy)
        : healthy_(std::move(healthy)) {}

    QueryResult execute(std::string_view, const std::vector<Param>&) override { return {}; }
    void executeScript(const std::string&) override {}
    bool isHealthy() const noexcept override { return healthy_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> healthy_;
};

}  // namespace

class PoolTest : public ::testing::Test {
protected:
    ConnectionFactory factory() {
        return [this]() -> std::unique_ptr<Connection> {
            ++created_;
            if (fail_) {
                throw DatabaseError(DatabaseError::Kind::Connection, "refused");
            }
            return std::make_unique<FakeConnection>(healthy_);
        };
    }

    static PoolOptions options(std::size_t size, int timeout_ms = 2000) {
        return PoolOptions{size, std::chrono::milliseconds{timeout_ms}};
    }

    std::atomic<int> created_{0};
    std::atomic<bool> fail_{false};
    std::shared_ptr<std::atomic<bool>> healthy_ = std::make_shared<std::atomic<bool>>(true);
};

TEST_F(PoolTest, OpensConnectionsOnlyWhenNeeded) {
    ConnectionPool pool{factory(), options(3)};
    EXPECT_EQ(pool.openCount(), 0U);
    {
        auto lease = pool.acquire();
        EXPECT_EQ(pool.openCount(), 1U);
        EXPECT_EQ(pool.idleCount(), 0U);
    }
    EXPECT_EQ(pool.openCount(), 1U);
    EXPECT_EQ(pool.idleCount(), 1U);
}

TEST_F(PoolTest, ReusesAReturnedConnection) {
    ConnectionPool pool{factory(), options(3)};
    { auto lease = pool.acquire(); }
    { auto lease = pool.acquire(); }
    EXPECT_EQ(created_.load(), 1);
}

TEST_F(PoolTest, TimesOutWhenAllConnectionsAreInUse) {
    ConnectionPool pool{factory(), options(1, 50)};
    auto lease = pool.acquire();
    try {
        static_cast<void>(pool.acquire());
        FAIL() << "Expected an exception";
    } catch (const DatabaseError& error) {
        EXPECT_EQ(error.kind(), DatabaseError::Kind::Timeout);
    }
    EXPECT_EQ(pool.openCount(), 1U);
}

TEST_F(PoolTest, WaitsForAConnectionToBecomeFree) {
    ConnectionPool pool{factory(), options(1, 10000)};
    std::latch holding{1};
    std::latch release{1};

    std::jthread holder{[&] {
        auto lease = pool.acquire();
        holding.count_down();
        release.wait();
    }};
    holding.wait();

    std::jthread waiter{[&] {
        auto lease = pool.acquire();
        EXPECT_EQ(created_.load(), 1);
    }};
    release.count_down();
}

TEST_F(PoolTest, ClosesAnUnhealthyConnectionOnReturn) {
    ConnectionPool pool{factory(), options(1)};
    {
        auto lease = pool.acquire();
        healthy_->store(false);
    }
    EXPECT_EQ(pool.openCount(), 0U);

    healthy_->store(true);
    EXPECT_NO_THROW(static_cast<void>(pool.acquire()));
    EXPECT_EQ(created_.load(), 2);
}

TEST_F(PoolTest, ReplacesAConnectionThatBrokeWhileIdle) {
    ConnectionPool pool{factory(), options(1)};
    { auto lease = pool.acquire(); }
    EXPECT_EQ(pool.idleCount(), 1U);

    healthy_->store(false);
    auto replacement = pool.acquire();
    EXPECT_EQ(created_.load(), 2);
    EXPECT_EQ(pool.openCount(), 1U);
    healthy_->store(true);
}

TEST_F(PoolTest, DiscardedConnectionIsNotReused) {
    ConnectionPool pool{factory(), options(1)};
    {
        auto lease = pool.acquire();
        lease.discard();
    }
    EXPECT_EQ(pool.openCount(), 0U);
}

TEST_F(PoolTest, FailedConnectionFreesItsPlace) {
    ConnectionPool pool{factory(), options(1, 50)};
    fail_ = true;
    EXPECT_THROW(static_cast<void>(pool.acquire()), DatabaseError);
    EXPECT_EQ(pool.openCount(), 0U);

    fail_ = false;
    EXPECT_NO_THROW(static_cast<void>(pool.acquire()));
}

TEST_F(PoolTest, RejectsASizeOfZero) {
    EXPECT_THROW((ConnectionPool{factory(), options(0)}), DatabaseError);
}

TEST_F(PoolTest, NeverOpensMoreThanTheLimitUnderLoad) {
    ConnectionPool pool{factory(), options(3, 10000)};
    std::atomic<int> in_use{0};
    std::atomic<int> most_in_use{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 50; ++i) {
                    auto lease = pool.acquire();
                    const int now = ++in_use;
                    int seen = most_in_use.load();
                    while (seen < now && !most_in_use.compare_exchange_weak(seen, now)) {
                    }
                    --in_use;
                }
            });
        }
    }
    EXPECT_LE(most_in_use.load(), 3);
    EXPECT_LE(created_.load(), 3);
}

TEST(LikePatternTest, EscapesWildcardsSoTextMatchesAsWritten) {
    using cppboot::data::likePattern;
    using cppboot::data::TextMatch;
    EXPECT_EQ(likePattern("tea", TextMatch::StartsWith), "tea%");
    EXPECT_EQ(likePattern("tea", TextMatch::EndsWith), "%tea");
    EXPECT_EQ(likePattern("tea", TextMatch::Contains), "%tea%");
    EXPECT_EQ(likePattern(R"(50%_off\)", TextMatch::Contains), R"(%50\%\_off\\%)");
    EXPECT_EQ(likePattern("", TextMatch::Contains), "%%");
}
