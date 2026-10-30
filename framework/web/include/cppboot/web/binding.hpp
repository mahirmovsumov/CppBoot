#pragma once

#include <charconv>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "cppboot/web/http.hpp"

// std::optional becomes null when empty, and reads null as empty.
namespace nlohmann {
template <typename T>
struct adl_serializer<std::optional<T>> {
    template <typename BasicJson>
    static void to_json(BasicJson& json, const std::optional<T>& value) {
        if (value) {
            json = *value;
        } else {
            json = nullptr;
        }
    }

    template <typename BasicJson>
    static void from_json(const BasicJson& json, std::optional<T>& value) {
        if (json.is_null()) {
            value.reset();
        } else {
            value = json.template get<T>();
        }
    }
};
}  // namespace nlohmann

namespace cppboot::web {

/// The JSON type. It's nlohmann::json, keeping fields in the order they were added. This is the
/// one third-party type in CppBoot's public API, because applications work with it directly.
using Json = nlohmann::ordered_json;

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
    } catch (const nlohmann::json::exception& exception) {
        // Removes the library's own prefix, such as "[json.exception.out_of_range.403] ".
        std::string_view reason = exception.what();
        if (const auto prefix_end = reason.find("] "); prefix_end != std::string_view::npos) {
            reason.remove_prefix(prefix_end + 2);
        }
        throw HttpError(400, "Invalid request body: " + std::string{reason});
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
    const Json json = value;
    return Response::json(json.dump(-1, ' ', false, Json::error_handler_t::replace), status);
}

template <typename T>
Response toJsonResponse(const WithStatus<T>& result) {
    return toJsonResponse(result.value, result.status);
}

}  // namespace cppboot::web
