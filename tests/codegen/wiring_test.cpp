#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <typeindex>

#include "cppboot_components.generated.hpp"

namespace {

// The smallest registry the generated code can use: each component is created once, on first use.
// Phase 2 replaces this with the real container.
class TestRegistry {
public:
    template <typename T, typename... Interfaces, typename Factory>
    void add(Factory factory) {
        auto instance = std::make_shared<std::shared_ptr<T>>();
        const auto create = [this, factory, instance] {
            if (!*instance) {
                *instance = factory(*this);
            }
            return *instance;
        };

        creators_[typeid(T)] = [create]() -> std::shared_ptr<void> { return create(); };
        (addInterface<T, Interfaces>(create), ...);
    }

    template <typename T>
    std::shared_ptr<T> get() {
        return std::static_pointer_cast<T>(creators_.at(typeid(T))());
    }

    bool contains(const std::type_info& type) const { return creators_.contains(type); }

private:
    template <typename T, typename Interface, typename Create>
    void addInterface(Create create) {
        creators_[typeid(Interface)] = [create]() -> std::shared_ptr<void> {
            return std::static_pointer_cast<Interface>(create());
        };
    }

    std::map<std::type_index, std::function<std::shared_ptr<void>()>> creators_;
};

}  // namespace

TEST(GeneratedRegistrationTest, WiresConstructorDependencies) {
    TestRegistry registry;
    cppboot::generated::registerComponents(registry);

    const auto controller = registry.get<demo::web::UserController>();
    EXPECT_EQ(controller->handle(7), "Hello, user-7 / user-7");
}

TEST(GeneratedRegistrationTest, ComponentsCanBeFoundByTheirInterface) {
    TestRegistry registry;
    cppboot::generated::registerComponents(registry);

    EXPECT_EQ(registry.get<demo::UserRepository>()->findName(1), "user-1");
}

TEST(GeneratedRegistrationTest, SharesOneInstanceOfEachComponent) {
    TestRegistry registry;
    cppboot::generated::registerComponents(registry);

    const auto service = registry.get<demo::UserService>();
    EXPECT_EQ(registry.get<demo::web::UserController>()->users(), service.get());
}

TEST(GeneratedRegistrationTest, LeavesOutClassesThatAreNotMarked) {
    TestRegistry registry;
    cppboot::generated::registerComponents(registry);

    EXPECT_FALSE(registry.contains(typeid(demo::HelperWithoutAnnotation)));
}
