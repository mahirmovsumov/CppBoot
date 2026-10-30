#include <iostream>
#include <string>
#include <vector>

#include "cppboot/cli/cli.hpp"

int main(int argc, char* argv[]) {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    return cppboot::cli::run(arguments, std::cout, std::cerr);
}
