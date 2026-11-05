#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <cppboot/platform/signals.hpp>

// Usage: child <report-file> <exit-code> [arguments...]
// Writes its working directory and arguments to the report file, then exits with the given code.
//
// Usage: child <report-file> wait
// Waits for a shutdown request, such as Ctrl+C, then writes "stopped" to the report file and exits
// with 7.
int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 100;
    }

    const std::string_view text{argv[2]};
    if (text == "wait") {
        auto signal = cppboot::platform::ShutdownSignal::install();
        if (!signal) {
            return 102;
        }
        std::ofstream{argv[1], std::ios::binary} << "waiting\n";
        signal.value().wait();
        std::ofstream{argv[1], std::ios::binary} << "stopped\n";
        return 7;
    }

    std::ofstream report{argv[1], std::ios::binary};
    report << std::filesystem::current_path().string() << '\n';
    for (int i = 3; i < argc; ++i) {
        report << argv[i] << '\n';
    }

    int exit_code = 101;
    std::from_chars(text.data(), text.data() + text.size(), exit_code);
    return exit_code;
}
