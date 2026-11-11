#include "dev.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <system_error>
#include <thread>
#include <utility>

#include "build.hpp"
#include "cppboot/cli/cli.hpp"
#include "cppboot/platform/process.hpp"
#include "cppboot/platform/signals.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

/// When each watched file last changed, and its size, which also catches a change within the same
/// clock tick.
using FileStates = std::map<fs::path, std::pair<fs::file_time_type, std::uintmax_t>>;

/// The files that change what cppboot dev builds or runs: src, db, CMakeLists.txt and the settings.
FileStates watchedFiles(const fs::path& directory) {
    FileStates files;
    const auto add = [&](const fs::path& path) {
        std::error_code error;
        const auto time = fs::last_write_time(path, error);
        const auto size = error ? 0 : fs::file_size(path, error);
        if (!error) {
            files[path] = {time, size};
        }
    };

    for (const char* folder : {"src", "db"}) {
        std::error_code error;
        for (fs::recursive_directory_iterator entry{directory / folder, error}, end;
             !error && entry != end; entry.increment(error)) {
            std::error_code type_error;
            if (entry->is_regular_file(type_error)) {
                add(entry->path());
            }
        }
    }
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, error}) {
        const std::string file = entry.path().filename().string();
        if (file == "CMakeLists.txt" ||
            (entry.path().extension() == ".yaml" && file.starts_with("cppboot"))) {
            add(entry.path());
        }
    }
    return files;
}

/// What someone typed while cppboot dev runs. A thread reads it, because reading waits.
struct DevCommands {
    std::atomic<bool> restart{false};
    std::atomic<bool> quit{false};
};

std::shared_ptr<DevCommands> readDevCommands(std::istream& in) {
    auto commands = std::make_shared<DevCommands>();
    // It's left running at exit, still waiting for a line, so it holds only what outlives it.
    std::thread{[commands, &in] {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line == "q" || line == "quit") {
                commands->quit = true;
                return;
            }
            if (line == "r" || line.empty()) {
                commands->restart = true;
            }
        }
    }}.detach();
    return commands;
}

}  // namespace

Result<int> developProject(const fs::path& directory, const std::vector<std::string>& arguments,
                           std::istream& in, std::ostream& out) {
    using namespace std::chrono_literals;
    auto name = readProjectName(directory);
    if (!name) {
        return name.error();
    }
    auto signal = platform::ShutdownSignal::install();
    if (!signal) {
        return signal.error();
    }
    const auto commands = readDevCommands(in);
    const auto stopping = [&] { return signal.value().requested() || commands->quit.load(); };
    const auto say = [&](std::string_view message) { out << "cppboot dev: " << message << std::endl; };

    std::vector<std::string> command{
        (directory / "build" / platform::executableFileName(name.value())).string()};
    const auto first = !arguments.empty() && arguments.front() == "--" ? 1 : 0;
    command.insert(command.end(), arguments.begin() + first, arguments.end());

    std::optional<platform::ChildProcess> application;
    FileStates seen;
    bool rebuild = true;
    while (!stopping()) {
        if (rebuild) {
            rebuild = false;
            // On Windows, a running program can't be replaced, so it stops before the build.
            if (application) {
                application->stop(10s);
                application.reset();
            }
            // Taken before the build, so that a change made during it starts another one.
            seen = watchedFiles(directory);
            // Input goes to the thread that reads r and q, so the programs started here get none.
            auto built = buildProject(directory, Sanitizer::None, {}, Input::None);
            if (stopping()) {
                break;
            }
            if (!built) {
                say(built.error().message());
            } else if (built.value() != 0) {
                say("the build failed. Fix it and save, and it builds again.");
            } else if (auto started = platform::ChildProcess::start({command, directory, false});
                       !started) {
                say(started.error().message());
            } else {
                application = std::move(started).value();
                say("running. It restarts when you save a change. Type r and Enter to restart now, or "
                    "press Ctrl+C to stop.");
            }
        }

        std::this_thread::sleep_for(250ms);
        if (application) {
            if (const auto code = application->exitCode()) {
                say("the application stopped, with exit code " + std::to_string(*code) +
                    ". It starts again when you save a change.");
                application.reset();
            }
        }

        FileStates now = watchedFiles(directory);
        if (now != seen) {
            // Editors often save in more than one step, so this waits until the files settle.
            do {
                seen = std::move(now);
                std::this_thread::sleep_for(200ms);
                now = watchedFiles(directory);
            } while (now != seen && !stopping());
            say("a file changed, so it's building again");
            rebuild = true;
        }
        if (commands->restart.exchange(false)) {
            say("restarting");
            rebuild = true;
        }
    }

    if (application) {
        say("stopping the application");
        application->stop(10s);
    }
    return 0;
}

}  // namespace cppboot::cli
