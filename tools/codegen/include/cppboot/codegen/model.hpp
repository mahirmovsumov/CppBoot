#pragma once

#include <string>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::codegen {

/// A class marked for registration. Type names are fully qualified.
struct Component {
    std::string type;
    /// Where the class is declared, as "file:line", for error messages.
    std::string location;
    /// The abstract base classes that other components can depend on instead of this type.
    std::vector<std::string> interfaces;
    /// The component types its constructor needs, in parameter order.
    std::vector<std::string> dependencies;
};

/// Fails if a dependency has no provider, a type has two providers, or dependencies form a cycle.
Result<void> validateComponents(const std::vector<Component>& components);

/// Returns a header defining cppboot::generated::registerComponents(), which registers each
/// component with a cppboot::di::Container.
std::string renderRegistration(const std::vector<Component>& components,
                               const std::vector<std::string>& headers);

}  // namespace cppboot::codegen
