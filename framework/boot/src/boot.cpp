#include "cppboot/boot.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cppboot {

namespace {

/// Lets the application own a module that the container also holds.
class ContainerModule : public Module {
public:
    explicit ContainerModule(std::shared_ptr<Module> instance) : instance_(std::move(instance)) {}

    std::string_view name() const noexcept override { return instance_->name(); }
    std::vector<std::string> dependencies() const override { return instance_->dependencies(); }
    Result<void> start(ApplicationContext& context) override { return instance_->start(context); }
    void stop() noexcept override { instance_->stop(); }

private:
    std::shared_ptr<Module> instance_;
};

/// Shares an object the application owns. Their public functions are const, and components
/// receive them as pointers to const, so nothing can change them through this pointer.
template <typename T>
std::shared_ptr<T> borrow(const T& value) {
    return std::shared_ptr<T>{const_cast<T*>(&value), [](T*) {}};
}

}  // namespace

int runApplication(int argc, char* argv[], const ComponentRegistrar& register_components,
                   ApplicationOptions options) {
    try {
        Application app(argc, argv, std::move(options));

        // Declared after the application, so that singletons are destroyed while the logger and
        // configuration still exist. Modules hold their own references to what they use.
        di::Container container;

        app.setModuleProvider([&](const config::Config& config, const logging::Logger& logger)
                                  -> Result<std::vector<std::unique_ptr<Module>>> {
            container.registerInstance(borrow(config));
            container.registerInstance(borrow(logger));
            try {
                register_components(container);
            } catch (const Exception& exception) {
                // Such as settings that don't fit a CPPBOOT_CONFIG struct. Returning the error
                // keeps its code, so the application exits as for any other configuration error.
                return exception.error();
            }

            if (auto valid = container.validate(); !valid) {
                return valid.error();
            }

            std::vector<std::unique_ptr<Module>> modules;
            for (auto& instance : container.resolveAll<Module>()) {
                modules.push_back(std::make_unique<ContainerModule>(std::move(instance)));
            }
            return modules;
        });

        return app.run();
    } catch (const std::exception& exception) {
        std::cerr << "CppBoot failed: " << exception.what() << '\n';
        return kExitFailure;
    }
}

}  // namespace cppboot
