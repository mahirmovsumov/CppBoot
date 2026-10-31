#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cppboot/codegen/model.hpp"
#include "cppboot/codegen/parser.hpp"

namespace {

namespace fs = std::filesystem;
using namespace cppboot;

constexpr std::string_view kUsage =
    "Usage: cppboot-codegen --output <file> [--actuator] [<header>...] [-- <compiler arguments>...]\n"
    "       cppboot-codegen --print-resource-dir\n";

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

    if (auto valid = codegen::validateDeclarations(declarations, options); !valid) {
        return valid;
    }

    std::error_code ignored;
    if (output.has_parent_path()) {
        fs::create_directories(output.parent_path(), ignored);
    }

    std::ofstream file{output, std::ios::binary};
    file << codegen::renderRegistration(declarations, includes, options);
    file.close();
    if (!file) {
        return Error{ErrorCode::IoError, "Cannot write " + output.generic_string()};
    }
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    fs::path output;
    std::vector<fs::path> headers;
    std::vector<std::string> compiler_arguments;
    codegen::RenderOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--") {
            compiler_arguments.assign(argv + i + 1, argv + argc);
            break;
        }
        if (argument == "--print-resource-dir") {
            std::cout << codegen::clangResourceDirectory() << '\n';
            return 0;
        }
        if (argument == "--actuator") {
            options.actuator = true;
        } else if (argument == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else {
            headers.emplace_back(argument);
        }
    }

    if (output.empty()) {
        std::cerr << kUsage;
        return 2;
    }

    if (auto generated = generate(output, headers, compiler_arguments, options); !generated) {
        std::cerr << "cppboot-codegen: " << generated.error().message() << '\n';
        return 1;
    }
    return 0;
}
