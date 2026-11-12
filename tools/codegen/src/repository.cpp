#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include "cppboot/codegen/model.hpp"

namespace cppboot::codegen {

namespace {

using Words = std::vector<std::string>;

/// Splits a name at its capitals: findByUnitPrice becomes find, By, Unit, Price. A run of digits
/// stays with the word before it, so Top10 is one word.
Words camelWords(std::string_view name) {
    Words words;
    for (const char c : name) {
        if (std::isupper(static_cast<unsigned char>(c)) != 0 || words.empty()) {
            words.emplace_back();
        }
        words.back() += c;
    }
    return words;
}

/// unit_price becomes Unit, Price, as it would be written in a function name.
Words fieldWords(std::string_view field) {
    Words words;
    bool start = true;
    for (const char c : field) {
        if (c == '_') {
            start = true;
            continue;
        }
        if (start) {
            words.emplace_back(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            start = false;
        } else {
            words.back() += c;
        }
    }
    return words;
}

std::string join(const Words& words, std::size_t from, std::size_t to) {
    std::string text;
    for (std::size_t i = from; i < to; ++i) {
        text += words[i];
    }
    return text;
}

enum class Operator {
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    Between,
    Like,
    NotLike,
    StartsWith,
    EndsWith,
    Contains,
    IsNull,
    IsNotNull,
    IsTrue,
    IsFalse,
};

struct OperatorSpelling {
    std::vector<std::string_view> words;
    Operator op;
};

// Longer spellings first, so that LessThanEqual isn't read as LessThan followed by Equal.
const std::vector<OperatorSpelling>& operatorSpellings() {
    static const std::vector<OperatorSpelling> spellings = {
        {{"Is", "Not", "Null"}, Operator::IsNotNull},
        {{"Less", "Than", "Equal"}, Operator::LessOrEqual},
        {{"Greater", "Than", "Equal"}, Operator::GreaterOrEqual},
        {{"Not", "Null"}, Operator::IsNotNull},
        {{"Is", "Null"}, Operator::IsNull},
        {{"Is", "Not"}, Operator::NotEqual},
        {{"Less", "Than"}, Operator::Less},
        {{"Greater", "Than"}, Operator::Greater},
        {{"Not", "Like"}, Operator::NotLike},
        {{"Starting", "With"}, Operator::StartsWith},
        {{"Starts", "With"}, Operator::StartsWith},
        {{"Ending", "With"}, Operator::EndsWith},
        {{"Ends", "With"}, Operator::EndsWith},
        {{"Is", "True"}, Operator::IsTrue},
        {{"Is", "False"}, Operator::IsFalse},
        {{"Null"}, Operator::IsNull},
        {{"Not"}, Operator::NotEqual},
        {{"Is"}, Operator::Equal},
        {{"Equals"}, Operator::Equal},
        {{"Before"}, Operator::Less},
        {{"After"}, Operator::Greater},
        {{"Between"}, Operator::Between},
        {{"Like"}, Operator::Like},
        {{"Containing"}, Operator::Contains},
        {{"Contains"}, Operator::Contains},
        {{"True"}, Operator::IsTrue},
        {{"False"}, Operator::IsFalse},
    };
    return spellings;
}

std::size_t parameterCount(Operator op) {
    switch (op) {
        case Operator::Between: return 2;
        case Operator::IsNull:
        case Operator::IsNotNull:
        case Operator::IsTrue:
        case Operator::IsFalse: return 0;
        default: return 1;
    }
}

bool isTextMatch(Operator op) {
    return op == Operator::Like || op == Operator::NotLike || op == Operator::StartsWith ||
           op == Operator::EndsWith || op == Operator::Contains;
}

std::string_view kindName(ValueKind kind) {
    switch (kind) {
        case ValueKind::Number: return "a number";
        case ValueKind::Text: return "text";
        case ValueKind::Bool: return "a bool";
        case ValueKind::Page: return "a page";
        case ValueKind::Other: break;
    }
    return "another type";
}

/// Builds the query, keeping the function it's for, so every error can name it.
class QueryBuilder {
public:
    QueryBuilder(const RepositoryInterface& repository, const RepositoryFunction& function,
                 const MappedStruct& row)
        : repository_(repository), function_(function), row_(row) {}

    Result<RepositoryQuery> derive();

private:
    Error fail(const std::string& message) const {
        return Error{ErrorCode::DependencyError, function_.location + ": " + repository_.type +
                                                     "::" + function_.name + " " + message};
    }

    /// The field whose words start at words[position], taking the longest that fits.
    const StructField* matchField(const Words& words, std::size_t position, std::size_t end,
                                  std::size_t& length) const;

    /// Takes the next parameter, which has to suit the field.
    std::optional<Error> takeParameter(const StructField& field, Operator op, std::string& placeholder);

    std::optional<Error> readCriteria(const Words& words, std::size_t from, std::size_t to,
                                      std::string& sql);
    std::optional<Error> readOrder(const Words& words, std::size_t from, std::string& sql) const;

    std::string columns() const {
        std::string list;
        for (const StructField& field : row_.fields) {
            list += list.empty() ? "" : ", ";
            list += field.name;
        }
        return list;
    }

    const RepositoryInterface& repository_;
    const RepositoryFunction& function_;
    const MappedStruct& row_;
    std::size_t next_parameter_{0};
    RepositoryQuery query_;
};

const StructField* QueryBuilder::matchField(const Words& words, std::size_t position,
                                            std::size_t end, std::size_t& length) const {
    const StructField* best = nullptr;
    length = 0;
    for (const StructField& field : row_.fields) {
        const Words name = fieldWords(field.name);
        if (name.size() <= length || position + name.size() > end) {
            continue;
        }
        if (std::equal(name.begin(), name.end(), words.begin() + static_cast<std::ptrdiff_t>(position))) {
            best = &field;
            length = name.size();
        }
    }
    return best;
}

std::optional<Error> QueryBuilder::takeParameter(const StructField& field, Operator op,
                                                 std::string& placeholder) {
    if (next_parameter_ >= function_.parameters.size() ||
        function_.parameters[next_parameter_].kind == ValueKind::Page) {
        return fail("needs a parameter for " + field.name + ", but it has too few");
    }
    const RepositoryParameter& parameter = function_.parameters[next_parameter_++];
    if (parameter.kind == ValueKind::Other) {
        return fail("has parameter '" + parameter.name + "', which isn't text, a number or a bool. "
                    "Parameters can't be a std::optional: to find a missing value, use IsNull");
    }
    if (field.kind != ValueKind::Other && parameter.kind != field.kind) {
        return fail("compares " + field.name + ", which is " + std::string{kindName(field.kind)} +
                    ", with parameter '" + parameter.name + "', which is " +
                    std::string{kindName(parameter.kind)});
    }

    query_.arguments.push_back(parameter.name);
    placeholder = "$" + std::to_string(query_.arguments.size());
    if (op == Operator::StartsWith || op == Operator::EndsWith || op == Operator::Contains) {
        const char* match = op == Operator::StartsWith ? "StartsWith"
                            : op == Operator::EndsWith ? "EndsWith"
                                                       : "Contains";
        query_.arguments.back() = "cppboot::data::likePattern(" + parameter.name +
                                  ", cppboot::data::TextMatch::" + match + ")";
    }
    return std::nullopt;
}

std::optional<Error> QueryBuilder::readCriteria(const Words& words, std::size_t from,
                                                std::size_t to, std::string& sql) {
    std::size_t position = from;
    while (position < to) {
        std::size_t length = 0;
        const StructField* field = matchField(words, position, to, length);
        if (field == nullptr) {
            std::string fields;
            for (const StructField& candidate : row_.fields) {
                fields += fields.empty() ? "" : ", ";
                fields += candidate.name;
            }
            return fail("refers to " + words[position] + ", which isn't a field of " +
                        row_.type + ". Its fields are " + fields);
        }
        position += length;

        // The words up to the next And or Or are the operator, and maybe IgnoreCase.
        std::size_t clause_end = position;
        while (clause_end < to && words[clause_end] != "And" && words[clause_end] != "Or") {
            ++clause_end;
        }
        bool ignore_case = false;
        std::size_t operator_end = clause_end;
        if (operator_end - position >= 2 && words[operator_end - 1] == "Case" &&
            (words[operator_end - 2] == "Ignore" || words[operator_end - 2] == "Ignoring")) {
            ignore_case = true;
            operator_end -= 2;
        }

        Operator op = Operator::Equal;
        if (operator_end > position) {
            const auto spelled = std::find_if(
                operatorSpellings().begin(), operatorSpellings().end(),
                [&](const OperatorSpelling& spelling) {
                    return spelling.words.size() == operator_end - position &&
                           std::equal(spelling.words.begin(), spelling.words.end(),
                                      words.begin() + static_cast<std::ptrdiff_t>(position));
                });
            if (spelled == operatorSpellings().end()) {
                const std::string written = join(words, position, operator_end);
                if (written == "In" || written == "NotIn") {
                    return fail("uses " + written + ", which isn't supported yet. Use CPPBOOT_QUERY");
                }
                return fail("has '" + written + "' after " + field->name + ", which isn't a "
                            "comparison it knows. Use one such as LessThan, Between, Containing or "
                            "IsNull");
            }
            op = spelled->op;
        }

        if (isTextMatch(op) && field->kind != ValueKind::Text && field->kind != ValueKind::Other) {
            return fail("matches text in " + field->name + ", which isn't text");
        }
        if ((op == Operator::IsTrue || op == Operator::IsFalse) && field->kind != ValueKind::Bool &&
            field->kind != ValueKind::Other) {
            return fail("checks whether " + field->name + " is true or false, but it isn't a bool");
        }
        if (ignore_case &&
            (field->kind != ValueKind::Text ||
             !(op == Operator::Equal || op == Operator::NotEqual || isTextMatch(op)))) {
            return fail("ignores case for " + field->name + ", which only works when comparing or "
                        "matching text");
        }

        const std::string column =
            ignore_case ? "LOWER(" + field->name + ")" : field->name;
        const auto value = [&](const std::string& placeholder) {
            return ignore_case ? "LOWER(" + placeholder + ")" : placeholder;
        };

        std::string first;
        std::string second;
        if (parameterCount(op) >= 1) {
            if (auto error = takeParameter(*field, op, first)) {
                return error;
            }
        }
        if (parameterCount(op) == 2) {
            if (auto error = takeParameter(*field, op, second)) {
                return error;
            }
        }

        switch (op) {
            case Operator::Equal: sql += column + " = " + value(first); break;
            case Operator::NotEqual: sql += column + " <> " + value(first); break;
            case Operator::Less: sql += column + " < " + first; break;
            case Operator::LessOrEqual: sql += column + " <= " + first; break;
            case Operator::Greater: sql += column + " > " + first; break;
            case Operator::GreaterOrEqual: sql += column + " >= " + first; break;
            case Operator::Between: sql += column + " BETWEEN " + first + " AND " + second; break;
            case Operator::Like: sql += column + " LIKE " + value(first); break;
            case Operator::NotLike: sql += column + " NOT LIKE " + value(first); break;
            case Operator::StartsWith:
            case Operator::EndsWith:
            case Operator::Contains:
                sql += column + " LIKE " + value(first) + " ESCAPE '\\'";
                break;
            case Operator::IsNull: sql += column + " IS NULL"; break;
            case Operator::IsNotNull: sql += column + " IS NOT NULL"; break;
            case Operator::IsTrue: sql += column + " = TRUE"; break;
            case Operator::IsFalse: sql += column + " = FALSE"; break;
        }

        position = clause_end;
        if (position < to) {
            if (position + 1 >= to) {
                return fail("ends with " + words[position] + ", with nothing after it");
            }
            sql += words[position] == "And" ? " AND " : " OR ";
            ++position;
        }
    }
    return std::nullopt;
}

std::optional<Error> QueryBuilder::readOrder(const Words& words, std::size_t from,
                                             std::string& sql) const {
    if (from >= words.size()) {
        return fail("ends with OrderBy, with no field after it");
    }
    std::string order;
    std::size_t position = from;
    while (position < words.size()) {
        std::size_t length = 0;
        const StructField* field = matchField(words, position, words.size(), length);
        if (field == nullptr) {
            return fail("orders by " + words[position] + ", which isn't a field of " + row_.type);
        }
        position += length;
        std::string direction = " ASC";
        if (position < words.size() && (words[position] == "Asc" || words[position] == "Desc")) {
            direction = words[position] == "Desc" ? " DESC" : " ASC";
            ++position;
        }
        order += (order.empty() ? "" : ", ") + field->name + direction;
    }
    sql += " ORDER BY " + order;
    return std::nullopt;
}

Result<RepositoryQuery> QueryBuilder::derive() {
    const Words words = camelWords(function_.name);
    const auto by = std::find(words.begin(), words.end(), "By");
    const std::string subject = join(words, 0, static_cast<std::size_t>(by - words.begin()));

    // What the function does, from the words before By.
    enum class Action { Find, Count, Exists, Delete } action = Action::Find;
    std::optional<std::int64_t> limit;
    bool limited_to_one = false;
    if (subject == "find" || subject == "findAll" || subject == "get" || subject == "read" ||
        subject == "query") {
        action = Action::Find;
    } else if (subject.starts_with("findFirst") || subject.starts_with("findTop")) {
        const std::string_view count =
            std::string_view{subject}.substr(subject.starts_with("findFirst") ? 9 : 7);
        if (count.empty()) {
            limited_to_one = true;
        } else {
            std::int64_t value = 0;
            const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), value);
            if (error != std::errc{} || end != count.data() + count.size() || value < 1) {
                return fail("has '" + subject + "', but the number after First or Top has to be "
                            "a whole number from 1");
            }
            limit = value;
        }
    } else if (subject == "count") {
        action = Action::Count;
    } else if (subject == "exists") {
        action = Action::Exists;
    } else if (subject == "delete" || subject == "remove") {
        action = Action::Delete;
    } else {
        return fail("can't be written from its name. Start it with findBy, findFirstBy, countBy, "
                    "existsBy or deleteBy, call it findAll or count, or give it its SQL with "
                    "CPPBOOT_QUERY");
    }

