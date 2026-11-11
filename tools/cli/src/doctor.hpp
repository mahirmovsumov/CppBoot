#pragma once

#include <iosfwd>

namespace cppboot::cli {

/// Checks the installation, tools and libraries, and says how to fix each problem. With full, it
/// also builds and runs a sample project. Returns 0 if there are no problems.
int runDoctor(std::ostream& out, bool full);

}  // namespace cppboot::cli
