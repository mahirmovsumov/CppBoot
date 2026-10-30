#include "cppboot/di/container.hpp"

#include <algorithm>
#include <cstdlib>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif

namespace cppboot::di {

namespace {

std::string typeName(std::type_index type) {
#if __has_include(<cxxabi.h>)
    int status = 0;
    const std::unique_ptr<char, decltype(&std::free)> demangled{
        abi::__cxa_demangle(type.name(), nullptr, nullptr, &status), std::free};
    if (status == 0 && demangled) {
        return demangled.get();
    }
#endif
    return type.name();
}

std::string describeChain(const std::vector<std::type_index>& chain, std::type_index last) {
    std::string text;
    for (const std::type_index type : chain) {
        text += typeName(type) + " -> ";
    }
    return text + typeName(last);
}

/// Removes the last entry of the resolution chain when a resolve finishes, even by throwing.
class ChainEntry {
public:
    ChainEntry(std::vector<std::type_index>& chain, std::type_index type) : chain_(chain) {
        chain_.push_back(type);
    }
    ~ChainEntry() { chain_.pop_back(); }

    ChainEntry(const ChainEntry&) = delete;
    ChainEntry& operator=(const ChainEntry&) = delete;

private:
    std::vector<std::type_index>& chain_;
};

}  // namespace

Container::~Container() {
    for (auto registration = created_singletons_.rbegin();
         registration != created_singletons_.rend(); ++registration) {
        (*registration)->instance.reset();
    }
}

void Container::add(std::type_index type, Lifetime lifetime,
                    std::vector<std::type_index> dependencies, bool dependencies_known,
                    ErasedFactory factory, std::shared_ptr<void> instance) {
    if (lifetime == Lifetime::External && !instance) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "A null instance was registered for " + typeName(type)});
    }

    const std::lock_guard lock{mutex_};
    const auto [entry, inserted] = registrations_.try_emplace(
        type, Registration{lifetime, std::move(dependencies), dependencies_known,
                           std::move(factory), std::move(instance)});
    if (!inserted) {
        throw Exception(
            Error{ErrorCode::DependencyError, typeName(type) + " is already registered"});
    }
}

void Container::addToGroup(std::type_index group, std::type_index member, ErasedFactory factory) {
    const std::lock_guard lock{mutex_};
    groups_[group].push_back(GroupMember{member, std::move(factory)});
}

std::vector<Container::ErasedFactory> Container::groupMembers(std::type_index group) const {
    const std::lock_guard lock{mutex_};

    std::vector<ErasedFactory> factories;
    if (const auto entry = groups_.find(group); entry != groups_.end()) {
        for (const GroupMember& member : entry->second) {
            factories.push_back(member.factory);
        }
    }
    return factories;
}

bool Container::contains(std::type_index type) const {
    const std::lock_guard lock{mutex_};
    return registrations_.contains(type);
}

std::shared_ptr<void> Container::create(std::type_index type, Registration& registration,
                                        Resolver& resolver) {
    std::shared_ptr<void> instance = registration.factory(resolver);
    if (!instance) {
        throw Exception(
            Error{ErrorCode::DependencyError, "The factory for " + typeName(type) + " returned null"});
    }
    return instance;
}

