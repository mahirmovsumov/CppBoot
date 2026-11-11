#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <utility>

#include "cppboot/cli/cli.hpp"

namespace cppboot::cli {

/// Asks for what cppboot create would otherwise take as arguments. Returns nothing at the end of
/// input.
std::optional<std::pair<std::string, ProjectOptions>> askForProject(std::istream& in,
                                                                     std::ostream& out);

/// Says what to do after creating a project, including the variables a secured one needs.
void printNextSteps(std::ostream& out, const std::string& name, const ProjectOptions& options);

}  // namespace cppboot::cli
