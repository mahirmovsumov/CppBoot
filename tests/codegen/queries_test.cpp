#include <gtest/gtest.h>

#include <algorithm>
#include <cppboot/boot.hpp>
#include <cppboot/data/database.hpp>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cppboot_components.generated.hpp"

using catalog::Book;
using catalog::BookRepository;
using namespace cppboot;

namespace {

namespace fs = std::filesystem;
using Titles = std::vector<std::string>;

/// The titles in the order the query returned them, for queries with an ORDER BY.
Titles titles(const std::vector<Book>& books) {
    Titles result;
    for (const Book& book : books) {
        result.push_back(book.title);
    }
    return result;
}

/// The titles in any order, for queries without one, since SQL doesn't promise an order then.
Titles anyOrder(const std::vector<Book>& books) {
    Titles result = titles(books);
    std::sort(result.begin(), result.end());
    return result;
}

Titles sorted(Titles names) {
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

// Runs the functions the generator wrote for BookRepository against SQLite, and against PostgreSQL
// when CPPBOOT_TEST_POSTGRESQL_URL names a database these tests may change.
class QueriesTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info->name();
        std::replace(name.begin(), name.end(), '/', '-');
        directory_ = fs::temp_directory_path() / ("cppboot-queries-test-" + name);
        fs::remove_all(directory_);
        fs::create_directories(directory_);

        const bool postgresql = GetParam() == "postgresql";
        std::string url = "sqlite:" + (directory_ / "test.db").generic_string();
        if (postgresql) {
            const char* const postgresql_url = std::getenv("CPPBOOT_TEST_POSTGRESQL_URL");
            if (postgresql_url == nullptr) {
                GTEST_SKIP() << "Set CPPBOOT_TEST_POSTGRESQL_URL to test PostgreSQL";
            }
            url = postgresql_url;
        }

        config::ConfigSources sources;
        sources.directory = directory_;
        sources.defaults = {{"cppboot.database.url", url}};
        container_ = std::make_unique<di::Container>();
        container_->registerInstance(
            std::make_shared<config::Config>(config::Config::load(sources).value()));
        container_->registerInstance(
            std::make_shared<logging::Logger>(logging::Logger::create({}).value()));
        cppboot::generated::registerComponents(*container_);

        auto database = container_->resolve<data::Database>();
        database->execute("DROP TABLE IF EXISTS books");
        database->execute(
            std::string{"CREATE TABLE books (id "} +
            (postgresql ? "BIGSERIAL PRIMARY KEY" : "INTEGER PRIMARY KEY") +
            ", title TEXT NOT NULL, author TEXT NOT NULL, price DOUBLE PRECISION NOT NULL, "
            "pages INTEGER NOT NULL, in_print BOOLEAN NOT NULL, series TEXT)");
        const auto add = [&](const char* title, const char* author, double price, int pages,
                             bool in_print, std::optional<std::string> series) {
            database->execute(
                "INSERT INTO books (title, author, price, pages, in_print, series) "
                "VALUES ($1, $2, $3, $4, $5, $6)",
                title, author, price, pages, in_print, series);
        };
        add("Dune", "Herbert", 12.5, 412, true, "Dune");
        add("Children of Dune", "Herbert", 9.0, 444, true, "Dune");
        add("Emma", "Austen", 7.0, 474, true, std::nullopt);
        add("Persuasion", "Austen", 6.5, 249, false, std::nullopt);
        add("100%_Real", "Odd", 3.0, 99, true, std::nullopt);

        books_ = container_->resolve<BookRepository>();
    }