    if (by == words.end() && !(subject == "findAll" || subject == "count")) {
        return fail("needs By and what to find by, such as " + subject + "ByName");
    }
    if (action == Action::Delete && by + 1 >= words.end()) {
        // Deleting every row is too easy to do by mistake from a name.
        return fail("would delete every row. Give it a condition, or use CPPBOOT_QUERY if that's "
                    "what you mean");
    }

    // The words between By and OrderBy are the conditions.
    const std::size_t criteria_start =
        by == words.end() ? words.size() : static_cast<std::size_t>(by - words.begin()) + 1;
    std::size_t criteria_end = words.size();
    std::optional<std::size_t> order_start;
    for (std::size_t i = criteria_start; i + 1 < words.size(); ++i) {
        if (words[i] == "Order" && words[i + 1] == "By") {
            criteria_end = i;
            order_start = i + 2;
            break;
        }
    }

    const bool has_page = !function_.parameters.empty() &&
                          function_.parameters.back().kind == ValueKind::Page;
    if (std::any_of(function_.parameters.begin(), function_.parameters.end() - (has_page ? 1 : 0),
                    [](const RepositoryParameter& parameter) {
                        return parameter.kind == ValueKind::Page;
                    })) {
        return fail("has a cppboot::data::Page that isn't its last parameter");
    }

