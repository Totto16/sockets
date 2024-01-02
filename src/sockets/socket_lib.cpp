#include "sockets/socket_lib.hpp"
#include "socket_headers.hpp"
#include <iostream>

namespace c2k {
    SocketLib::SocketLib() {
#ifdef _WIN32
        auto wsa_data = WSADATA{};
        static constexpr auto winsock_version = MAKEWORD(2, 2);
        if (WSAStartup(winsock_version, &wsa_data) != 0) {
            throw std::runtime_error{ "unable to initialize winsock" };
        }
#endif
    }

    SocketLib::~SocketLib() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // clang-format off
    [[nodiscard]] ClientSocket SocketLib::create_client_socket(
        AddressFamily const address_family,
        std::string const& host,
        std::uint16_t const port,
        SocketLib const&
    ) { // clang-format on
        return ClientSocket{ address_family, host, port };
    }

    [[nodiscard]] SocketLib const& SocketLib::instance() {
        static auto handle = SocketLib{};
        return handle;
    }
} // namespace c2k