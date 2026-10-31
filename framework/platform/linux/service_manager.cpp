#include "cppboot/platform/service_manager.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace cppboot::platform {

void notifyServiceManager(std::string_view state) noexcept {
    const char* const path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || (path[0] != '/' && path[0] != '@')) {
        return;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::size_t length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) {
        return;
    }
    std::memcpy(address.sun_path, path, length);
    if (path[0] == '@') {
        // A name in the abstract namespace starts with a null byte.
        address.sun_path[0] = '\0';
    }

    const int socket_handle = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_handle < 0) {
        return;
    }
    static_cast<void>(sendto(socket_handle, state.data(), state.size(), MSG_NOSIGNAL,
                             reinterpret_cast<const sockaddr*>(&address),
                             static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length)));
    close(socket_handle);
}

}  // namespace cppboot::platform