    // The return type has to fit the action.
    const auto returns = [&](RepositoryReturn expected) { return function_.returns == expected; };
    const bool returns_rows = returns(RepositoryReturn::List) || returns(RepositoryReturn::Optional);
    if (returns_rows && function_.element_type != row_.type) {
        return fail("returns " + function_.element_type + ", but " + repository_.type +
                    " holds " + row_.type);
    }
    switch (action) {
        case Action::Find:
            if (limited_to_one && !returns(RepositoryReturn::Optional)) {
                return fail("finds one row, so it has to return std::optional<" + row_.type + ">");
            }
            if (limit && !returns(RepositoryReturn::List)) {
                return fail("finds up to " + std::to_string(*limit) +
                            " rows, so it has to return std::vector<" + row_.type + ">");
            }
            if (!returns_rows) {
                return fail("finds rows, so it has to return std::vector<" + row_.type +
                            "> or std::optional<" + row_.type + ">");
            }
            if (has_page && !(returns(RepositoryReturn::List) && !limit)) {
                return fail("takes a cppboot::data::Page, which only works when returning a "
                            "std::vector with no First or Top");
            }
            break;
        case Action::Count:
            if (!returns(RepositoryReturn::Integer)) {
                return fail("counts rows, so it has to return std::int64_t");
            }
            break;
        case Action::Exists:
            if (!returns(RepositoryReturn::Bool)) {
                return fail("checks whether rows exist, so it has to return bool");
            }
            break;
        case Action::Delete:
            if (!returns(RepositoryReturn::Integer) && !returns(RepositoryReturn::Void)) {
                return fail("deletes rows, so it has to return std::int64_t, the number it "
                            "deleted, or void");
            }
            break;
    }
    if (has_page && action != Action::Find) {
        return fail("takes a cppboot::data::Page, which only works when finding rows");
    }
    if (order_start && action != Action::Find) {
        return fail("has OrderBy, which only changes anything when finding rows");
    }