    void TearDown() override {
        books_.reset();
        container_.reset();
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    fs::path directory_;
    std::unique_ptr<di::Container> container_;
    std::shared_ptr<BookRepository> books_;
};

INSTANTIATE_TEST_SUITE_P(Databases, QueriesTest, ::testing::Values("sqlite", "postgresql"),
                         [](const ::testing::TestParamInfo<std::string>& database) {
                             return database.param;
                         });

TEST_P(QueriesTest, TheInterfaceIsAComponent) {
    const auto valid = container_->validate();
    EXPECT_TRUE(valid) << valid.error().message();
}

TEST_P(QueriesTest, FindsAllAndById) {
    EXPECT_EQ(books_->findAll().size(), 5U);
    const auto emma = books_->findById(3);
    ASSERT_TRUE(emma);
    EXPECT_EQ(emma->title, "Emma");
    EXPECT_FALSE(emma->series);
    EXPECT_FALSE(books_->findById(99));
}

TEST_P(QueriesTest, CombinesConditionsAndOrders) {
    EXPECT_EQ(anyOrder(books_->findByAuthor("Austen")), sorted({"Emma", "Persuasion"}));
    EXPECT_EQ(titles(books_->findByAuthorOrderByPriceDesc("Herbert")),
              (Titles{"Dune", "Children of Dune"}));
    EXPECT_EQ(titles(books_->findByAuthorAndPriceLessThan("Herbert", 10)),
              (Titles{"Children of Dune"}));
    EXPECT_EQ(anyOrder(books_->findByAuthorOrTitle("Odd", "Emma")), sorted({"Emma", "100%_Real"}));
    EXPECT_EQ(titles(books_->findByPagesBetweenOrderByPagesAsc(249, 444)),
              (Titles{"Persuasion", "Dune", "Children of Dune"}));
    EXPECT_EQ(books_->findByPriceGreaterThanEqual(9).size(), 2U);
    EXPECT_EQ(titles(books_->findByPriceLessThanEqualOrderByTitle(7)),
              (Titles{"100%_Real", "Emma", "Persuasion"}));
    EXPECT_EQ(books_->findByAuthorNot("Herbert").size(), 3U);
}

TEST_P(QueriesTest, MatchesTextLiterally) {
    EXPECT_EQ(titles(books_->findByTitleStartingWith("Child")), (Titles{"Children of Dune"}));
    EXPECT_EQ(anyOrder(books_->findByTitleEndingWith("Dune")), sorted({"Dune", "Children of Dune"}));
    EXPECT_EQ(anyOrder(books_->findByTitleContainingIgnoreCase("DUNE")),
              sorted({"Dune", "Children of Dune"}));
    EXPECT_EQ(anyOrder(books_->findByAuthorIgnoreCase("austen")), sorted({"Emma", "Persuasion"}));

    // % and _ are text here, not wildcards, so they match only themselves.
    EXPECT_EQ(titles(books_->findByTitleContainingIgnoreCase("%_")), (Titles{"100%_Real"}));
    EXPECT_TRUE(books_->findByTitleStartingWith("%").empty());
    EXPECT_TRUE(books_->findByTitleContainingIgnoreCase("0_r").empty());
}

TEST_P(QueriesTest, ChecksForNullAndBools) {
    EXPECT_EQ(books_->findBySeriesIsNull().size(), 3U);
    EXPECT_EQ(titles(books_->findBySeriesIsNotNullOrderByIdAsc()), (Titles{"Dune", "Children of Dune"}));
    EXPECT_EQ(titles(books_->findByInPrintFalse()), (Titles{"Persuasion"}));
}

TEST_P(QueriesTest, ReturnsPages) {
    EXPECT_EQ(titles(books_->findByInPrintTrueOrderByIdAsc({2, 0})), (Titles{"Dune", "Children of Dune"}));
    EXPECT_EQ(titles(books_->findByInPrintTrueOrderByIdAsc({2, 2})), (Titles{"Emma", "100%_Real"}));
    EXPECT_TRUE(books_->findByInPrintTrueOrderByIdAsc({2, 10}).empty());
}

TEST_P(QueriesTest, FindsTheFirstOrTheTop) {
    EXPECT_EQ(books_->findFirstByAuthorOrderByPriceAsc("Herbert")->title, "Children of Dune");
    EXPECT_FALSE(books_->findFirstByAuthorOrderByPriceAsc("Nobody"));
    EXPECT_EQ(books_->findFirstByOrderByPagesDesc()->title, "Emma");
    EXPECT_EQ(titles(books_->findTop2ByOrderByPriceDesc()), (Titles{"Dune", "Children of Dune"}));
}

TEST_P(QueriesTest, AnOptionalOfOneRowRejectsTwo) {
    EXPECT_EQ(books_->findByTitle("Emma")->author, "Austen");
    EXPECT_FALSE(books_->findByTitle("Nothing"));

    container_->resolve<data::Database>()->execute(
        "INSERT INTO books (title, author, price, pages, in_print) VALUES ('Emma', 'Copy', 1, 1, TRUE)");
    try {
        static_cast<void>(books_->findByTitle("Emma"));
        FAIL() << "two rows were accepted";
    } catch (const data::DatabaseError& error) {
        EXPECT_NE(std::string{error.what()}.find("findByTitle found more than one row"),
                  std::string::npos)
            << error.what();
    }
}

TEST_P(QueriesTest, CountsChecksAndDeletes) {
    EXPECT_EQ(books_->count(), 5);
    EXPECT_EQ(books_->countByAuthor("Herbert"), 2);
    EXPECT_EQ(books_->countByAuthor("Nobody"), 0);
    EXPECT_TRUE(books_->existsByTitle("Dune"));
    EXPECT_FALSE(books_->existsByTitle("Nothing"));
    EXPECT_TRUE(books_->hasBooksBy("Austen"));

    EXPECT_EQ(books_->deleteByAuthor("Austen"), 2);
    EXPECT_EQ(books_->deleteByAuthor("Austen"), 0);
    books_->deleteByPriceGreaterThan(10);
    EXPECT_EQ(anyOrder(books_->findAll()), sorted({"Children of Dune", "100%_Real"}));
}

TEST_P(QueriesTest, RunsItsOwnSql) {
    const auto counts = books_->countByEachAuthor();
    ASSERT_EQ(counts.size(), 3U);
    EXPECT_EQ(counts[0].author, "Austen");
    EXPECT_EQ(counts[0].books, 2);

    EXPECT_EQ(books_->highestPrice("Herbert"), 12.5);
    EXPECT_EQ(books_->countLongerThan(400), 3);
    EXPECT_EQ(books_->changePrices(2, "Austen"), 2);
    EXPECT_EQ(books_->findById(3)->price, 14.0);
}

TEST_P(QueriesTest, AnInterfaceOutsideANamespaceWorksToo) {
    EXPECT_EQ(container_->resolve<Shelf>()->countByInPrintTrue(), 4);
}
