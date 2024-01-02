#pragma once

#include "socket.hpp"

namespace c2k {
    class SocketLib final {
    private:
        SocketLib();

    public:
        SocketLib(SocketLib const& other) = delete;
        SocketLib(SocketLib&& other) noexcept = delete;
        SocketLib& operator=(SocketLib const& other) = delete;
        SocketLib& operator=(SocketLib&& other) noexcept = delete;
        ~SocketLib();

        // clang-format off
        [[nodiscard]] static auto create_server_socket(
            AddressFamily const address_family,
            std::uint16_t const port,
            std::function<void(ClientSocket)> callback,
            SocketLib const& = instance()
        ) {
            return ServerSocket{ address_family, port, std::move(callback) };
        }

        [[nodiscard]] static ClientSocket create_client_socket(
            AddressFamily address_family,
            std::string const& host,
            std::uint16_t port,
            SocketLib const& = instance()
        );
        // clang-format on

    private:
        [[nodiscard]] static SocketLib const& instance();
    };
} // namespace c2k