    std::string sql;
    switch (action) {
        case Action::Find: sql = "SELECT " + columns() + " FROM " + repository_.table; break;
        case Action::Count: sql = "SELECT COUNT(*) FROM " + repository_.table; break;
        case Action::Exists: sql = "SELECT 1 FROM " + repository_.table; break;
        case Action::Delete: sql = "DELETE FROM " + repository_.table; break;
    }
    if (criteria_end > criteria_start) {
        sql += " WHERE ";
        if (auto error = readCriteria(words, criteria_start, criteria_end, sql)) {
            return *std::move(error);
        }
    }
    const std::size_t used = next_parameter_ + (has_page ? 1 : 0);
    if (used != function_.parameters.size()) {
        return fail("has " + std::to_string(function_.parameters.size()) +
                    " parameters, but its name uses " + std::to_string(next_parameter_));
    }
    if (order_start) {
        if (auto error = readOrder(words, *order_start, sql)) {
            return *std::move(error);
        }
    }

    switch (action) {
        case Action::Find:
            if (limited_to_one) {
                sql += " LIMIT 1";
                query_.run = QueryRun::FirstRow;
            } else if (limit) {
                sql += " LIMIT " + std::to_string(*limit);
                query_.run = QueryRun::Rows;
            } else if (returns(RepositoryReturn::Optional)) {
                // Two rows, so that finding more than one can be reported.
                sql += " LIMIT 2";
                query_.run = QueryRun::OneRow;
            } else {
                query_.run = QueryRun::Rows;
            }
            break;
        case Action::Count: query_.run = QueryRun::Value; break;
        case Action::Exists:
            sql += " LIMIT 1";
            query_.run = QueryRun::Exists;
            break;
        case Action::Delete: query_.run = QueryRun::Change; break;
    }
    if (has_page) {
        const std::string page = function_.parameters.back().name;
        query_.arguments.push_back(page + ".limit");
        query_.arguments.push_back(page + ".offset");
        sql += " LIMIT $" + std::to_string(query_.arguments.size() - 1) + " OFFSET $" +
               std::to_string(query_.arguments.size());
    }
    query_.sql = std::move(sql);
    return std::move(query_);
}

/// The highest $n in a statement, skipping text in quotes.
std::size_t highestPlaceholder(std::string_view sql) {
    std::size_t highest = 0;
    char quote = 0;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (quote != 0) {
            quote = c == quote ? 0 : quote;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '$') {
            std::size_t value = 0;
            const auto [end, error] = std::from_chars(sql.data() + i + 1, sql.data() + sql.size(), value);
            if (error == std::errc{}) {
                highest = std::max(highest, value);
            }
        }
    }
    return highest;
}

