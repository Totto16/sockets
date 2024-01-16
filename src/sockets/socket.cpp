#include "socket_headers.hpp"
#include "sockets/detail/byte_order.hpp"
#include "sockets/detail/unreachable.hpp"
#include "sockets/sockets.hpp"
#include <cassert>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace c2k {
    [[nodiscard]] static constexpr int to_ai_family(AddressFamily const family) {
        switch (family) {
            case AddressFamily::Unspecified:
                return AF_UNSPEC;
            case AddressFamily::Ipv4:
                return AF_INET;
            case AddressFamily::Ipv6:
                return AF_INET6;
        }
        unreachable();
    }

    [[nodiscard]] static constexpr addrinfo generate_hints(AddressFamily const address_family, bool const is_passive) {
        auto hints = addrinfo{};
        hints.ai_family = to_ai_family(address_family);
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (is_passive) {
            hints.ai_flags = AI_PASSIVE;
        }
        return hints;
    }

    enum class SelectStatusCategory {
        Read,
        Write,
        Except,
    };

    [[nodiscard]] static fd_set generate_fd_set(AbstractSocket::OsSocketHandle const socket) {
        auto descriptors = fd_set{};
        FD_ZERO(&descriptors);
        FD_SET(socket, &descriptors);
        return descriptors;
    }

    [[nodiscard]] static bool is_socket_ready(
            AbstractSocket::OsSocketHandle const socket,
            SelectStatusCategory const category,
            std::size_t const timeout_milliseconds
    ) {
        auto const microseconds = timeout_milliseconds * 1000;
        auto timeout = timeval{ .tv_sec = static_cast<long>(microseconds / (1000 * 1000)),
                                .tv_usec = static_cast<long>(microseconds % (1000 * 1000)) };
        auto const select_result = [&] {
            auto descriptors = generate_fd_set(socket);
            switch (category) {
                case SelectStatusCategory::Read:
                    return select(static_cast<int>(socket + 1), &descriptors, nullptr, nullptr, &timeout);
                case SelectStatusCategory::Write:
                    return select(static_cast<int>(socket + 1), nullptr, &descriptors, nullptr, &timeout);
                case SelectStatusCategory::Except:
                    return select(static_cast<int>(socket + 1), nullptr, nullptr, &descriptors, &timeout);
                default:
                    unreachable();
                    break;
            }
        }();
        if (select_result == socket_error) {
            throw std::runtime_error{ "failed to call select on socket" };
        }
        return select_result == 1;
    }

    using AddressInfos = std::unique_ptr<addrinfo, decltype([](addrinfo* const pointer) { freeaddrinfo(pointer); })>;

    // clang-format off
    [[nodiscard]] static AddressInfos get_address_infos(
        AddressFamily const address_family,
        std::uint16_t const port,
        char const* const host = nullptr
    ) { // clang-format on
        auto const is_server = (host == nullptr);

        auto const hints = generate_hints(address_family, is_server);

        auto result = static_cast<addrinfo*>(nullptr);
        if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &result) != 0) {
            throw std::runtime_error{ "unable to call getaddrinfo" };
        }
        if (result == nullptr) {
            throw std::runtime_error{ "no addresses found" };
        }
        return AddressInfos{ result };
    }

    enum class SocketOption {
        TcpNoDelay,
        ReusePort,
    };

    static constexpr auto to_string(SocketOption const option) {
        switch (option) {
            case SocketOption::TcpNoDelay:
                return "TcpNoDelay";
            case SocketOption::ReusePort:
                return "ReusePort";
        }
        unreachable();
    }

    static void set_socket_option(AbstractSocket::OsSocketHandle const socket, SocketOption const option) {
#ifdef _WIN32
        auto flag = char{ 1 };
#else
        auto flag = 1;
#endif
        auto const option_name = [&] {
            switch (option) {
                case SocketOption::TcpNoDelay:
                    return tcp_no_delay;
                case SocketOption::ReusePort:
                    return reuse_port;
            }
            unreachable();
        }();
        auto const level = [&]() -> int {
            switch (option) {
                case SocketOption::TcpNoDelay:
                    return IPPROTO_TCP;
                case SocketOption::ReusePort:
                    return SOL_SOCKET;
            }
            unreachable();
        }();
        auto const result = ::setsockopt(socket, level, option_name, &flag, sizeof(flag));
        if (result < 0) {
            using namespace std::string_literals;
            throw std::runtime_error{ "failed to set "s + to_string(option) };
        }
    }

    static void set_all_default_socket_options(AbstractSocket::OsSocketHandle const socket) {
        set_socket_option(socket, SocketOption::TcpNoDelay);
        set_socket_option(socket, SocketOption::ReusePort);
    }

    [[nodiscard]] static AbstractSocket::OsSocketHandle create_socket(AddressInfos const& address_infos) {
        auto const socket = ::socket(address_infos->ai_family, address_infos->ai_socktype, address_infos->ai_protocol);
        if (socket == invalid_socket) {
            throw std::runtime_error{ "failed to create socket" };
        }
        set_all_default_socket_options(socket);
        return socket;
    }

    static void bind_socket(AbstractSocket::OsSocketHandle const socket, AddressInfos const& address_infos) {
        if (bind(socket, address_infos->ai_addr, static_cast<SockLen>(address_infos->ai_addrlen)) == socket_error) {
            closesocket(socket);
            throw std::runtime_error{ "failed to bind socket" };
        }
    }

    static void connect_socket(AbstractSocket::OsSocketHandle const socket, AddressInfos const& address_infos) {
        if (connect(socket, address_infos->ai_addr, static_cast<SockLen>(address_infos->ai_addrlen)) == socket_error) {
            closesocket(socket);
            throw std::runtime_error{ "unable to connect" };
        }
    }

    static void socket_deleter(AbstractSocket::OsSocketHandle const handle) {
        closesocket(handle);
    }

