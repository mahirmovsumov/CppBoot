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

}  // namespace

int runApplication(int argc, char* argv[], const ComponentRegistrar& register_components,
                   ApplicationOptions options) {
    // Declared before the application, so that components outlive the modules using them.
    di::Container container;

    try {
        register_components(container);

        if (auto valid = container.validate(); !valid) {
            std::cerr << "CppBoot failed: " << valid.error().describe() << '\n';
            return kExitFailure;
        }

        Application app(argc, argv, std::move(options));
        for (auto& instance : container.resolveAll<Module>()) {
            app.emplaceModule<ContainerModule>(std::move(instance));
        }
        return app.run();
    } catch (const std::exception& exception) {
        std::cerr << "CppBoot failed: " << exception.what() << '\n';
        return kExitFailure;
    }
}

}  // namespace cppboot
