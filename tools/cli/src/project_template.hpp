#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cppboot/cli/cli.hpp"

namespace cppboot::cli {

struct RenderedFile {
    /// Relative to the project folder, with / between folders.
    std::string_view path;
    std::string content;
};

/// The files of a new project with these options, with the name and settings filled in.
std::vector<RenderedFile> renderProjectFiles(std::string_view name, const ProjectOptions& options);

}  // namespace cppboot::cli
