#include "cppboot/cli/cli.hpp"

#include <filesystem>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "build.hpp"
#include "cppboot/base/version.hpp"
#include "cppboot/platform/process.hpp"
#include "dev.hpp"
#include "doctor.hpp"
#include "generators.hpp"
#include "package.hpp"
#include "prompts.hpp"
#include "toolchain.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage = R"(Usage: cppboot <command> [arguments]

Commands:
  create [name]     Create a new project in a new folder. With no name, it asks what you want
                    Add --web for a web API with a sample controller
                    Add --sqlite or --postgresql with --web to store its data in a database
                    Add --security with --web for sign-in and protected routes
                    Add --actuator for health and metrics routes
  add <kind> <Name> Add a class to the project's src folder, where kind is one of: service,
                    controller, repository, middleware, module, health, config
                    A repository also gets a migration that creates its table
  build             Build the project in the current folder
  run [arguments]   Build and run the project, passing arguments to the application
  dev [arguments]   Build and run the project, and do it again whenever you save a change
  test [arguments]  Build and run the tests in tests/, passing arguments such as
                    --gtest_filter=Order* to them
                    Put --sanitize=address or --sanitize=thread first on build, run or test to
                    find memory errors or data races, on Linux
  package           Build a release into dist/, with everything needed to run it elsewhere
                    On Linux, this includes a systemd service and a script that installs it
  doctor            Check that everything CppBoot needs is installed, and say how to fix what isn't
                    Add --full to also build and run a sample project
  info              Show version and installation details
  help              Show this message
)";

void printInfo(std::ostream& out) {
    out << "CppBoot " << kVersionString << '\n';

    if (auto executable = platform::executablePath()) {
        out << "Executable: " << executable.value().generic_string() << '\n';
    }

    if (auto prefix = findInstallation()) {
        out << "Installation: " << prefix.value().generic_string() << '\n';
    } else {
        out << "Installation: not found\n";
    }

    if (auto name = readProjectName(fs::current_path())) {
        out << "Project: " << name.value() << '\n';
    } else {
        out << "Project: none in this folder\n";
    }
}

int usage(std::ostream& err) {
    err << kUsage;
    return kExitUsage;
}

/// The exit code of a command, after reporting its error, if it had one.
int finish(const Result<int>& result, std::ostream& err) {
    if (!result) {
        err << "cppboot: " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}

int create(const std::vector<std::string>& arguments, std::istream& in, std::ostream& out,
           std::ostream& err) {
    // With no arguments at all, it asks.
    if (arguments.empty()) {
        auto answers = askForProject(in, out);
        if (!answers) {
            err << "cppboot: no answers to read. Give the name instead: cppboot create <name>\n";
            return kExitUsage;
        }
        if (auto created = createProject(answers->first, fs::current_path(), answers->second);
            !created) {
            return finish(created.error(), err);
        }
        printNextSteps(out, answers->first, answers->second);
        return 0;
    }

    std::vector<std::string> names;
    ProjectOptions options;
    bool sqlite = false;
    bool postgresql = false;
    for (const std::string& argument : arguments) {
        if (argument == "--web") {
            options.web = true;
        } else if (argument == "--sqlite") {
            sqlite = true;
        } else if (argument == "--postgresql") {
            postgresql = true;
        } else if (argument == "--security") {
            options.security = true;
        } else if (argument == "--actuator") {
            options.actuator = true;
        } else {
            names.push_back(argument);
        }
    }
    if (names.size() != 1 || (sqlite && postgresql)) {
        return usage(err);
    }
    if ((sqlite || postgresql || options.security) && !options.web) {
        err << "cppboot: --sqlite, --postgresql and --security need --web, because their "
               "samples are web APIs\n";
        return kExitUsage;
    }
    if (sqlite) {
        options.database = ProjectDatabase::Sqlite;
    } else if (postgresql) {
        options.database = ProjectDatabase::Postgresql;
    }

    if (auto created = createProject(names.front(), fs::current_path(), options); !created) {
        return finish(created.error(), err);
    }
    printNextSteps(out, names.front(), options);
    return 0;
}

int add(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
    if (arguments.size() != 2) {
        return usage(err);
    }
    auto added = addComponent(fs::current_path(), arguments[0], arguments[1]);
    if (!added) {
        return finish(added.error(), err);
    }
    for (const fs::path& file : added.value()) {
        out << "Created " << file.generic_string() << '\n';
    }
    out << "The next build finds it, with no registration needed.\n";
    return 0;
}

/// build, run and test, which all take --sanitize= first.
int buildRunOrTest(const std::string& command, std::vector<std::string> arguments,
                   std::ostream& err) {
    const auto sanitizer = takeSanitizer(arguments);
    if (!sanitizer || (command == "build" && !arguments.empty())) {
        return usage(err);
    }
    if (command == "build") {
        return finish(buildProject(fs::current_path(), *sanitizer), err);
    }
    if (command == "run") {
        return finish(runProject(fs::current_path(), std::move(arguments), *sanitizer), err);
    }
    if (!arguments.empty() && arguments.front() == "--") {
        arguments.erase(arguments.begin());
    }
    return finish(testProject(fs::current_path(), arguments, *sanitizer), err);
}

int package(std::ostream& out, std::ostream& err) {
    auto packaged = packageProject(fs::current_path());
    if (!packaged) {
        return finish(packaged.error(), err);
    }
    out << "Packaged " << packaged.value().generic_string() << '\n';
    return 0;
}

}  // namespace

int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
    // Never destroyed, because cppboot dev reads its input on a thread that it leaves running.
    static auto* const nothing = new std::istringstream;
    return run(arguments, *nothing, out, err);
}

int run(const std::vector<std::string>& arguments, std::istream& in, std::ostream& out,
        std::ostream& err) {
    if (arguments.empty()) {
        return usage(err);
    }

    const std::string& command = arguments.front();
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    applyToolchain();
    // Commands that start other programs flush first, so that output stays in order.
    out.flush();

    if (command == "help" || command == "--help" || command == "-h") {
        out << kUsage;
        return 0;
    }
    if (command == "info" && rest.empty()) {
        printInfo(out);
        return 0;
    }
    if (command == "doctor" && (rest.empty() || (rest.size() == 1 && rest.front() == "--full"))) {
        return runDoctor(out, !rest.empty());
    }
    if (command == "create") {
        return create(rest, in, out, err);
    }
    if (command == "add") {
        return add(rest, out, err);
    }
    if (command == "build" || command == "run" || command == "test") {
        return buildRunOrTest(command, rest, err);
    }
    if (command == "dev") {
        return finish(developProject(fs::current_path(), rest, in, out), err);
    }
    if (command == "package" && rest.empty()) {
        return package(out, err);
    }
    return usage(err);
}

}  // namespace cppboot::cli
