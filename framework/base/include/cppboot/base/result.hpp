#pragma once

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "cppboot/base/error.hpp"
#include "cppboot/base/exceptions.hpp"

namespace cppboot {

/// Holds either a value or an Error. Reading the wrong alternative is a bug and throws Exception.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result cannot hold a reference");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result cannot hold an Error value");

public:
    using value_type = T;

    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}

    bool hasValue() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() & {
        requireValue();
        return std::get<0>(state_);
    }

    const T& value() const& {
        requireValue();
        return std::get<0>(state_);
    }

    T&& value() && {
        requireValue();
        return std::get<0>(std::move(state_));
    }

    const Error& error() const {
        if (hasValue()) {
            throw Exception(Error{ErrorCode::InvalidState, "Result holds a value, not an error"});
        }
        return std::get<1>(state_);
    }

private:
    void requireValue() const {
        if (!hasValue()) {
            throw Exception(std::get<1>(state_));
        }
    }

    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    bool hasValue() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return hasValue(); }

    void value() const {
        if (error_) {
            throw Exception(*error_);
        }
    }

    const Error& error() const {
        if (!error_) {
            throw Exception(Error{ErrorCode::InvalidState, "Result holds a value, not an error"});
        }
        return *error_;
    }

private:
    std::optional<Error> error_;
};

}  // namespace cppboot
