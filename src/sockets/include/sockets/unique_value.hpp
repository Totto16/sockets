#pragma once

#include <functional>
#include <optional>

namespace c2k {
    template<typename T, typename Deleter = std::function<void(T)>>
    class UniqueValue final {
    private:
        std::optional<T> m_value;
        Deleter m_deleter;

    public:
        explicit UniqueValue(T value) : UniqueValue{ std::move(value), Deleter{} } { }
        UniqueValue(T value, Deleter deleter) : m_value{ std::move(value) }, m_deleter{ std::move(deleter) } { }

        UniqueValue(UniqueValue const& other) = delete;
        UniqueValue& operator=(UniqueValue const& other) = delete;

        UniqueValue(UniqueValue&& other) noexcept
            : m_value{ std::exchange(other.m_value, std::nullopt) },
              m_deleter{ std::move(other.m_deleter) } { }

        UniqueValue& operator=(UniqueValue&& other) noexcept {
            if (this == std::addressof(other)) {
                return *this;
            }
            using std::swap;
            swap(m_value, other.m_value);
            swap(m_deleter, other.m_deleter);
            return *this;
        }

        ~UniqueValue() {
            if (m_value.has_value()) {
                m_deleter(m_value.value());
            }
        }

        [[nodiscard]] explicit operator T const&() const {
            return value();
        }

        [[nodiscard]] explicit operator T&() {
            return value();
        }

        [[nodiscard]] bool has_value() const {
            return m_value.has_value();
        }

        [[nodiscard]] T const& value() const {
            return m_value.value();
        }

        [[nodiscard]] T& value() {
            return m_value.value();
        }

        [[nodiscard]] T const& operator*() const {
            return value();
        }

        [[nodiscard]] T& operator*() {
            return value();
        }
    };
} // namespace c2k
