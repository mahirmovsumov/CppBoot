#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::di {

enum class Lifetime {
    /// One instance per container.
    Singleton,
    /// A new instance for each resolution.
    Transient,
    /// One instance per Scope.
    Scoped,
    /// An instance created by the application and handed to the container.
    External,
};

/// Lists the types a component's constructor takes, each passed as a std::shared_ptr.
template <typename... Dependencies>
struct Deps {};

class Container;
class Scope;

/// Passed to factories for requesting their dependencies. It's valid only during the factory call.
class Resolver {
public:
    /// Throws cppboot::Exception if the type isn't registered or depends on itself.
    template <typename T>
    std::shared_ptr<T> resolve();

private:
    friend class Container;
    friend class Scope;

    Resolver(Container& container, Scope* scope) : container_(container), scope_(scope) {}

    Container& container_;
    Scope* scope_;
    std::vector<std::type_index> chain_;
};

template <typename Factory, typename T>
concept ComponentFactory =
    std::invocable<Factory&, Resolver&> &&
    std::convertible_to<std::invoke_result_t<Factory&, Resolver&>, std::shared_ptr<T>>;

/// Creates components and supplies their dependencies. Registration and resolution are both safe
/// to call from any thread. Singletons are destroyed in the reverse of their creation order.
class Container {
public:
    Container() = default;
    ~Container();

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    /// Registers T, created as Impl: container.registerSingleton<Repo, SqlRepo>(Deps<Database>{}).
    /// Registering a type twice throws cppboot::Exception, as do the functions below.
    template <typename T, typename Impl = T, typename... Dependencies>
    Container& registerSingleton(Deps<Dependencies...> = {}) {
        return addComponent<T, Impl, Dependencies...>(Lifetime::Singleton);
    }

    template <typename T, typename Impl = T, typename... Dependencies>
    Container& registerTransient(Deps<Dependencies...> = {}) {
        return addComponent<T, Impl, Dependencies...>(Lifetime::Transient);
    }

    template <typename T, typename Impl = T, typename... Dependencies>
    Container& registerScoped(Deps<Dependencies...> = {}) {
        return addComponent<T, Impl, Dependencies...>(Lifetime::Scoped);
    }

    /// Registers T with a factory, for components that need more than their dependencies to be
    /// built. validate() can't see inside a factory, so resolving is what reports its problems.
    template <typename T, ComponentFactory<T> Factory>
    Container& registerSingleton(Factory factory) {
        return addFactory<T>(Lifetime::Singleton, std::move(factory));
    }

    template <typename T, ComponentFactory<T> Factory>
    Container& registerTransient(Factory factory) {
        return addFactory<T>(Lifetime::Transient, std::move(factory));
    }

    template <typename T, ComponentFactory<T> Factory>
    Container& registerScoped(Factory factory) {
        return addFactory<T>(Lifetime::Scoped, std::move(factory));
    }

    /// Registers an instance owned by the application. A null pointer throws cppboot::Exception.
    template <typename T>
    Container& registerInstance(std::shared_ptr<T> instance) {
        add(typeid(T), Lifetime::External, {}, true, {}, std::shared_ptr<void>{std::move(instance)});
        return *this;
    }

    /// Makes a registered Component available as Interface too, using the same instance.
    template <typename Interface, typename Component>
    Container& bind() {
        static_assert(std::convertible_to<Component*, Interface*>,
                      "Component must publicly derive from Interface");
        add(
            typeid(Interface), Lifetime::Transient, {typeid(Component)}, true,
            [](Resolver& resolver) -> std::shared_ptr<void> {
                std::shared_ptr<Interface> instance = resolver.resolve<Component>();
                return instance;
            },
            {});
        return *this;
    }

    /// Adds a registered Component to a group, for interfaces with many implementations. A group
    /// is separate from normal resolution, and is read with resolveAll().
    template <typename Interface, typename Component>
    Container& bindAll() {
        static_assert(std::convertible_to<Component*, Interface*>,
                      "Component must publicly derive from Interface");
        addToGroup(typeid(Interface), typeid(Component),
                   [](Resolver& resolver) -> std::shared_ptr<void> {
                       std::shared_ptr<Interface> instance = resolver.resolve<Component>();
                       return instance;
                   });
        return *this;
    }

