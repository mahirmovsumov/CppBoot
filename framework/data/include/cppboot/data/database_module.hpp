#pragma once

#include <memory>
#include <string_view>

#include "cppboot/config/config.hpp"
#include "cppboot/core/application_context.hpp"
#include "cppboot/core/module.hpp"
#include "cppboot/data/database.hpp"

namespace cppboot::data {

/// Checks the database connection at startup, so that a bad URL or a stopped server fails at once
/// rather than on the first request. It also applies migrations if cppboot.database.migrations
/// names a folder.
class DatabaseModule : public Module {
public:
    DatabaseModule(std::shared_ptr<Database> database,
                   std::shared_ptr<const config::Config> config);

    std::string_view name() const noexcept override { return "database"; }
    Result<void> start(ApplicationContext& context) override;

private:
    std::shared_ptr<Database> database_;
    std::shared_ptr<const config::Config> config_;
};

}  // namespace cppboot::data
