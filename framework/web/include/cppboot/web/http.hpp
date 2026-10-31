#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::web {

enum class Method { Get, Head, Post, Put, Patch, Delete, Options };

std::string_view toString(Method method) noexcept;
std::optional<Method> parseMethod(std::string_view text) noexcept;

using Headers = std::vector<std::pair<std::string, std::string>>;

/// An HTTP request. Handlers receive it after routing, with its path parameters filled in.
class Request {
public:
    /// Fails if the target has invalid percent-encoding or encodes a null character.
    static Result<Request> create(Method method, std::string_view target, Headers headers = {},
                                  std::string body = {});

    Method method() const noexcept { return method_; }
    const std::string& path() const noexcept { return path_; }
    const std::string& body() const noexcept { return body_; }
    const Headers& headers() const noexcept { return headers_; }

    /// Returns a header by name, ignoring letter case.
    std::optional<std::string> header(std::string_view name) const;

    /// Returns a decoded query parameter, such as "page" from /users?page=2.
    std::optional<std::string> query(std::string_view name) const;

    /// Returns a decoded path parameter, such as "id" from /users/{id}.
    std::optional<std::string> param(std::string_view name) const;

    const std::vector<std::string>& pathSegments() const noexcept { return segments_; }

    /// The pattern of the matched route, such as "/users/{id}", or empty if no route matched. Use
    /// this rather than the path in logs and metrics, because clients choose the paths they send.
    const std::string& route() const noexcept { return route_; }

    /// The address of the client, or empty if the request didn't come from a server. Behind a
    /// proxy, this needs cppboot.server.trusted_proxies, or it's the address of the proxy.
    const std::string& clientAddress() const noexcept { return client_address_; }
    void setClientAddress(std::string address) { client_address_ = std::move(address); }

private:
    friend class Router;

    Request() = default;

    Method method_{Method::Get};
    std::string path_;
    std::vector<std::string> segments_;
    std::map<std::string, std::string, std::less<>> query_;
    std::map<std::string, std::string, std::less<>> params_;
    std::string route_;
    std::string client_address_;
    Headers headers_;
    std::string body_;
};

/// An HTTP response.
class Response {
public:
    explicit Response(int status = 200) : status_(status) {}

    static Response text(std::string body, int status = 200);
    static Response json(std::string body, int status = 200);

    int status() const noexcept { return status_; }
    const std::string& body() const noexcept { return body_; }
    const Headers& headers() const noexcept { return headers_; }

    /// Sets a header, replacing any with the same name. Line breaks in a name or value would let
    /// one header forge others, so they throw cppboot::Exception.
    Response& setHeader(std::string name, std::string value);

    std::optional<std::string> header(std::string_view name) const;

private:
    int status_;
    std::string body_;
    Headers headers_;
};

/// Throw from a handler or middleware to send an error response. Its message goes to the client.
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& message)
        : std::runtime_error(message), status_(status) {}

    int status() const noexcept { return status_; }

private:
    int status_;
};

/// Returns the standard reason for a status code, such as "Not Found", or "Unknown".
std::string_view statusReason(int status) noexcept;

/// Returns a JSON error response in CppBoot's standard form.
Response errorResponse(int status, std::string_view message);

}  // namespace cppboot::web
