#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cppboot/codegen/model.hpp"
#include "cppboot/codegen/parser.hpp"
#include "cppboot/codegen/scan.hpp"

namespace {

namespace fs = std::filesystem;
using namespace cppboot;

constexpr std::string_view kUsage =
    "Usage: cppboot-codegen --output <file> [--actuator] [<header>...] [-- <compiler arguments>...]\n"
    "       cppboot-codegen --scan <header> --output <file> [--depfile <file>] [-- <arguments>...]\n"
    "       cppboot-codegen --merge --output <file> [--actuator] [<scan>...]\n"
    "       cppboot-codegen --print-resource-dir\n"
    "\n"
    "--scan reads one header and saves what it found, and --merge combines saved scans into the\n"
    "generated header. A build does this so that only changed headers are read again.\n";

Result<void> writeFile(const fs::path& path, const std::string& content) {
    std::error_code ignored;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ignored);
    }
    std::ofstream file{path, std::ios::binary};
    file << content;
    file.close();
    if (!file) {
        return Error{ErrorCode::IoError, "Cannot write " + path.generic_string()};
    }
    return {};
}

Result<std::string> readFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::IoError, "Cannot read " + path.generic_string()};
    }
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void printWarnings(const codegen::Declarations& declarations) {
    for (const std::string& warning : declarations.warnings) {
        std::cerr << warning << '\n';
    }
}

/// Checks the declarations from every header together, then writes the generated header.
Result<void> render(const fs::path& output, const codegen::Declarations& declarations,
                    const std::vector<std::string>& includes, codegen::RenderOptions options) {
    if (auto valid = codegen::validateDeclarations(declarations, options); !valid) {
        return valid;
    }
    return writeFile(output, codegen::renderRegistration(declarations, includes, options));
}

Result<void> generate(const fs::path& output, const std::vector<fs::path>& headers,
                      const std::vector<std::string>& compiler_arguments,
                      codegen::RenderOptions options) {
    codegen::Declarations declarations;
    std::vector<std::string> includes;

    for (const fs::path& header : headers) {
        auto found = codegen::parseDeclarations(header, compiler_arguments);
        if (!found) {
            return found.error();
        }
        declarations.append(std::move(found).value());
        includes.push_back(fs::absolute(header).generic_string());
    }

    printWarnings(declarations);
    return render(output, declarations, includes, options);
}

Result<void> scan(const fs::path& header, const fs::path& output, const fs::path& depfile,
                  const std::vector<std::string>& compiler_arguments) {
    std::vector<fs::path> included_files;
    auto found = codegen::parseDeclarations(header, compiler_arguments,
                                            depfile.empty() ? nullptr : &included_files);
    if (!found) {
        return found.error();
    }

    // Warnings print here, where the header is read, rather than on every build.
    printWarnings(found.value());

    const codegen::ScannedHeader scanned{fs::absolute(header).generic_string(),
                                         std::move(found).value()};
    if (auto written = writeFile(output, codegen::toJson(scanned)); !written) {
        return written;
    }
    if (!depfile.empty()) {
        return writeFile(depfile, codegen::makeDepfile(output, included_files));
    }
    return {};
}

Result<void> merge(const fs::path& output, const std::vector<fs::path>& scans,
                   codegen::RenderOptions options) {
    codegen::Declarations declarations;
    std::vector<std::string> includes;

    for (const fs::path& path : scans) {
        auto text = readFile(path);
        if (!text) {
            return text.error();
        }
        auto scanned = codegen::scannedHeaderFromJson(text.value());
        if (!scanned) {
            return scanned.error();
        }
        includes.push_back(scanned.value().header);
        declarations.append(std::move(scanned.value().declarations));
    }
    return render(output, declarations, includes, options);
}

}  // namespace

int main(int argc, char* argv[]) {
    fs::path output;
    fs::path scan_header;
    fs::path depfile;
    bool merging = false;
    std::vector<fs::path> inputs;
    std::vector<std::string> compiler_arguments;
    codegen::RenderOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--") {
            compiler_arguments = codegen::usableCompilerArguments(
                std::vector<std::string>(argv + i + 1, argv + argc));
            break;
        }
        if (argument == "--print-resource-dir") {
            std::cout << codegen::clangResourceDirectory() << '\n';
            return 0;
        }
        if (argument == "--actuator") {
            options.actuator = true;
        } else if (argument == "--merge") {
            merging = true;
        } else if (argument == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (argument == "--scan" && i + 1 < argc) {
            scan_header = argv[++i];
        } else if (argument == "--depfile" && i + 1 < argc) {
            depfile = argv[++i];
        } else {
            inputs.emplace_back(argument);
        }
    }

    if (output.empty() || (merging && !scan_header.empty())) {
        std::cerr << kUsage;
        return 2;
    }

    Result<void> done{};
    if (!scan_header.empty()) {
        done = scan(scan_header, output, depfile, compiler_arguments);
    } else if (merging) {
        done = merge(output, inputs, options);
    } else {
        done = generate(output, inputs, compiler_arguments, options);
    }

    if (!done) {
        std::cerr << "cppboot-codegen: " << done.error().message() << '\n';
        return 1;
    }
    return 0;
}
