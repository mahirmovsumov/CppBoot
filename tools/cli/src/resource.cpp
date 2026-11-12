#include "resource.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <regex>
#include <system_error>

#include "source_files.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMostFields = 40;
constexpr std::size_t kLongestFieldName = 40;

// A field named like one of these wouldn't compile, or would need quoting in SQL.
constexpr std::string_view kReservedWords[] = {
    // C++
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "continue",
    "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "else",
    "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr",
    "operator", "or", "private", "protected", "public", "register", "requires", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try",
    "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while",
    "xor",
    // SQL
    "all", "by", "check", "column", "from", "group", "index", "key", "limit", "offset", "order",
    "select", "table", "user", "where",
};

struct TypeInfo {
    std::string_view written;
    FieldType type;
};

constexpr std::array<TypeInfo, 6> kTypes = {{
    {"string", FieldType::Text},
    {"int", FieldType::Int},
    {"long", FieldType::Long},
    {"double", FieldType::Double},
    {"bool", FieldType::Bool},
    {"text", FieldType::Text},
}};

bool isFieldName(std::string_view name) {
    return !name.empty() && name.size() <= kLongestFieldName && name.front() >= 'a' &&
           name.front() <= 'z' &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
           });
}

std::string cppType(const ResourceField& field) {
    std::string type;
    switch (field.type) {
        case FieldType::Text: type = "std::string"; break;
        case FieldType::Int: type = "int"; break;
        case FieldType::Long: type = "std::int64_t"; break;
        case FieldType::Double: type = "double"; break;
        case FieldType::Bool: type = "bool"; break;
    }
    return field.optional ? "std::optional<" + type + ">" : type;
}

/// What a field starts as, so that a struct is never left with an unset number.
std::string initializer(const ResourceField& field) {
    if (field.optional) {
        return "";
    }
    switch (field.type) {
        case FieldType::Text: return "";
        case FieldType::Bool: return "{false}";
        default: return "{0}";
    }
}

std::string sqlType(const ResourceField& field, bool postgresql) {
    std::string type;
    switch (field.type) {
        case FieldType::Text: type = "TEXT"; break;
        case FieldType::Int: type = "INTEGER"; break;
        case FieldType::Long: type = "BIGINT"; break;
        case FieldType::Double: type = postgresql ? "DOUBLE PRECISION" : "REAL"; break;
        case FieldType::Bool: type = "BOOLEAN"; break;
    }
    return field.optional ? type : type + " NOT NULL";
}

/// A value of the field in C++, different for each version so a test can tell them apart.
std::string sampleValue(const ResourceField& field, int version) {
    const std::string number = std::to_string(version);
    switch (field.type) {
        case FieldType::Text: return "\"" + field.name + " " + number + "\"";
        case FieldType::Int:
        case FieldType::Long: return number;
        case FieldType::Double: return number + ".5";
        case FieldType::Bool: return version % 2 == 1 ? "true" : "false";
    }
    return "{}";
}

std::string sampleJson(const ResourceField& field) {
    switch (field.type) {
        case FieldType::Text: return "\\\"x\\\"";
        case FieldType::Int:
        case FieldType::Long: return "1";
        case FieldType::Double: return "1.5";
        case FieldType::Bool: return "true";
    }
    return "null";
}

/// The assertion that a read value equals the sample of that version.
std::string expectSample(const ResourceField& field, const std::string& object, int version) {
    const std::string actual = object + "." + field.name;
    const std::string expected = sampleValue(field, version);
    if (field.type == FieldType::Double) {
        return field.optional ? "ASSERT_TRUE(" + actual + ");\n        EXPECT_DOUBLE_EQ(*" + actual +
                                    ", " + expected + ");"
                              : "EXPECT_DOUBLE_EQ(" + actual + ", " + expected + ");";
    }
    return "EXPECT_EQ(" + actual + ", " + expected + ");";
}

