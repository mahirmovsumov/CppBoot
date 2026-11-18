#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/config/config.hpp"
#include "cppboot/core/application_context.hpp"
#include "cppboot/core/module.hpp"
#include "cppboot/web/router.hpp"
#include "cppboot/web/server.hpp"

namespace cppboot::web {

/// Reads server options from settings under cppboot.server, and fails on an invalid value.
Result<ServerOptions> readServerOptions(const config::Config& config);

/// The access log's line for a request, such as
/// "GET /api/users/7 200 in 0.42 ms, 52 bytes, from 10.0.0.8, request 0q3k8c1x7m2fa".
/// Anything in the method or path that could break a log line is replaced with '?'.
std::string formatRequestRecord(const RequestRecord& record);

/// Runs an HTTP server as part of the application's lifecycle. It takes its settings from
/// cppboot.server, and stops with the application.
class ServerModule : public Module {
public:
    explicit ServerModule(std::shared_ptr<Router> router);
    ~ServerModule() override;

    std::string_view name() const noexcept override { return "web"; }
    Result<void> start(ApplicationContext& context) override;
    void stop() noexcept override;

    /// The port in use, once started.
    std::uint16_t port() const noexcept;

private:
    std::shared_ptr<Router> router_;
    std::unique_ptr<Server> server_;
};

}  // namespace cppboot::web