bool isSelect(std::string_view sql) {
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front())) != 0) {
        sql.remove_prefix(1);
    }
    std::string first;
    for (const char c : sql.substr(0, 6)) {
        first += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return first.starts_with("SELECT") || first.starts_with("WITH");
}

Result<RepositoryQuery> explicitQuery(const RepositoryInterface& repository,
                                      const RepositoryFunction& function,
                                      const Declarations& declarations) {
    const auto fail = [&](const std::string& message) {
        return Error{ErrorCode::DependencyError, function.location + ": " + repository.type +
                                                     "::" + function.name + " " + message};
    };

    RepositoryQuery query;
    query.sql = function.query;
    for (const RepositoryParameter& parameter : function.parameters) {
        if (parameter.kind == ValueKind::Page || parameter.kind == ValueKind::Other) {
            return fail("has parameter '" + parameter.name + "', which isn't text, a number or a "
                        "bool. With CPPBOOT_QUERY, write LIMIT and OFFSET in the SQL");
        }
        query.arguments.push_back(parameter.name);
    }
    if (highestPlaceholder(query.sql) != function.parameters.size()) {
        return fail("has " + std::to_string(function.parameters.size()) +
                    " parameters, but its SQL goes up to $" +
                    std::to_string(highestPlaceholder(query.sql)));
    }

    const bool is_row = std::any_of(
        declarations.row_types.begin(), declarations.row_types.end(),
        [&](const MappedStruct& row) { return row.type == function.element_type; });
    switch (function.returns) {
        case RepositoryReturn::List:
            if (!is_row) {
                return fail("returns a std::vector of " + function.element_type +
                            ", which isn't marked CPPBOOT_ROW");
            }
            query.run = QueryRun::Rows;
            break;
        case RepositoryReturn::Optional:
            if (!is_row && function.element_kind == ValueKind::Other) {
                return fail("returns a std::optional of " + function.element_type +
                            ", which isn't marked CPPBOOT_ROW, and isn't text, a number or a bool");
            }
            query.run = is_row ? QueryRun::FirstRow : QueryRun::Value;
            break;
        case RepositoryReturn::Integer:
        case RepositoryReturn::Bool:
            // A SELECT gives a value, such as a COUNT. Anything else changes rows.
            query.run = isSelect(query.sql) ? QueryRun::Value : QueryRun::Change;
            if (query.run == QueryRun::Change && function.returns == RepositoryReturn::Bool) {
                return fail("returns bool, which only works for a SELECT");
            }
            break;
        case RepositoryReturn::Void:
            query.run = QueryRun::Change;
            break;
        case RepositoryReturn::Other:
            return fail("returns a type that a query can't give. Use std::vector or std::optional "
                        "of a CPPBOOT_ROW struct, std::optional of a value, std::int64_t, bool or "
                        "void");
    }
    return query;
}

}  // namespace

Result<RepositoryQuery> repositoryQuery(const RepositoryInterface& repository,
                                        const RepositoryFunction& function,
                                        const Declarations& declarations) {
    if (!function.query.empty()) {
        return explicitQuery(repository, function, declarations);
    }
    const auto row = std::find_if(
        declarations.row_types.begin(), declarations.row_types.end(),
        [&](const MappedStruct& candidate) { return candidate.type == repository.row_type; });
    if (row == declarations.row_types.end()) {
        return Error{ErrorCode::DependencyError,
                     repository.location + ": " + repository.type + " holds " + repository.row_type +
                         ", which isn't marked CPPBOOT_ROW"};
    }
    return QueryBuilder{repository, function, *row}.derive();
}

}  // namespace cppboot::codegen
