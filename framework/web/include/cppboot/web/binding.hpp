#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "cppboot/base/json.hpp"
#include "cppboot/web/http.hpp"

namespace cppboot::web {

/// The JSON type, which keeps fields in the order they were added. See cppboot::Json.
using Json = cppboot::Json;

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// Converts text from a URL. Bad input is the client's mistake, so it's reported as a 400.
template <typename T>
T convertText(std::string_view text, std::string_view description) {
    const auto invalid = [&](std::string_view expected) {
        return HttpError(400, std::string{description} + " must be " + std::string{expected});
    };

    if constexpr (std::is_same_v<T, std::string>) {
        return std::string{text};
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "true" || text == "1") return true;
        if (text == "false" || text == "0") return false;
        throw invalid("true or false");
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        T value{};
        const char* const end = text.data() + text.size();
        const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || parsed_end != end) {
            throw invalid(std::is_integral_v<T> ? "a whole number in range" : "a number");
        }
        return value;
    } else {
        static_assert(!sizeof(T), "Route parameters must be text, a number or a bool");
    }
}

}  // namespace detail

/// Returns a path parameter as T. Generated route code calls this.
template <typename T>
T pathParam(const Request& request, std::string_view name) {
    const auto value = request.param(name);
    if (!value) {
        throw HttpError(500, "Internal Server Error");
    }
    return detail::convertText<T>(*value, "Path parameter '" + std::string{name} + "'");
}

/// Returns a query parameter as T. It's required unless T is a std::optional.
template <typename T>
T queryParam(const Request& request, std::string_view name) {
    const std::string description = "Query parameter '" + std::string{name} + "'";
    const auto value = request.query(name);

    if constexpr (detail::IsOptional<T>::value) {
        if (!value) {
            return std::nullopt;
        }
        return detail::convertText<typename T::value_type>(*value, description);
    } else {
        if (!value) {
            throw HttpError(400, description + " is required");
        }
        return detail::convertText<T>(*value, description);
    }
}

/// Returns the request body parsed from JSON as T. Invalid JSON or a wrong shape is a 400.
template <typename T>
T bodyAs(const Request& request) {
    try {
        return Json::parse(request.body()).template get<T>();
    } catch (const JsonError& error) {
        throw HttpError(400, std::string{"Invalid request body: "} + error.what());
    }
}

/// Return this from a controller to send a value as JSON with a status other than 200:
/// return WithStatus<User>{201, user};
template <typename T>
struct WithStatus {
    int status;
    T value;
};

/// Returns a response with a value converted to JSON.
template <typename T>
Response toJsonResponse(const T& value, int status = 200) {
    return Response::json(Json{value}.dump(), status);
}

template <typename T>
Response toJsonResponse(const WithStatus<T>& result) {
    return toJsonResponse(result.value, result.status);
}

}  // namespace cppboot::web
