#include "source_files.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace cppboot::cli {

namespace fs = std::filesystem;

namespace {

/// The highest number that starts a file name in the folder, such as 2 for 002_create_users.sql.
int highestMigrationNumber(const fs::path& folder) {
    int highest = 0;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{folder, error}) {
        const std::string file = entry.path().filename().string();
        int number = 0;
        const auto [end, parse_error] = std::from_chars(file.data(), file.data() + file.size(), number);
        if (parse_error == std::errc{} && end != file.data()) {
            highest = std::max(highest, number);
        }
    }
    return highest;
}

}  // namespace

std::string readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::stringstream text;
    text << input.rdbuf();
    return text.str();
}

Result<void> writeNew(const fs::path& path, const std::string& content) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary};
    output << content;
    output.close();
    if (!output) {
        return Error{ErrorCode::IoError, "Cannot write " + path.generic_string()};
    }
    return {};
}

std::string replaceAll(std::string text, std::string_view placeholder, std::string_view value) {
    std::size_t position = 0;
    while ((position = text.find(placeholder, position)) != std::string::npos) {
        text.replace(position, placeholder.size(), value);
        position += value.size();
    }
    return text;
}

std::string snakeCase(std::string_view name) {
    std::string result;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c)) != 0) {
            if (i > 0) {
                result += '_';
            }
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            result += c;
        }
    }
    return result;
}

std::string upperCase(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string plural(const std::string& word) {
    return word.ends_with('s') ? word : word + "s";
}

bool isClassName(std::string_view name) {
    return !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0 &&
           std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

Result<void> checkProject(const fs::path& project) {
    std::error_code error;
    if (!fs::exists(project / "CMakeLists.txt", error) || !fs::exists(project / "src", error)) {
        return Error{ErrorCode::NotFound,
                     "No CppBoot project here. Run this from a folder made by 'cppboot create'."};
    }
    return {};
}

bool usesPostgresql(const fs::path& project) {
    return readFile(project / "cppboot.yaml").find("postgresql:") != std::string::npos;
}

bool createsTable(const fs::path& folder, const std::string& table) {
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{folder, error}) {
        std::string text = readFile(entry.path());
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (text.find("create table " + table + " ") != std::string::npos ||
            text.find("create table " + table + "(") != std::string::npos ||
            text.find("create table if not exists " + table + " ") != std::string::npos) {
            return true;
        }
    }
    return false;
}

Result<fs::path> newTableMigration(const fs::path& project, const std::string& table) {
    const fs::path folder = project / "db" / "migrations";
    if (createsTable(folder, table)) {
        return Error{ErrorCode::InvalidState,
                     "A migration in db/migrations already creates the " + table + " table"};
    }
    char number[16];
    std::snprintf(number, sizeof(number), "%03d", highestMigrationNumber(folder) + 1);
    return fs::path{"db"} / "migrations" / (std::string{number} + "_create_" + table + ".sql");
}

}  // namespace cppboot::cli