    /// Returns every member of a group in the order they were added, or nothing for an unknown one.
    template <typename Interface>
    std::vector<std::shared_ptr<Interface>> resolveAll() {
        std::vector<std::shared_ptr<Interface>> instances;
        for (const ErasedFactory& factory : groupMembers(typeid(Interface))) {
            Resolver resolver{*this, nullptr};
            instances.push_back(std::static_pointer_cast<Interface>(factory(resolver)));
        }
        return instances;
    }

    /// Throws cppboot::Exception if the type isn't registered, depends on itself, is scoped and is
    /// resolved outside a Scope, or if its factory fails.
    template <typename T>
    std::shared_ptr<T> resolve() {
        Resolver resolver{*this, nullptr};
        return resolver.resolve<T>();
    }

    bool contains(std::type_index type) const;

    /// Checks declared dependencies without creating anything. Fails on a missing registration, a
    /// cycle, or a singleton that depends on a scoped component, which would outlive its scope.
    Result<void> validate() const;

private:
    friend class Resolver;
    friend class Scope;

    using ErasedFactory = std::function<std::shared_ptr<void>(Resolver&)>;

    struct Registration {
        Lifetime lifetime;
        std::vector<std::type_index> dependencies;
        /// False for a factory, whose dependencies can't be known without running it.
        bool dependencies_known;
        ErasedFactory factory;
        std::shared_ptr<void> instance;
    };

    template <typename T, typename Impl, typename... Dependencies>
    Container& addComponent(Lifetime lifetime) {
        static_assert(std::convertible_to<Impl*, T*>, "Impl must publicly derive from T");
        static_assert(std::constructible_from<Impl, std::shared_ptr<Dependencies>...>,
                      "Impl must be constructible from a std::shared_ptr to each dependency");
        add(
            typeid(T), lifetime, {std::type_index{typeid(Dependencies)}...}, true,
            [](Resolver& resolver) -> std::shared_ptr<void> {
                std::shared_ptr<T> instance =
                    std::make_shared<Impl>(resolver.resolve<Dependencies>()...);
                return instance;
            },
            {});
        return *this;
    }

    template <typename T, typename Factory>
    Container& addFactory(Lifetime lifetime, Factory factory) {
        add(
            typeid(T), lifetime, {}, false,
            [factory = std::move(factory)](Resolver& resolver) mutable -> std::shared_ptr<void> {
                std::shared_ptr<T> instance = factory(resolver);
                return instance;
            },
            {});
        return *this;
    }

    void add(std::type_index type, Lifetime lifetime, std::vector<std::type_index> dependencies,
             bool dependencies_known, ErasedFactory factory, std::shared_ptr<void> instance);

    void addToGroup(std::type_index group, std::type_index member, ErasedFactory factory);
    std::vector<ErasedFactory> groupMembers(std::type_index group) const;

    std::shared_ptr<void> resolveEntry(std::type_index type, Resolver& resolver);
    std::shared_ptr<void> create(std::type_index type, Registration& registration,
                                 Resolver& resolver);

    struct GroupMember {
        std::type_index type;
        ErasedFactory factory;
    };

    // Hash maps because std::type_info::before() isn't a consistent order on MinGW when local or
    // anonymous-namespace types are mixed with others, which breaks std::map. Equality is reliable.
    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::type_index, Registration> registrations_;
    std::vector<Registration*> created_singletons_;
    std::unordered_map<std::type_index, std::vector<GroupMember>> groups_;
};

/// A lifetime for scoped components, such as one web request. It must not outlive its container.
/// Its components are destroyed with it, in the reverse of their creation order.
class Scope {
public:
    explicit Scope(Container& container) : container_(container) {}
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    template <typename T>
    std::shared_ptr<T> resolve() {
        Resolver resolver{container_, this};
        return resolver.resolve<T>();
    }

private:
    friend class Container;

    Container& container_;
    std::recursive_mutex mutex_;
    std::vector<std::pair<std::type_index, std::shared_ptr<void>>> instances_;
};

template <typename T>
std::shared_ptr<T> Resolver::resolve() {
    return std::static_pointer_cast<T>(container_.resolveEntry(typeid(T), *this));
}

}  // namespace cppboot::di
