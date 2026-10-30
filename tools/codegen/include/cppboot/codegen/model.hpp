#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::codegen {

/// A component with this interface is added to a group rather than bound to the interface, since
/// an application has many modules.
inline constexpr std::string_view kModuleInterface = "cppboot::Module";

/// Types that cppboot::runApplication() provides, so components can depend on them without any
/// component declaring them.
inline constexpr std::array<std::string_view, 2> kFrameworkTypes = {
    "cppboot::config::Config",
    "cppboot::logging::Logger",
};

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
