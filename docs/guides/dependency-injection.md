# Dependency injection

Dependency injection means a class receives the objects it needs instead of creating them. `cppboot::di::Container` creates your components, passes each one its dependencies and manages how long they live.

There are two ways to register components. Most applications should mark their classes and let CppBoot generate the registration, and you can still register by hand when you need more control.

## Automatic registration

Mark a class with `CPPBOOT_SERVICE`, and take the components it needs as `std::shared_ptr` constructor parameters:

```cpp
class CPPBOOT_SERVICE MemoryUserRepository : public UserRepository {
public:
    std::string findName(int id) const override;
};

class CPPBOOT_SERVICE UserService {
public:
    explicit UserService(std::shared_ptr<UserRepository> repository);
};
```

A project made by `cppboot create` generates the registration during its build, from every `.hpp` file in `src`. A project with its own CMake setup calls this:

```cmake
target_link_libraries(my-app PRIVATE cppboot::boot)
cppboot_generate_components(my-app HEADERS src/services.hpp)
```

Then include the generated header, and pass its function to `runApplication`:

```cpp
#include <cppboot/boot.hpp>

#include "cppboot_components.generated.hpp"

int main(int argc, char* argv[]) {
    return cppboot::runApplication(argc, argv, cppboot::generated::registerComponents);
}
```

### Rules for marked classes

The build fails with the file and line if a marked class breaks one of these rules:

- It can't be abstract or a template, or be in an anonymous namespace.
- It needs one public constructor, not counting copy and move constructors, or none at all.
- Every constructor parameter must be a `std::shared_ptr` to a component, by value or by reference.

The build also fails on a missing dependency, a dependency cycle, or two components that provide the same type.

### What marked classes get

- All marked classes are singletons, created on first use.
- Other components can ask for a marked class by its own type, or by an abstract base class it directly and publicly inherits from.
- A marked class that derives from `cppboot::Module` starts and stops with the application, so it's created at startup.

### Settings and logging

A marked class can also take the application's settings and logger in its constructor. `runApplication` provides both, so you don't have to register them:

```cpp
class CPPBOOT_SERVICE UserService {
public:
    UserService(std::shared_ptr<UserRepository> repository,
                std::shared_ptr<const cppboot::config::Config> config,
                std::shared_ptr<const cppboot::logging::Logger> logger);
};
```

Components are created during startup, after the configuration has loaded and before modules start.

## Manual registration

## Example

```cpp
#include <cppboot/cppboot.hpp>
#include <cppboot/di/container.hpp>
#include <iostream>
#include <memory>
#include <string>

class UserRepository {
public:
    virtual ~UserRepository() = default;
    virtual std::string findName(int id) const = 0;
};

class MemoryUserRepository : public UserRepository {
public:
    std::string findName(int id) const override { return "user-" + std::to_string(id); }
};

class UserService {
public:
    explicit UserService(std::shared_ptr<UserRepository> repository)
        : repository_(std::move(repository)) {}

    std::string greeting(int id) const { return "Hello, " + repository_->findName(id); }

private:
    std::shared_ptr<UserRepository> repository_;
};

class GreetingModule : public cppboot::Module {
public:
    explicit GreetingModule(std::shared_ptr<UserService> users) : users_(std::move(users)) {}

    std::string_view name() const noexcept override { return "greeting"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("{}", users_->greeting(7));
        return {};
    }

private:
    std::shared_ptr<UserService> users_;
};

int main(int argc, char* argv[]) {
    using namespace cppboot::di;

    Container container;
    container.registerSingleton<UserRepository, MemoryUserRepository>();
    container.registerSingleton<UserService>(Deps<UserRepository>{});

    if (auto valid = container.validate(); !valid) {
        std::cerr << valid.error().message() << '\n';
        return cppboot::kExitFailure;
    }

    cppboot::Application app(argc, argv);
    app.emplaceModule<GreetingModule>(container.resolve<UserService>());
    return app.run();
}
```

Declare the container before the application, so that it's destroyed after the modules that use its components.

## Register components

`Deps<...>` lists the types a constructor takes. Each constructor parameter must accept a `std::shared_ptr` to that type, and a mismatch is a compile error.

```cpp
// Register a class as itself.
container.registerSingleton<UserService>(Deps<UserRepository>{});

// Register a class as its interface.
container.registerSingleton<UserRepository, MemoryUserRepository>();

// Use a factory for anything else a component needs to be built.
container.registerSingleton<Database>([](Resolver& resolver) {
    return std::make_shared<Database>("localhost", resolver.resolve<Logger>());
});

// Hand over an object the application created.
container.registerInstance(std::make_shared<Clock>());

// Make a registered component available through an interface too. Both share one instance.
container.registerSingleton<MemoryUserRepository>();
container.bind<UserRepository, MemoryUserRepository>();

// Add components to a group, for an interface with many implementations.
container.registerSingleton<EmailNotifier>();
container.registerSingleton<SmsNotifier>();
container.bindAll<Notifier, EmailNotifier>();
container.bindAll<Notifier, SmsNotifier>();
auto notifiers = container.resolveAll<Notifier>();
```

Registering the same type twice throws `cppboot::Exception`.

## Lifetimes

| Lifetime | Function | Instances | Destroyed |
|---|---|---|---|
| Singleton | `registerSingleton` | One per container, created on first use | With the container, newest first |
| Transient | `registerTransient` | A new one each time | When the last `shared_ptr` is released |
| Scoped | `registerScoped` | One per `Scope` | With the scope, newest first |
| External | `registerInstance` | The one you provided | When the last `shared_ptr` is released |

A scope is a shorter lifetime within the application, such as one web request:

```cpp
container.registerScoped<RequestContext>();

{
    cppboot::di::Scope scope{container};
    auto context = scope.resolve<RequestContext>();
}  // Scoped components are destroyed here.
```

A scope must not outlive its container. Resolving a scoped component without a scope throws an exception.

A singleton can't use a scoped component, because it would keep that component alive after its scope ended. This applies even when a transient sits between them.

## Find problems early

`validate()` checks the declared dependencies without creating anything, so call it at startup. It reports:

- A dependency that isn't registered
- A dependency cycle
- A singleton that depends on a scoped component

`validate()` can't see inside a factory. Problems in a factory's dependencies are reported when you resolve it, where `resolve()` throws `cppboot::Exception` with the chain of types involved. A cycle always produces this exception, never a stack overflow.

## Threads

Registering and resolving are safe from any thread, and two threads resolving a new singleton get the same instance. A factory runs while the container holds a lock for singletons, so don't make it wait for another thread that resolves from the same container.
