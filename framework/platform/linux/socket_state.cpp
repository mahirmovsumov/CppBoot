#include "cppboot/platform/socket_state.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>

namespace cppboot::platform {

bool peerHasClosed(std::uintptr_t socket_handle) noexcept {
    char byte = 0;
    const ssize_t received =
        recv(static_cast<int>(socket_handle), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (received == 0) {
        return true;
    }
    return received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

}  // namespace cppboot::platform
