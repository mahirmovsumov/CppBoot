#include "cppboot/data/database_module.hpp"

#include <string>
#include <utility>

#include "cppboot/data/migrations.hpp"
#include "cppboot/logging/logger.hpp"

namespace cppboot::data {

DatabaseModule::DatabaseModule(std::shared_ptr<Database> database,
                               std::shared_ptr<const config::Config> config)
    : database_(std::move(database)), config_(std::move(config)) {}

Result<void> DatabaseModule::start(ApplicationContext& context) {
    try {
        database_->query("SELECT 1");
    } catch (const DatabaseError& error) {
        return Error{ErrorCode::StartupError, error.error().message()};
    }

    const std::string migrations = config_->getString("cppboot.database.migrations");
    if (!migrations.empty()) {
        auto applied = runMigrations(*database_, migrations);
        if (!applied) {
            return applied.error();
        }
        context.logger().info("Applied {} new database migrations", applied.value());
    }

    context.logger().info("Database is ready");
    return {};
}

}  // namespace cppboot::data