#ifdef _WIN32
    [[nodiscard]] static AddressInfo extract_adress_info(SOCKADDR_STORAGE const& address) {
        switch (address.ss_family) {
            case AF_INET: {
                auto const ipv4_info = reinterpret_cast<sockaddr_in const*>(&address);
                auto ipv4_address = std::to_string(ipv4_info->sin_addr.S_un.S_un_b.s_b1) + "."
                                    + std::to_string(ipv4_info->sin_addr.S_un.S_un_b.s_b2) + "."
                                    + std::to_string(ipv4_info->sin_addr.S_un.S_un_b.s_b3) + "."
                                    + std::to_string(ipv4_info->sin_addr.S_un.S_un_b.s_b4);
                return AddressInfo{ AddressFamily::Ipv4,
                                    std::move(ipv4_address),
                                    from_network_byte_order(static_cast<std::uint16_t>(ipv4_info->sin_port)) };
            }
            case AF_INET6: {
                auto const ipv6_info = reinterpret_cast<sockaddr_in6 const*>(&address);
                auto stream = std::stringstream{};
                stream << std::hex << std::setfill('0');
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[0] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[1] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[2] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[3] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[4] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[5] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[6] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[7] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[8] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[9] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[10] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[11] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[12] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[13] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.u.Byte[14] << std::setw(2)
                       << ipv6_info->sin6_addr.u.Byte[15];
                return AddressInfo{ AddressFamily::Ipv6,
                                    std::move(stream).str(),
                                    from_network_byte_order(static_cast<std::uint16_t>(ipv6_info->sin6_port)) };
            }
        }
        unreachable();
    }
