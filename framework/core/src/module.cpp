#include "cppboot/core/module.hpp"

#include <exception>
#include <functional>
#include <map>
#include <utility>

namespace cppboot {

void ModuleManager::add(std::unique_ptr<Module> instance) {
    modules_.push_back(std::move(instance));
}

Result<void> ModuleManager::resolveOrder() {
    order_.clear();

    const std::size_t count = modules_.size();
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t i = 0; i < count; ++i) {
        if (!modules_[i]) {
            return Error{ErrorCode::InvalidArgument, "A null module was registered"};
        }
        std::string name{modules_[i]->name()};
        if (!indexes.emplace(name, i).second) {
            return Error{ErrorCode::DependencyError, "Duplicate module name: " + name};
        }
    }

    std::vector<std::size_t> unmet(count, 0);
    std::vector<std::vector<std::size_t>> dependents(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (const std::string& dependency : modules_[i]->dependencies()) {
            const auto found = indexes.find(dependency);
            if (found == indexes.end()) {
                return Error{ErrorCode::DependencyError,
                             "Module " + std::string{modules_[i]->name()} +
                                 " depends on unknown module " + dependency};
            }
            ++unmet[i];
            dependents[found->second].push_back(i);
        }
    }

    std::vector<bool> placed(count, false);
    std::vector<Module*> order;
    order.reserve(count);
    while (order.size() < count) {
        bool progressed = false;
        for (std::size_t i = 0; i < count && !progressed; ++i) {
            if (placed[i] || unmet[i] != 0) {
                continue;
            }
            placed[i] = true;
            order.push_back(modules_[i].get());
            for (const std::size_t dependent : dependents[i]) {
                --unmet[dependent];
            }
            progressed = true;
        }

        if (!progressed) {
            std::string remaining;
            for (std::size_t i = 0; i < count; ++i) {
                if (!placed[i]) {
                    remaining += remaining.empty() ? "" : ", ";
                    remaining += modules_[i]->name();
                }
            }
            return Error{ErrorCode::DependencyError,
                         "Circular dependency involving modules: " + remaining};
        }
    }

    order_ = std::move(order);
    return {};
}

Result<void> ModuleManager::startAll(ApplicationContext& context) {
    for (; started_ < order_.size(); ++started_) {
        Module& instance = *order_[started_];
        const std::string name{instance.name()};

        try {
            if (auto started = instance.start(context); !started) {
                return Error{ErrorCode::StartupError,
                             "Module " + name + " failed to start: " + started.error().describe()};
            }
        } catch (const std::exception& exception) {
            return Error{ErrorCode::StartupError,
                         "Module " + name + " threw during startup: " + exception.what()};
        } catch (...) {
            return Error{ErrorCode::StartupError,
                         "Module " + name + " threw an unknown exception during startup"};
        }
    }
    return {};
}

void ModuleManager::stopAll() noexcept {
    while (started_ > 0) {
        --started_;
        order_[started_]->stop();
    }
}

std::vector<std::string> ModuleManager::orderedNames() const {
    std::vector<std::string> names;
    names.reserve(order_.size());
    for (const Module* instance : order_) {
        names.emplace_back(instance->name());
    }
    return names;
}

}  // namespace cppboot