/// Whether a header in src declares a class or struct with this name.
bool declaresType(const fs::path& project, const std::string& type) {
    const std::regex declaration{R"((class|struct)\s[^;{]*\b)" + type + R"(\s*(final\s*)?[{:])"};
    std::error_code error;
    for (fs::recursive_directory_iterator entry{project / "src", error}, end; !error && entry != end;
         entry.increment(error)) {
        if (entry->path().extension() == ".hpp" &&
            std::regex_search(readFile(entry->path()), declaration)) {
            return true;
        }
    }
    return false;
}

struct Names {
    /// Product, ProductInput, ProductRepository and ProductController.
    std::string type;
    std::string input;
    std::string repository;
    std::string controller;
    /// order_item, as in order_item.hpp.
    std::string snake;
    /// order item, for messages.
    std::string words;
    std::string table;
    std::string path;
};

std::string columnList(const std::vector<ResourceField>& fields, bool with_id) {
    std::string list = with_id ? "id" : "";
    for (const ResourceField& field : fields) {
        list += list.empty() ? "" : ", ";
        list += field.name;
    }
    return list;
}

std::string renderHeader(const Names& names, const std::vector<ResourceField>& fields,
                         const std::string& migration) {
    const std::string columns = columnList(fields, true);
    std::string placeholders;
    std::string assignments;
    std::string arguments;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        placeholders += (i == 0 ? "$" : ", $") + std::to_string(i + 1);
        assignments += (i == 0 ? "" : ", ") + fields[i].name + " = $" + std::to_string(i + 2);
        arguments += ", input." + fields[i].name;
    }

    std::string text = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

)";
    text += "// A " + names.words + " as clients see it, and as a row of the " + names.table +
            " table, which\n// " + migration + " creates. Fields match columns by name.\n";
    text += "struct CPPBOOT_JSON CPPBOOT_ROW " + names.type + " {\n    std::int64_t id{0};\n";
    for (const ResourceField& field : fields) {
        text += "    " + cppType(field) + " " + field.name + initializer(field) + ";\n";
    }
    text += "};\n\n";

    text += "// What a client sends to create or replace a " + names.words +
            ". A request that fails a check gets a\n// 400 naming the field, before the "
            "controller runs.\n";
    text += "struct CPPBOOT_JSON " + names.input + " {\n";
    for (const ResourceField& field : fields) {
        std::string marks;
        if (field.type == FieldType::Text) {
            marks = field.optional ? "CPPBOOT_LENGTH(0, 255) " : "CPPBOOT_LENGTH(1, 255) ";
        }
        text += "    " + marks + cppType(field) + " " + field.name + initializer(field) + ";\n";
    }
    text += "};\n\n";

    text += "// Values go in as $1, $2 and so on, never as part of the SQL text, which prevents SQL "
            "injection.\n";
    text += "class CPPBOOT_SERVICE " + names.repository + " {\npublic:\n";
    text += "    explicit " + names.repository +
            "(cppboot::data::Database& database) : database_(database) {}\n\n";
    text += "    std::vector<" + names.type + "> list(std::int64_t limit, std::int64_t offset) {\n";
    text += "        return database_.queryAs<" + names.type + ">(\n            \"SELECT " + columns +
            " FROM " + names.table + " ORDER BY id LIMIT $1 OFFSET $2\", limit, offset);\n    }\n\n";
    text += "    std::optional<" + names.type + "> find(std::int64_t id) {\n";
    text += "        return database_.queryOne<" + names.type + ">(\"SELECT " + columns + " FROM " +
            names.table + " WHERE id = $1\", id);\n    }\n\n";
    text += "    " + names.type + " add(const " + names.input + "& input) {\n";
    text += "        return *database_.queryOne<" + names.type + ">(\n            \"INSERT INTO " +
            names.table + " (" + columnList(fields, false) + ") VALUES (" + placeholders +
            ") RETURNING " + columns + "\"" + arguments + ");\n    }\n\n";
    text += "    /// Returns nothing if there's no " + names.words + " with that ID.\n";
    text += "    std::optional<" + names.type + "> replace(std::int64_t id, const " + names.input +
            "& input) {\n";
    text += "        return database_.queryOne<" + names.type + ">(\n            \"UPDATE " +
            names.table + " SET " + assignments + " WHERE id = $1 RETURNING " + columns +
            "\",\n            id" + arguments + ");\n    }\n\n";
    text += "    /// Returns false if there was no " + names.words + " with that ID.\n";
    text += "    bool remove(std::int64_t id) {\n        return database_.execute(\"DELETE FROM " +
            names.table + " WHERE id = $1\", id) > 0;\n    }\n\n";
    text += "private:\n    cppboot::data::Database& database_;\n};\n\n";

    const std::string missing =
        "throw cppboot::web::HttpError(404, \"No " + names.words + " has that ID\");";
    text += "class CPPBOOT_CONTROLLER(\"" + names.path + "\") " + names.controller + " {\npublic:\n";
    text += "    explicit " + names.controller + "(" + names.repository + "& " + names.table +
            ") : " + names.table + "_(" + names.table + ") {}\n\n";
    text += "    // GET " + names.path + "?limit=20&offset=40\n";
    text += "    CPPBOOT_GET(\"/\") std::vector<" + names.type +
            "> list(CPPBOOT_RANGE(1, 100) std::optional<std::int64_t> limit,\n"
            "                                    CPPBOOT_MIN(0) std::optional<std::int64_t> offset) {\n";
    text += "        return " + names.table + "_.list(limit.value_or(20), offset.value_or(0));\n    }\n\n";
    text += "    CPPBOOT_GET(\"/{id}\") " + names.type + " get(std::int64_t id) {\n";
    text += "        auto found = " + names.table + "_.find(id);\n        if (!found) {\n            " +
            missing + "\n        }\n        return *found;\n    }\n\n";
    text += "    CPPBOOT_POST(\"/\") cppboot::web::WithStatus<" + names.type + "> create(const " +
            names.input + "& input) {\n";
    text += "        return {201, " + names.table + "_.add(input)};\n    }\n\n";
    text += "    CPPBOOT_PUT(\"/{id}\") " + names.type + " replace(std::int64_t id, const " +
            names.input + "& input) {\n";
    text += "        auto replaced = " + names.table + "_.replace(id, input);\n        if (!replaced) {\n"
            "            " + missing + "\n        }\n        return *replaced;\n    }\n\n";
    text += "    // Answers 204 No Content.\n";
    text += "    CPPBOOT_DELETE(\"/{id}\") void remove(std::int64_t id) {\n";
    text += "        if (!" + names.table + "_.remove(id)) {\n            " + missing +
            "\n        }\n    }\n\n";
    text += "private:\n    " + names.repository + "& " + names.table + "_;\n};\n";
    return text;
}

