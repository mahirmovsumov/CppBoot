#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "cppboot/data/driver.hpp"

namespace cppboot::data {

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

bool parseBool(std::string_view text, std::string_view column);
[[noreturn]] void throwConversionError(std::string_view text, std::string_view column,
                                       std::string_view expected);

template <typename T>
T convertColumn(const std::string& text, std::string_view column) {
    if constexpr (std::is_same_v<T, std::string>) {
        return text;
    } else if constexpr (std::is_same_v<T, bool>) {
        return parseBool(text, column);
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        T value{};
        const char* const end = text.data() + text.size();
        const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || parsed_end != end) {
            throwConversionError(text, column, "a number in range");
        }
        return value;
    } else {
        static_assert(!sizeof(T), "Columns can be read as text, a number, a bool or a std::optional");
    }
}

}  // namespace detail

/// One row of a result. It's valid as long as the Rows it came from.
class Row {
public:
    Row(const QueryResult& result, std::size_t index) : result_(&result), index_(index) {}

    /// Returns a column by name, or by position with an index. Only a std::optional can read SQL
    /// NULL. A missing column or an unconvertible value throws DatabaseError.
    template <typename T>
    T get(std::string_view column) const {
        return convert<T>(valueAt(indexOf(column)), column);
    }

    template <typename T>
    T get(std::size_t column) const {
        return convert<T>(valueAt(column), nameOf(column));
    }

    /// Reads a column into an existing variable. Generated code calls this.
    template <typename T>
    void read(std::string_view column, T& value) const {
        value = get<T>(column);
    }

    bool isNull(std::string_view column) const { return !valueAt(indexOf(column)).has_value(); }

private:
    template <typename T>
    static T convert(const std::optional<std::string>& value, std::string_view column) {
        if constexpr (detail::IsOptional<T>::value) {
            if (!value) {
                return std::nullopt;
            }
            return detail::convertColumn<typename T::value_type>(*value, column);
        } else {
            if (!value) {
                throw DatabaseError(DatabaseError::Kind::Mapping,
                                    "Column '" + std::string{column} +
                                        "' is NULL. Read it as a std::optional");
            }
            return detail::convertColumn<T>(*value, column);
        }
    }

    std::size_t indexOf(std::string_view column) const;
    std::string_view nameOf(std::size_t column) const;
    const std::optional<std::string>& valueAt(std::size_t column) const;

    const QueryResult* result_;
    std::size_t index_;
};

/// The rows a statement returned, which you can use in a range-based for loop.
class Rows {
public:
    explicit Rows(QueryResult result)
        : result_(std::make_shared<const QueryResult>(std::move(result))) {}

    class Iterator {
    public:
        Iterator(const QueryResult& result, std::size_t index) : result_(&result), index_(index) {}
        Row operator*() const { return Row{*result_, index_}; }
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        bool operator==(const Iterator& other) const { return index_ == other.index_; }

    private:
        const QueryResult* result_;
        std::size_t index_;
    };

    Iterator begin() const { return Iterator{*result_, 0}; }
    Iterator end() const { return Iterator{*result_, size()}; }

    std::size_t size() const noexcept { return result_->rows.size(); }
    bool empty() const noexcept { return size() == 0; }
    Row operator[](std::size_t index) const { return Row{*result_, index}; }

    /// The number of rows a write changed.
    std::int64_t affected() const noexcept { return result_->affected; }

private:
    std::shared_ptr<const QueryResult> result_;
};

}  // namespace cppboot::data