#else
    [[nodiscard]] static AddressInfo extract_adress_info(sockaddr_storage const& address) {
        switch (address.ss_family) {
            case AF_INET: {
                auto const ipv4_info = reinterpret_cast<sockaddr_in const*>(&address);
                auto const ipv4_address_32 = from_network_byte_order(ipv4_info->sin_addr.s_addr);
                static_assert(sizeof(ipv4_address_32) == 4);
                auto ipv4_address = std::to_string((ipv4_address_32 >> 24) & 0xFF) + "."
                                    + std::to_string((ipv4_address_32 >> 16) & 0xFF) + "."
                                    + std::to_string((ipv4_address_32 >> 8) & 0xFF) + "."
                                    + std::to_string((ipv4_address_32 >> 0) & 0xFF);
                return AddressInfo{ AddressFamily::Ipv4,
                                    std::move(ipv4_address),
                                    from_network_byte_order(static_cast<std::uint16_t>(ipv4_info->sin_port)) };
            }
            case AF_INET6: {
                auto const ipv6_info = reinterpret_cast<sockaddr_in6 const*>(&address);
                auto stream = std::stringstream{};
                stream << std::hex << std::setfill('0');
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[0] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[1] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[2] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[3] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[4] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[5] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[6] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[7] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[8] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[9] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[10] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[11] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[12] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[13] << ':';
                stream << std::setw(2) << ipv6_info->sin6_addr.__in6_u.__u6_addr8[14] << std::setw(2)
                       << ipv6_info->sin6_addr.__in6_u.__u6_addr8[15];
                return AddressInfo{ AddressFamily::Ipv6,
                                    std::move(std::move(stream).str()),
                                    from_network_byte_order(static_cast<std::uint16_t>(ipv6_info->sin6_port)) };
            }
        }
        unreachable();
    }
