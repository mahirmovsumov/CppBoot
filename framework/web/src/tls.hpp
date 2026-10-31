#pragma once

#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
#include <string_view>

#include "cppboot/base/result.hpp"
#include "cppboot/web/server.hpp"

namespace cppboot::web {

/// Loads the certificate and key, and applies CppBoot's fixed security settings. Fails if anything
/// is wrong, so that a server asked for HTTPS can never start without it. An expired certificate is
/// only logged, because refusing to restart wouldn't help the clients already being turned away.
Result<std::unique_ptr<boost::asio::ssl::context>> makeTlsContext(
    const TlsOptions& options, const std::function<void(std::string_view)>& log_error);

}  // namespace cppboot::web
