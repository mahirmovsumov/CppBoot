#include "cppboot/platform/socket_state.hpp"

#include <winsock2.h>

namespace cppboot::platform {

bool peerHasClosed(std::uintptr_t socket_handle) noexcept {
    const auto socket = static_cast<SOCKET>(socket_handle);

    // These sockets block on a plain recv, so this checks first that it would return at once.
    WSAPOLLFD entry{};
    entry.fd = socket;
    entry.events = POLLRDNORM;
    if (WSAPoll(&entry, 1, 0) <= 0 || entry.revents == 0) {
        return false;
    }

    char byte = 0;
    const int received = recv(socket, &byte, 1, MSG_PEEK);
    if (received == 0) {
        return true;
    }
    return received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK;
}

}  // namespace cppboot::platform