std::string renderMigration(const Names& names, const std::vector<ResourceField>& fields,
                            bool postgresql) {
    std::string sql = "CREATE TABLE " + names.table + " (\n    id " +
                      (postgresql ? "BIGSERIAL PRIMARY KEY" : "INTEGER PRIMARY KEY");
    for (const ResourceField& field : fields) {
        sql += ",\n    " + field.name + " " + sqlType(field, postgresql);
    }
    return sql + "\n);\n";
}

std::string renderTest(const Names& names, const std::vector<ResourceField>& fields,
                       const std::string& header, bool postgresql) {
    const std::string fixture = names.repository + "Test";
    const std::string sample = "sample" + names.type;
    const std::string member = names.table + "_";

    std::string text = R"(#include <gtest/gtest.h>

#include <chrono>
#include <cppboot/cppboot.hpp>
#include <cppboot/data/migrations.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/platform/environment.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "cppboot_components.generated.hpp"

namespace {

)";
    text += names.input + " " + sample + "() {\n    " + names.input + " input;\n";
    for (const ResourceField& field : fields) {
        text += "    input." + field.name + " = " + sampleValue(field, 1) + ";\n";
    }
    text += "    return input;\n}\n\n}  // namespace\n\n";

    text += "// Runs " + names.repository + " against a real database, with the project's migrations "
            "applied.\n";
    text += postgresql ? "// It needs CPPBOOT_TEST_DATABASE_URL to name a PostgreSQL database that "
                         "the tests may change,\n// and is skipped without it.\n"
                       : "// Each test gets a new SQLite file.\n";
    text += "class " + fixture + " : public ::testing::Test {\nprotected:\n";
    text += "    void SetUp() override {\n";
    text += "        const auto environment = cppboot::platform::environmentVariables();\n";
    if (postgresql) {
        text += "        const auto url = environment.find(\"CPPBOOT_TEST_DATABASE_URL\");\n";
        text += "        if (url == environment.end()) {\n            GTEST_SKIP() << \"Set "
                "CPPBOOT_TEST_DATABASE_URL to a PostgreSQL database these tests may change\";\n"
                "        }\n";
        text += "        const std::string database_url = url->second;\n\n";
    } else {
        text += "        database_file_ = std::filesystem::temp_directory_path() /\n"
                "                         (\"cppboot-" + names.snake + "-test-\" +\n"
                "                          std::to_string(std::chrono::steady_clock::now()."
                "time_since_epoch().count()) +\n"
                "                          \".db\");\n";
        text += "        const std::string database_url = \"sqlite:\" + "
                "database_file_.generic_string();\n\n";
    }
    text += R"(        // The settings the application would read, with the test database in place of its own.
        cppboot::config::ConfigSources sources;
        sources.environment = environment;
        sources.arguments = {"--cppboot.database.url=" + database_url};
        auto config = cppboot::config::Config::load(sources);
        ASSERT_TRUE(config) << config.error().message();
        auto logger = cppboot::logging::Logger::create({"test", cppboot::logging::Level::Warn, {}});
        ASSERT_TRUE(logger) << logger.error().message();
        const std::string migrations =
            config.value().getString("cppboot.database.migrations", "db/migrations");

        container_ = std::make_unique<cppboot::di::Container>();
        container_->registerInstance(
            std::make_shared<cppboot::config::Config>(std::move(config).value()));
        container_->registerInstance(
            std::make_shared<cppboot::logging::Logger>(std::move(logger).value()));
        cppboot::generated::registerComponents(*container_);

        const auto migrated =
            cppboot::data::runMigrations(*container_->resolve<cppboot::data::Database>(), migrations);
        ASSERT_TRUE(migrated) << migrated.error().message();
)";
    text += "        " + member + " = container_->resolve<" + names.repository + ">();\n    }\n\n";
    text += "    void TearDown() override {\n        " + member + ".reset();\n";
    text += "        // Closes the database before its file goes.\n        container_.reset();\n";
    if (!postgresql) {
        text += "        std::error_code ignored;\n";
        text += "        for (const char* suffix : {\"\", \"-wal\", \"-shm\"}) {\n"
                "            std::filesystem::remove(database_file_.string() + suffix, ignored);\n"
                "        }\n";
    }
    text += "    }\n\n";
    if (!postgresql) {
        text += "    std::filesystem::path database_file_;\n";
    }
    text += "    std::unique_ptr<cppboot::di::Container> container_;\n";
    text += "    std::shared_ptr<" + names.repository + "> " + member + ";\n};\n\n";

    text += "TEST_F(" + fixture + ", AddsFindsReplacesAndRemoves) {\n";
    text += "    const " + names.type + " added = " + member + "->add(" + sample + "());\n";
    text += "    EXPECT_GT(added.id, 0);\n";
    for (const ResourceField& field : fields) {
        text += "    " + expectSample(field, "added", 1) + "\n";
    }
    text += "\n    const auto found = " + member + "->find(added.id);\n    ASSERT_TRUE(found);\n";
    for (const ResourceField& field : fields) {
        text += "    " + expectSample(field, "(*found)", 1) + "\n";
    }
    text += "\n    " + names.input + " changed;\n";
    for (const ResourceField& field : fields) {
        text += field.optional ? "    // Empty, which has to be stored as NULL.\n"
                               : "    changed." + field.name + " = " + sampleValue(field, 2) + ";\n";
    }
    text += "    const auto replaced = " + member + "->replace(added.id, changed);\n";
    text += "    ASSERT_TRUE(replaced);\n";
    for (const ResourceField& field : fields) {
        text += field.optional ? "    EXPECT_FALSE(replaced->" + field.name + ");\n"
                               : "    " + expectSample(field, "(*replaced)", 2) + "\n";
    }
    text += "\n    EXPECT_TRUE(" + member + "->remove(added.id));\n";
    text += "    EXPECT_FALSE(" + member + "->find(added.id));\n";
    text += "    EXPECT_FALSE(" + member + "->remove(added.id));\n";
    text += "    EXPECT_FALSE(" + member + "->replace(added.id, changed));\n}\n\n";

    text += "TEST_F(" + fixture + ", ListsInPagesInIdOrder) {\n";
    text += "    for (int i = 0; i < 3; ++i) {\n        " + member + "->add(" + sample + "());\n    }\n";
    text += "    const auto first = " + member + "->list(2, 0);\n";
    text += "    ASSERT_EQ(first.size(), 2U);\n    EXPECT_LT(first[0].id, first[1].id);\n";
    text += "    const auto next = " + member + "->list(2, 1);\n";
    text += "    ASSERT_FALSE(next.empty());\n    EXPECT_EQ(next[0].id, first[1].id);\n}\n";

    // A test of the checks, when there's a required text field to leave empty.
    const auto checked = std::find_if(fields.begin(), fields.end(), [](const ResourceField& field) {
        return field.type == FieldType::Text && !field.optional;
    });
    if (checked != fields.end()) {
        std::string valid;
        std::string empty;
        for (const ResourceField& field : fields) {
            if (field.optional) {
                continue;
            }
            const std::string entry = "\\\"" + field.name + "\\\":";
            valid += (valid.empty() ? "" : ",") + entry + sampleJson(field);
            empty += (empty.empty() ? "" : ",") + entry +
                     (field.name == checked->name ? "\\\"\\\"" : sampleJson(field));
        }
        text += "\n// The checks on " + names.input + " in " + header +
                " run wherever it's read from JSON.\n";
        text += "TEST(" + names.input + "Test, ChecksWhatClientsSend) {\n";
        text += "    EXPECT_NO_THROW(static_cast<void>(\n        cppboot::Json::parse(\"{" + valid +
                "}\").get<" + names.input + ">()));\n";
        text += "    EXPECT_THROW(static_cast<void>(\n        cppboot::Json::parse(\"{" + empty +
                "}\").get<" + names.input + ">()),\n                 cppboot::JsonError);\n}\n";
    }
    return text;
}

}  // namespace

