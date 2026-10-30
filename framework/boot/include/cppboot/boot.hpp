#pragma once

#include <functional>

#include "cppboot/cppboot.hpp"
#include "cppboot/di/container.hpp"

namespace cppboot {

using ComponentRegistrar = std::function<void(di::Container&)>;

/// Runs an application whose components are in a container, and returns its exit code.
///
/// This creates the container, calls register_components, validates the result and adds every
/// component in the cppboot::Module group to the application. Generated code provides the
/// registrar as cppboot::generated::registerComponents.
int runApplication(int argc, char* argv[], const ComponentRegistrar& register_components,
                   ApplicationOptions options = {});

}  // namespace cppboot
