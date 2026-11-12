#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cppboot::data {

/// The base of an interface whose functions the generator writes from their names, for rows of T:
///
///   class CPPBOOT_REPOSITORY("products") ProductRepository
///       : public cppboot::data::Repository<Product> {
///   public:
///       virtual std::vector<Product> findByPriceLessThan(double price) = 0;
///   };
///
/// T is a CPPBOOT_ROW struct, whose fields are the table's columns. A component takes the
/// interface as ProductRepository&.
template <typename T>
class Repository {
public:
    using Row = T;

    virtual ~Repository() = default;
};

/// Which page of results a repository function returns, as its last parameter.
struct Page {
    std::int64_t limit{20};
    std::int64_t offset{0};
};

enum class TextMatch { StartsWith, EndsWith, Contains };

/// A LIKE pattern that matches text as written, so that % and _ in it aren't wildcards. Use it with
/// ESCAPE '\'.
std::string likePattern(std::string_view text, TextMatch match);

}  // namespace cppboot::data
