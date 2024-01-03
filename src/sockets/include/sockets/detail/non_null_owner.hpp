#pragma once

#include <cassert>
#include <concepts>
#include <iostream>
#include <memory>
#include <utility>

namespace c2k {
    template<std::default_initializable T>
    class NonNullOwner final {
        template<typename U, typename... Args>
        friend NonNullOwner<U> make_non_null_owner(Args&&... args);

    private:
        std::unique_ptr<T> m_owned;

        explicit NonNullOwner(std::unique_ptr<T> owned) : m_owned{ std::move(owned) } {
            assert(m_owned != nullptr);
        }

    public:
        NonNullOwner(NonNullOwner const& other) = delete;
        NonNullOwner& operator=(NonNullOwner const& other) = delete;

        NonNullOwner(NonNullOwner&& other) noexcept : m_owned{ std::exchange(other.m_owned, std::make_unique<T>()) } { }

        NonNullOwner& operator=(NonNullOwner&& other) noexcept {
            if (this == std::addressof(other)) {
                return *this;
            }
            m_owned = std::exchange(other.m_owned, std::make_unique<T>());
            return *this;
        }

        [[nodiscard]] T const& operator*() const {
            return *m_owned;
        }

        [[nodiscard]] T& operator*() {
            return *m_owned;
        }

        [[nodiscard]] T const* operator->() const {
            return m_owned.get();
        }

        [[nodiscard]] T* operator->() {
            return m_owned.get();
        }
    };

    template<typename T, typename... Args>
    [[nodiscard]] NonNullOwner<T> make_non_null_owner(Args&&... args) {
        static_assert(sizeof...(args) > 0);
        return NonNullOwner{ std::make_unique<T>(std::forward<Args>(args)...) };
    }
} // namespace c2k