std::shared_ptr<void> Container::resolveEntry(std::type_index type, Resolver& resolver) {
    if (std::find(resolver.chain_.begin(), resolver.chain_.end(), type) != resolver.chain_.end()) {
        throw Exception(Error{ErrorCode::DependencyError,
                              "Circular dependency: " + describeChain(resolver.chain_, type)});
    }

    Registration* registration = nullptr;
    {
        const std::lock_guard lock{mutex_};
        const auto entry = registrations_.find(type);
        if (entry == registrations_.end()) {
            throw Exception(Error{ErrorCode::DependencyError,
                                  "No component is registered for " +
                                      describeChain(resolver.chain_, type)});
        }
        // Map entries keep their address, and a registration is never removed.
        registration = &entry->second;
    }

    const ChainEntry chain_entry{resolver.chain_, type};

    switch (registration->lifetime) {
        case Lifetime::External:
            return registration->instance;

        case Lifetime::Transient:
            return create(type, *registration, resolver);

        case Lifetime::Singleton: {
            // Held during creation, so that two threads can't both create the instance. A
            // container's lock is never held while waiting for a scope's lock, so they can't
            // deadlock: singletons are resolved without a scope.
            const std::lock_guard lock{mutex_};
            if (!registration->instance) {
                Scope* const scope = std::exchange(resolver.scope_, nullptr);
                try {
                    registration->instance = create(type, *registration, resolver);
                } catch (...) {
                    resolver.scope_ = scope;
                    throw;
                }
                resolver.scope_ = scope;
                created_singletons_.push_back(registration);
            }
            return registration->instance;
        }

        case Lifetime::Scoped: {
            if (resolver.scope_ == nullptr) {
                throw Exception(Error{ErrorCode::DependencyError,
                                      typeName(type) +
                                          " is scoped, so it must be resolved from a Scope and "
                                          "can't be used by a singleton: " +
                                          describeChain(resolver.chain_, type)});
            }

            Scope& scope = *resolver.scope_;
            const std::lock_guard lock{scope.mutex_};
            const auto existing =
                std::find_if(scope.instances_.begin(), scope.instances_.end(),
                             [&](const auto& instance) { return instance.first == type; });
            if (existing != scope.instances_.end()) {
                return existing->second;
            }

            std::shared_ptr<void> instance = create(type, *registration, resolver);
            scope.instances_.emplace_back(type, instance);
            return instance;
        }
    }

    throw Exception(Error{ErrorCode::InvalidState, "Unknown lifetime for " + typeName(type)});
}

Result<void> Container::validate() const {
    const std::lock_guard lock{mutex_};

    for (const auto& [type, registration] : registrations_) {
        for (const std::type_index dependency : registration.dependencies) {
            if (!registrations_.contains(dependency)) {
                return Error{ErrorCode::DependencyError,
                             typeName(type) + " needs " + typeName(dependency) +
                                 ", which isn't registered"};
            }
        }
    }

    for (const auto& [group, members] : groups_) {
        for (const GroupMember& member : members) {
            if (!registrations_.contains(member.type)) {
                return Error{ErrorCode::DependencyError,
                             "Group " + typeName(group) + " includes " + typeName(member.type) +
                                 ", which isn't registered"};
            }
        }
    }

    enum class VisitState { InProgress, Done };
    std::unordered_map<std::type_index, VisitState> states;
    std::vector<std::type_index> path;

    const std::function<Result<void>(std::type_index)> visit =
        [&](std::type_index type) -> Result<void> {
        if (const auto state = states.find(type); state != states.end()) {
            if (state->second == VisitState::Done) {
                return {};
            }
            const auto start = std::find(path.begin(), path.end(), type);
            return Error{ErrorCode::DependencyError,
                         "Circular dependency: " +
                             describeChain(std::vector<std::type_index>(start, path.end()), type)};
        }

        states.emplace(type, VisitState::InProgress);
        path.push_back(type);
        for (const std::type_index dependency : registrations_.at(type).dependencies) {
            if (auto visited = visit(dependency); !visited) {
                return visited;
            }
        }
        path.pop_back();
        states[type] = VisitState::Done;
        return {};
    };

    for (const auto& [type, registration] : registrations_) {
        if (auto visited = visit(type); !visited) {
            return visited;
        }
    }

    // A transient created by a singleton lives as long as that singleton, so look through
    // transients. Other singletons are checked by their own turn in the outer loop.
    const std::function<const std::type_index*(const Registration&)> findScoped =
        [&](const Registration& registration) -> const std::type_index* {
        for (const std::type_index& dependency : registration.dependencies) {
            const Registration& target = registrations_.at(dependency);
            if (target.lifetime == Lifetime::Scoped) {
                return &dependency;
            }
            if (target.lifetime == Lifetime::Transient) {
                if (const std::type_index* found = findScoped(target)) {
                    return found;
                }
            }
        }
        return nullptr;
    };

    for (const auto& [type, registration] : registrations_) {
        if (registration.lifetime == Lifetime::Singleton) {
            if (const std::type_index* scoped = findScoped(registration)) {
                return Error{ErrorCode::DependencyError,
                             "Singleton " + typeName(type) + " depends on scoped component " +
                                 typeName(*scoped) + ", which it would outlive"};
            }
        }
    }
    return {};
}

Scope::~Scope() {
    while (!instances_.empty()) {
        instances_.pop_back();
    }
}

}  // namespace cppboot::di