Result<std::vector<ResourceField>> parseResourceFields(const std::vector<std::string>& specs) {
    if (specs.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "A resource needs at least one field, such as name:string or price:double"};
    }
    if (specs.size() > kMostFields) {
        return Error{ErrorCode::InvalidArgument,
                     "A resource can have at most " + std::to_string(kMostFields) + " fields"};
    }

    std::vector<ResourceField> fields;
    for (const std::string& spec : specs) {
        const auto colon = spec.find(':');
        if (colon == std::string::npos) {
            return Error{ErrorCode::InvalidArgument,
                         "'" + spec + "' needs a type, such as " + spec + ":string"};
        }
        ResourceField field;
        field.name = spec.substr(0, colon);
        std::string_view type = std::string_view{spec}.substr(colon + 1);
        if (type.ends_with('?')) {
            field.optional = true;
            type.remove_suffix(1);
        }

        if (!isFieldName(field.name)) {
            return Error{ErrorCode::InvalidArgument,
                         "'" + field.name + "' isn't a field name. Use lower-case letters, digits "
                                            "and _, starting with a letter, such as unit_price"};
        }
        if (field.name == "id") {
            return Error{ErrorCode::InvalidArgument,
                         "Every resource has an id already, so leave it out of the fields"};
        }
        if (std::find(std::begin(kReservedWords), std::end(kReservedWords), field.name) !=
            std::end(kReservedWords)) {
            return Error{ErrorCode::InvalidArgument,
                         "'" + field.name + "' is a word that C++ or SQL reserves. Choose another "
                                            "name, such as " + field.name + "_name"};
        }
        if (std::any_of(fields.begin(), fields.end(),
                        [&](const ResourceField& other) { return other.name == field.name; })) {
            return Error{ErrorCode::InvalidArgument, "The field '" + field.name + "' is listed twice"};
        }

        const auto known = std::find_if(kTypes.begin(), kTypes.end(),
                                        [&](const TypeInfo& info) { return info.written == type; });
        if (known == kTypes.end()) {
            return Error{ErrorCode::InvalidArgument,
                         "'" + std::string{type} + "' in '" + spec + "' isn't a type. Use string, "
                         "int, long, double or bool, with ? after it for a field that can be missing"};
        }
        field.type = known->type;
        fields.push_back(std::move(field));
    }
    return fields;
}

