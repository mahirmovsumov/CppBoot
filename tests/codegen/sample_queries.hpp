#pragma once

#include <cppboot/base/annotations.hpp>
#include <cppboot/data/repository.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace catalog {

struct CPPBOOT_ROW Book {
    std::int64_t id{0};
    std::string title;
    std::string author;
    double price{0};
    int pages{0};
    bool in_print{true};
    std::optional<std::string> series;
};

struct CPPBOOT_ROW AuthorCount {
    std::string author;
    std::int64_t books{0};
};

// Every function here is written by the generator, from its name or its SQL.
class CPPBOOT_REPOSITORY("books") BookRepository : public cppboot::data::Repository<Book> {
public:
    virtual std::vector<Book> findAll() = 0;
    virtual std::optional<Book> findById(std::int64_t id) = 0;
    virtual std::vector<Book> findByAuthor(const std::string& author) = 0;
    virtual std::vector<Book> findByAuthorOrderByPriceDesc(const std::string& author) = 0;
    virtual std::vector<Book> findByAuthorAndPriceLessThan(const std::string& author,
                                                           double price) = 0;
    virtual std::vector<Book> findByAuthorOrTitle(const std::string& author,
                                                  const std::string& title) = 0;
    virtual std::vector<Book> findByPagesBetweenOrderByPagesAsc(int low, int high) = 0;
    virtual std::vector<Book> findByPriceGreaterThanEqual(double price) = 0;
    virtual std::vector<Book> findByPriceLessThanEqualOrderByTitle(double price) = 0;
    virtual std::vector<Book> findByAuthorNot(const std::string& author) = 0;
    virtual std::vector<Book> findByTitleStartingWith(const std::string& prefix) = 0;
    virtual std::vector<Book> findByTitleContainingIgnoreCase(const std::string& part) = 0;
    virtual std::vector<Book> findByTitleEndingWith(const std::string& suffix) = 0;
    virtual std::vector<Book> findByAuthorIgnoreCase(const std::string& author) = 0;
    virtual std::vector<Book> findBySeriesIsNull() = 0;
    virtual std::vector<Book> findBySeriesIsNotNullOrderByIdAsc() = 0;
    virtual std::vector<Book> findByInPrintFalse() = 0;
    virtual std::vector<Book> findByInPrintTrueOrderByIdAsc(cppboot::data::Page page) = 0;
    virtual std::optional<Book> findFirstByAuthorOrderByPriceAsc(const std::string& author) = 0;
    virtual std::optional<Book> findFirstByOrderByPagesDesc() const = 0;
    virtual std::vector<Book> findTop2ByOrderByPriceDesc() = 0;
    virtual std::optional<Book> findByTitle(const std::string& title) = 0;
    virtual std::int64_t count() = 0;
    virtual std::int64_t countByAuthor(const std::string& author) = 0;
    virtual bool existsByTitle(const std::string& title) = 0;
    virtual std::int64_t deleteByAuthor(const std::string& author) = 0;
    virtual void deleteByPriceGreaterThan(double price) = 0;

    CPPBOOT_QUERY("SELECT author, COUNT(*) AS books FROM books GROUP BY author ORDER BY author")
    virtual std::vector<AuthorCount> countByEachAuthor() = 0;

    CPPBOOT_QUERY("SELECT MAX(price) FROM books WHERE author = $1")
    virtual std::optional<double> highestPrice(const std::string& author) = 0;

    CPPBOOT_QUERY("SELECT COUNT(*) FROM books WHERE pages > $1")
    virtual std::int64_t countLongerThan(int pages) = 0;

    CPPBOOT_QUERY("UPDATE books SET price = price * $1 WHERE author = $2")
    virtual std::int64_t changePrices(double factor, const std::string& author) = 0;

    // Not pure virtual, so the generator leaves it alone, and it can use the others.
    virtual bool hasBooksBy(const std::string& author) { return countByAuthor(author) > 0; }
};

}  // namespace catalog

// In the global namespace, where the generated class's name mustn't clash with it.
class CPPBOOT_REPOSITORY("books") Shelf : public cppboot::data::Repository<catalog::Book> {
public:
    virtual std::int64_t countByInPrintTrue() = 0;
};
