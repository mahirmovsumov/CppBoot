#include "cppboot/data/row.hpp"

#include <algorithm>

namespace cppboot::data {

namespace detail {

void throwConversionError(std::string_view text, std::string_view column,
                          std::string_view expected) {
    throw DatabaseError(DatabaseError::Kind::Mapping,
                        "Column '" + std::string{column} + "' has value '" + std::string{text} +
                            "', which isn't " + std::string{expected});
}

bool parseBool(std::string_view text, std::string_view column) {
    // PostgreSQL returns t or f, and SQLite stores booleans as 1 or 0.
    if (text == "t" || text == "true" || text == "1") {
        return true;
    }
    if (text == "f" || text == "false" || text == "0") {
        return false;
    }
    throwConversionError(text, column, "a boolean");
}

}  // namespace detail

std::size_t Row::indexOf(std::string_view column) const {
    const auto found = std::find(result_->columns.begin(), result_->columns.end(), column);
    if (found == result_->columns.end()) {
        throw DatabaseError(DatabaseError::Kind::Mapping,
                            "The result has no column named '" + std::string{column} + "'");
    }
    return static_cast<std::size_t>(found - result_->columns.begin());
}

std::string_view Row::nameOf(std::size_t column) const {
    if (column >= result_->columns.size()) {
        throw DatabaseError(DatabaseError::Kind::Mapping,
                            "The result has no column " + std::to_string(column));
    }
    return result_->columns[column];
}

const std::optional<std::string>& Row::valueAt(std::size_t column) const {
    if (index_ >= result_->rows.size() || column >= result_->rows[index_].size()) {
        throw DatabaseError(DatabaseError::Kind::Mapping,
                            "The result has no column " + std::to_string(column));
    }
    return result_->rows[index_][column];
}

}  // namespace cppboot::data