#endif

    AbstractSocket::AbstractSocket(OsSocketHandle const os_socket_handle)
        : m_socket_descriptor{ os_socket_handle, socket_deleter } {
#ifdef _WIN32
        auto local_info = SOCKADDR_STORAGE{};
        auto len = static_cast<int>(sizeof(local_info));
        if (getsockname(os_socket_handle, reinterpret_cast<SOCKADDR*>(&local_info), &len) == socket_error) {
            throw std::runtime_error{ "failed to get local address and port" };
        }
        m_local_address_info = extract_adress_info(local_info);

        // for server sockets, there is no remote address, so we ignore errors here
        auto remote_info = SOCKADDR_STORAGE{};
        len = static_cast<int>(sizeof(remote_info));
        if (getpeername(os_socket_handle, reinterpret_cast<SOCKADDR*>(&remote_info), &len) != socket_error) {
            m_remote_address_info = extract_adress_info(remote_info);
        }
#else
        auto local_info = sockaddr_storage{};
        auto len = static_cast<socklen_t>(sizeof(local_info));
        if (getsockname(os_socket_handle, reinterpret_cast<sockaddr*>(&local_info), &len) == socket_error) {
            throw std::runtime_error{ "failed to get local address and port" };
        }
        m_local_address_info = extract_adress_info(local_info);

        // for server sockets, there is no remote address, so we ignore errors here
        auto remote_info = sockaddr_storage{};
        len = static_cast<socklen_t>(sizeof(local_info));
        if (getpeername(os_socket_handle, reinterpret_cast<sockaddr*>(&remote_info), &len) != socket_error) {
            m_remote_address_info = extract_adress_info(remote_info);
        }
#endif
    }

    // clang-format off
    [[nodiscard]] AbstractSocket::OsSocketHandle initialize_server_socket(
        AddressFamily const address_family,
        std::uint16_t const port
    ) {
        auto const address_infos = get_address_infos(address_family, port);
        auto const socket = create_socket(address_infos);
        bind_socket(socket, address_infos);
        return socket;
    }
    // clang-format on

    [[nodiscard]] static auto
    initialize_client_socket(AddressFamily const address_family, std::string const& host, std::uint16_t const port) {
        auto const address_infos = get_address_infos(address_family, port, host.c_str());
        auto const socket = create_socket(address_infos);
        connect_socket(socket, address_infos);
        return socket;
    }

    void server_listen(
            std::stop_token const& stop_token,
            AbstractSocket::OsSocketHandle const listen_socket,
            std::function<void(ClientSocket)> const& on_connect
    ) {
        while (not stop_token.stop_requested()) {
            auto const can_accept = is_socket_ready(listen_socket, SelectStatusCategory::Read, 100);
            if (not can_accept) {
                continue;
            }

            auto const client_socket = accept(listen_socket, nullptr, nullptr);
            // clang-format off
            assert(
                client_socket != invalid_socket
                and "successful acceptance is guaranteed by previous call to select"
            );
            // clang-format on

            set_all_default_socket_options(client_socket);

            on_connect(ClientSocket{ client_socket });
        }
    }

    ServerSocket::ServerSocket(
            AddressFamily const address_family,
            std::uint16_t const port,
            std::function<void(ClientSocket)> on_connect
    )
        : AbstractSocket{ initialize_server_socket(address_family, port) } {
        assert(m_socket_descriptor.has_value() and "has been set via parent constructor");
        if (listen(m_socket_descriptor.value(), SOMAXCONN) == socket_error) {
            throw std::runtime_error{ "failed to listen on socket" };
        }

        m_listen_thread = std::jthread{ server_listen, m_socket_descriptor.value(), std::move(on_connect) };
    }

    ServerSocket::~ServerSocket() {
        stop();
    }

    void ServerSocket::stop() {
        m_listen_thread.request_stop();
    }

    void ClientSocket::State::clear_queues() {
        receive_tasks.apply([this](std::deque<ReceiveTask>& tasks) {
            while (not tasks.empty()) {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                task.promise.set_value({});
            }
        });
        send_tasks.apply([this](std::deque<SendTask>& tasks) {
            while (not tasks.empty()) {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                task.promise.set_value(0);
            }
        });
    }

    ClientSocket::ClientSocket(OsSocketHandle const os_socket_handle)
        : AbstractSocket{ os_socket_handle },
          m_send_thread{ keep_sending, std::ref(*m_shared_state), m_socket_descriptor.value() },
          m_receive_thread{ keep_receiving, std::ref(*m_shared_state), m_socket_descriptor.value() } { }

    ClientSocket::ClientSocket(AddressFamily const address_family, std::string const& host, std::uint16_t const port)
        : ClientSocket{ initialize_client_socket(address_family, host, port) } { }

    template<typename Queue, typename Element = typename Queue::value_type>
    [[nodiscard]] static std::optional<Element> try_dequeue_task(Synchronized<Queue>& queue) {
        return queue.apply([](Queue& tasks) -> std::optional<Element> {
            if (tasks.empty()) {
                return std::nullopt;
            }
            auto result = std::move(tasks.front());
            tasks.pop_front();
            return result;
        });
    }

    void ClientSocket::keep_sending(State& state, OsSocketHandle const socket) {
        while (state.is_running()) {
            auto processed_send_task = false;
            if (auto send_task = try_dequeue_task(state.send_tasks)) {
                processed_send_task = true;
                if (not process_send_task(socket, *std::move(send_task))) {
                    // connection is dead
                    state.stop_running();
                    break;
                }
            }

            if (not processed_send_task) {
                state.send_tasks.wait(state.data_sent_condition_variable, [&state](std::deque<SendTask> const& tasks) {
                    return not state.is_running() or not tasks.empty();
                });
            }
        }
        state.clear_queues();
    }

    void ClientSocket::keep_receiving(State& state, OsSocketHandle const socket) {
        while (state.is_running()) {
            auto processed_receive_task = false;
            if (auto receive_task = try_dequeue_task(state.receive_tasks)) {
                processed_receive_task = true;
                if (not process_receive_task(socket, *std::move(receive_task))) {
                    // connection is dead
                    state.stop_running();
                    break;
                }
            }

            if (not processed_receive_task) {
                state.receive_tasks.wait(
                        state.data_received_condition_variable,
                        [&state](std::deque<ReceiveTask> const& tasks) {
                            return not state.is_running() or not tasks.empty();
                        }
                );
            }
        }
        state.clear_queues();
    }

    ClientSocket::~ClientSocket() {
        close();
    }

    // clang-format off
    [[nodiscard("discarding the return value may lead to the data to never be transmitted")]]
    std::future<std::size_t> ClientSocket::send(std::vector<std::byte> data) {
        // clang-format on
        auto promise = std::promise<std::size_t>{};
        auto future = promise.get_future();
        auto const return_immediately = m_shared_state->send_tasks.apply([&](std::deque<SendTask>& send_tasks) {
            if (not m_shared_state->is_running()) {
                promise.set_value({});
                m_shared_state->data_sent_condition_variable.notify_one();
                return true;
            }
            send_tasks.emplace_back(std::move(promise), std::move(data));
            return false;
        });

        // todo: can this function be simplified? it was just converted to the new API of Synchronized<T>

        if (return_immediately) {
            return future;
        }

        m_shared_state->data_sent_condition_variable.notify_one();
        return future;
    }

    // clang-format off
    [[nodiscard("discarding the return value may lead to the data to never be transmitted")]]
    std::future<std::size_t> ClientSocket::send(std::string_view const text) {
        // clang-format on
        auto data = std::vector<std::byte>{};
        data.resize(text.length(), std::byte{});
        std::memcpy(data.data(), text.data(), text.size());
        return send(std::move(data));
    }

    [[nodiscard]] std::future<std::vector<std::byte>> ClientSocket::receive(std::size_t const max_num_bytes) {
        auto promise = std::promise<std::vector<std::byte>>{};
        auto future = promise.get_future();
        auto const return_immediately =
                m_shared_state->receive_tasks.apply([&](std::deque<ReceiveTask>& receive_tasks) {
                    if (not m_shared_state->is_running()) {
                        promise.set_value({});
                        m_shared_state->data_sent_condition_variable.notify_one();
                        return true;
                    }
                    receive_tasks.emplace_back(std::move(promise), max_num_bytes);
                    return false;
                });
        if (return_immediately) {
            return future;
        }
        // todo: can this also be simplified?
        m_shared_state->data_received_condition_variable.notify_one();
        return future;
    }

    [[nodiscard]] std::future<std::string> ClientSocket::receive_string(std::size_t const max_num_bytes) {
        return std::async([this, max_num_bytes]() -> std::string {
            auto const data = receive(max_num_bytes).get();
            auto result = std::string{};
            result.resize(data.size());
            std::memcpy(result.data(), data.data(), data.size());
            return result;
        });
    }

    void ClientSocket::close() {
        if (m_shared_state != nullptr) {
            // if this object was moved from, the cleanup will be done by the object
            // this object was moved into
            m_shared_state->stop_running();
            m_shared_state->clear_queues();
        }
    }

    [[nodiscard]] bool ClientSocket::process_receive_task(OsSocketHandle const socket, ReceiveTask task) {
        if (not std::in_range<SendReceiveSize>(task.max_num_bytes)) {
            throw std::runtime_error{ "size of message to be received exceeds allowed maximum" };
        }

        auto receive_buffer = std::vector<std::byte>{};
        receive_buffer.resize(task.max_num_bytes);

        auto const receive_result =
                recv(socket,
                     reinterpret_cast<char*>(receive_buffer.data()),
                     static_cast<SendReceiveSize>(receive_buffer.size()),
                     0);

        if (receive_result == 0) {
            // connection has been gracefully closed => close socket
            task.promise.set_value({});
            return false;
        }

        if (receive_result == socket_error) {
            // connection no longer active
            task.promise.set_value({});
            return false;
        }

        receive_buffer.resize(static_cast<std::size_t>(receive_result));

        task.promise.set_value(std::move(receive_buffer));
        return true;
    }

    [[nodiscard]] bool ClientSocket::process_send_task(OsSocketHandle const socket, SendTask task) {
        if (not std::in_range<SendReceiveSize>(task.data.size())) {
            throw std::runtime_error{ "size of message to be sent exceeds allowed maximum" };
        }
        auto num_bytes_sent = std::size_t{ 0 };
        auto send_pointer = reinterpret_cast<char const*>(task.data.data());
        while (num_bytes_sent < task.data.size()) {
            auto const num_bytes_remaining = task.data.size() - num_bytes_sent;
            auto const result = ::send(socket, send_pointer, static_cast<SendReceiveSize>(num_bytes_remaining), 0);
            if (result == socket_error) {
                // connection no longer active
                task.promise.set_value(0);
                return false;
            }
            send_pointer += result;
            num_bytes_sent += static_cast<std::size_t>(result);
        }
        task.promise.set_value(num_bytes_sent);
        return true;
    }

} // namespace c2k
