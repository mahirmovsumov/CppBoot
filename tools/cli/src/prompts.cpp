#include "prompts.hpp"

#include <cctype>
#include <filesystem>
#include <functional>
#include <istream>
#include <ostream>
#include <system_error>

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

/// Asks a question until it gets an answer that check accepts. Returns nothing at the end of input.
std::optional<std::string> ask(std::istream& in, std::ostream& out, std::string_view question,
                               std::string_view fallback,
                               const std::function<bool(const std::string&)>& check) {
    while (true) {
        out << question << (fallback.empty() ? "" : " [" + std::string{fallback} + "]") << ": "
            << std::flush;
        std::string answer;
        if (!std::getline(in, answer)) {
            out << '\n';
            return std::nullopt;
        }
        while (!answer.empty() && std::isspace(static_cast<unsigned char>(answer.back())) != 0) {
            answer.pop_back();
        }
        answer.erase(0, answer.find_first_not_of(" \t"));
        if (answer.empty()) {
            answer = fallback;
        }
        if (check(answer)) {
            return answer;
        }
    }
}

std::optional<bool> askYesNo(std::istream& in, std::ostream& out, std::string_view question) {
    const auto answer = ask(in, out, std::string{question} + " (y/n)", "n", [&](const std::string& text) {
        const bool known = text == "y" || text == "yes" || text == "n" || text == "no";
        if (!known) {
            out << "  Answer y or n.\n";
        }
        return known;
    });
    if (!answer) {
        return std::nullopt;
    }
    return answer->front() == 'y';
}

}  // namespace

std::optional<std::pair<std::string, ProjectOptions>> askForProject(std::istream& in,
                                                                     std::ostream& out) {
    const auto name = ask(in, out, "Project name", "", [&](const std::string& text) {
        auto valid = validateProjectName(text);
        if (!valid) {
            out << "  " << valid.error().message() << '\n';
            return false;
        }
        std::error_code ignored;
        if (fs::exists(fs::current_path() / text, ignored)) {
            out << "  A folder with that name already exists here.\n";
            return false;
        }
        return true;
    });
    if (!name) {
        return std::nullopt;
    }

    ProjectOptions options;
    const auto web = askYesNo(in, out, "A web API, with a sample controller?");
    if (!web) {
        return std::nullopt;
    }
    options.web = *web;
    if (options.web) {
        const auto database = ask(in, out, "Database: none, sqlite or postgresql?", "none",
                                  [&](const std::string& text) {
                                      const bool known = text == "none" || text == "sqlite" ||
                                                         text == "postgresql";
                                      if (!known) {
                                          out << "  Answer none, sqlite or postgresql.\n";
                                      }
                                      return known;
                                  });
        if (!database) {
            return std::nullopt;
        }
        options.database = *database == "sqlite"       ? ProjectDatabase::Sqlite
                           : *database == "postgresql" ? ProjectDatabase::Postgresql
                                                       : ProjectDatabase::None;
        const auto security = askYesNo(in, out, "Sign-in and protected routes?");
        if (!security) {
            return std::nullopt;
        }
        options.security = *security;
    }
    const auto actuator = askYesNo(in, out, "Health and metrics routes?");
    if (!actuator) {
        return std::nullopt;
    }
    options.actuator = *actuator;
    return std::pair{*name, options};
}

void printNextSteps(std::ostream& out, const std::string& name, const ProjectOptions& options) {
    out << "Created " << name << ". Next:\n  cd " << name << '\n';
    if (options.security) {
#if defined(_WIN32)
        out << "  $env:CPPBOOT_SECURITY_JWT_SECRET = '<at least 32 random characters>'\n"
               "  $env:CPPBOOT_MYAPP_PASSWORD = '<a password for the sample account>'\n";
#else
        out << "  export CPPBOOT_SECURITY_JWT_SECRET='<at least 32 random characters>'\n"
               "  export CPPBOOT_MYAPP_PASSWORD='<a password for the sample account>'\n";
#endif
    }
    out << "  cppboot run\n";
}

}  // namespace cppboot::cli
