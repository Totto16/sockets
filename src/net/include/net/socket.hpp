#pragma once

#include "address_family.hpp"
#include "non_null_owner.hpp"
#include "unique_value.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

class ClientSocket;

class AbstractSocket {
public:
#ifdef _WIN32
    using OsSocketHandle = std::uintptr_t;
#else
    using OsSocketHandle = int;
#endif

private:
    friend void server_listen(
            OsSocketHandle listen_socket,
            std::atomic_bool const& running,
            std::function<void(ClientSocket)> on_connect
    );

protected:
    UniqueValue<OsSocketHandle, void (*)(OsSocketHandle handle)> m_socket_descriptor;

    explicit AbstractSocket(OsSocketHandle os_socket_handle);

public:
    [[nodiscard]] std::optional<OsSocketHandle> os_socket_handle() const {
        if (not m_socket_descriptor.has_value()) {
            return std::nullopt;
        }
        return m_socket_descriptor.value();
    }
};

namespace detail {
    [[nodiscard]] AbstractSocket::OsSocketHandle
    initialize_server_socket(AddressFamily address_family, std::uint16_t port);
}

class ServerSocket final : public AbstractSocket {
    friend class SocketLib;

private:
    NonNullOwner<std::atomic_bool> m_running{ make_non_null_owner<std::atomic_bool>(true) };
    std::jthread m_listen_thread;

    ServerSocket(AddressFamily address_family, std::uint16_t port, std::function<void(ClientSocket)> on_connect);

public:
    ~ServerSocket();
    void stop();
};

class ClientSocket final : public AbstractSocket {
    friend class SocketLib;
    friend void server_listen(
            OsSocketHandle listen_socket,
            std::atomic_bool const& running,
            std::function<void(ClientSocket)> on_connect
    );

private:
    struct SendTask {
        std::promise<std::size_t> promise;
        std::vector<std::byte> data;
    };

    struct ReceiveTask {
        std::promise<std::vector<std::byte>> promise;
        std::size_t max_num_bytes;
    };

    struct State {
        // todo: replace std::atomic_bool using stop_token (also do this in ServerSocket)
        NonNullOwner<std::atomic_bool> running{ make_non_null_owner<std::atomic_bool>(true) };
        std::mutex send_tasks_mutex;
        std::deque<SendTask> send_tasks;
        std::mutex receive_tasks_mutex;
        std::deque<ReceiveTask> receive_tasks;
    };

    std::unique_ptr<State> m_shared_state{ std::make_unique<State>() };
    std::jthread m_send_receive_thread;

    explicit ClientSocket(OsSocketHandle os_socket_handle);
    ClientSocket(AddressFamily address_family, std::string const& host, std::uint16_t port);

    static void keep_sending_and_receiving(State& state, OsSocketHandle socket);


public:
    ~ClientSocket();

    [[nodiscard]] bool is_connected() const {
        return *m_shared_state->running;
    }

    std::future<std::size_t> send(std::vector<std::byte> data);
    [[nodiscard]] std::future<std::vector<std::byte>> receive(std::size_t max_num_bytes);
};
