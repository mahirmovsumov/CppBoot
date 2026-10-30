#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot {

class ApplicationContext;

/// A unit of functionality with its own startup and shutdown.
class Module {
public:
    virtual ~Module() = default;

    /// A unique name. Other modules use it to declare a dependency on this module.
    virtual std::string_view name() const noexcept = 0;

    /// Names of the modules that must start before this one.
    virtual std::vector<std::string> dependencies() const { return {}; }

    virtual Result<void> start(ApplicationContext& context) = 0;

    /// Called only if start() succeeded. Modules stop in the reverse of their start order.
    virtual void stop() noexcept {}
};

/// Validates module dependencies and runs startup and shutdown in dependency order.
class ModuleManager {
public:
    void add(std::unique_ptr<Module> instance);

    /// Fails on a null module, a duplicate name, a missing dependency or a dependency cycle.
    /// Modules with no ordering constraint between them keep their registration order.
    Result<void> resolveOrder();

    /// Stops at the first failure. Modules that already started stay started until stopAll().
    Result<void> startAll(ApplicationContext& context);

    /// Stops every started module. Calling it again has no effect.
    void stopAll() noexcept;

    std::vector<std::string> orderedNames() const;

private:
    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<Module*> order_;
    std::size_t started_{0};
};

}  // namespace cppboot
