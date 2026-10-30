#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

// Usage: child <report-file> <exit-code> [arguments...]
// Writes its working directory and arguments to the report file, then exits with the given code.
int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 100;
    }

    std::ofstream report{argv[1], std::ios::binary};
    report << std::filesystem::current_path().string() << '\n';
    for (int i = 3; i < argc; ++i) {
        report << argv[i] << '\n';
    }

    const std::string_view text{argv[2]};
    int exit_code = 101;
    std::from_chars(text.data(), text.data() + text.size(), exit_code);
    return exit_code;
}