Result<std::vector<fs::path>> addResource(const fs::path& project, std::string_view name,
                                          const std::vector<std::string>& field_specs) {
    if (!isClassName(name)) {
        return Error{ErrorCode::InvalidArgument,
                     "'" + std::string{name} + "' isn't a class name. Use letters and digits, "
                                               "starting with a capital, such as Product"};
    }
    auto fields = parseResourceFields(field_specs);
    if (!fields) {
        return fields.error();
    }
    if (auto found = checkProject(project); !found) {
        return found.error();
    }
    const std::string cmake = readFile(project / "CMakeLists.txt");
    if (cmake.find("cppboot::web") == std::string::npos ||
        cmake.find("cppboot::data") == std::string::npos) {
        return Error{ErrorCode::InvalidArgument,
                     "A resource needs a web project with a database, made with --web and "
                     "--sqlite or --postgresql"};
    }

    Names names;
    names.type = std::string{name};
    names.input = names.type + "Input";
    names.repository = names.type + "Repository";
    names.controller = names.type + "Controller";
    names.snake = snakeCase(name);
    names.words = replaceAll(names.snake, "_", " ");
    names.table = plural(names.snake);
    names.path = "/api/" + replaceAll(names.table, "_", "-");

    const fs::path header = fs::path{"src"} / (names.snake + ".hpp");
    const fs::path test = fs::path{"tests"} / (names.snake + "_test.cpp");
    std::error_code error;
    for (const fs::path& file : {header, test}) {
        if (fs::exists(project / file, error)) {
            return Error{ErrorCode::InvalidState, file.generic_string() + " already exists"};
        }
    }
    for (const std::string& type : {names.type, names.input, names.repository, names.controller}) {
        if (declaresType(project, type)) {
            return Error{ErrorCode::InvalidState,
                         "A header in src already declares " + type + ". Choose another name"};
        }
    }
    auto migration = newTableMigration(project, names.table);
    if (!migration) {
        return migration.error();
    }

    const bool postgresql = usesPostgresql(project);
    const std::vector<std::pair<fs::path, std::string>> files = {
        {header, renderHeader(names, fields.value(), migration.value().generic_string())},
        {migration.value(), renderMigration(names, fields.value(), postgresql)},
        {test, renderTest(names, fields.value(), header.generic_string(), postgresql)},
    };
    std::vector<fs::path> written;
    for (const auto& [path, content] : files) {
        if (auto saved = writeNew(project / path, content); !saved) {
            return saved.error();
        }
        written.push_back(path);
    }
    return written;
}

}  // namespace cppboot::cli
