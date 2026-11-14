#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppboot/base/annotations.hpp"
#include "cppboot/base/result.hpp"

namespace cppboot::web {

enum class Method { Get, Head, Post, Put, Patch, Delete, Options };

std::string_view toString(Method method) noexcept;
std::optional<Method> parseMethod(std::string_view text) noexcept;

using Headers = std::vector<std::pair<std::string, std::string>>;

/// A request's headers, kept together in one block of memory that the request owns, rather than
/// in two strings for each header. Copying or moving it leaves nothing pointing at the original.
class RequestHeaders {
public:
    RequestHeaders() = default;
    /// Not explicit, so that a Headers can be passed wherever a request's headers are wanted.
    RequestHeaders(const Headers& headers);

    /// Makes room for headers whose names and values add up to this many characters.
    void reserve(std::size_t count, std::size_t characters);
    void add(std::string_view name, std::string_view value);

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    /// Returns the first header with a name, ignoring letter case. It's valid until these headers
    /// change or are destroyed.
    std::optional<std::string_view> find(std::string_view name) const noexcept
        CPPBOOT_LIFETIMEBOUND;

    /// Calls visit(name, value) for each header, in the order they were added.
    template <typename Visitor>
    void forEach(Visitor&& visit) const {
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const Entry entry = entryAt(offset);
            visit(entry.name, entry.value);
            offset = entry.end;
        }
    }

private:
    struct Entry {
        std::string_view name;
        std::string_view value;
        /// Where the next entry starts.
        std::size_t end;
    };

    Entry entryAt(std::size_t offset) const noexcept;

    /// Each header is the length of its name, the length of its value, then both texts. Lengths
    /// rather than pointers, so that a copy is complete in itself.
    std::string storage_;
    std::size_t count_{0};
};

/// An HTTP request. Handlers receive it after routing, with its path parameters filled in.
class Request {
public:
    /// Fails if the target has invalid percent-encoding or encodes a null character.
    static Result<Request> create(Method method, std::string_view target,
                                  RequestHeaders headers = {}, std::string body = {});

    Method method() const noexcept { return method_; }
    const std::string& path() const noexcept CPPBOOT_LIFETIMEBOUND { return path_; }
    const std::string& body() const noexcept CPPBOOT_LIFETIMEBOUND { return body_; }
    /// A copy of every header. Use header() or headerView() to read one.
    Headers headers() const;

    /// Returns a header by name, ignoring letter case.
    std::optional<std::string> header(std::string_view name) const;

    /// The same, without copying it. It's valid for as long as this request is, so copy it to keep
    /// it for longer.
    std::optional<std::string_view> headerView(std::string_view name) const noexcept
        CPPBOOT_LIFETIMEBOUND {
        return headers_.find(name);
    }

    /// Returns a decoded query parameter, such as "page" from /users?page=2.
    std::optional<std::string> query(std::string_view name) const;

    /// Returns a decoded path parameter, such as "id" from /users/{id}.
    std::optional<std::string> param(std::string_view name) const;

    const std::vector<std::string>& pathSegments() const noexcept CPPBOOT_LIFETIMEBOUND {
        return segments_;
    }

    /// The pattern of the matched route, such as "/users/{id}", or empty if no route matched. Use
    /// this rather than the path in logs and metrics, because clients choose the paths they send.
    const std::string& route() const noexcept CPPBOOT_LIFETIMEBOUND { return route_; }

    /// The address of the client, or empty if the request didn't come from a server. Behind a
    /// proxy, this needs cppboot.server.trusted_proxies, or it's the address of the proxy.
    const std::string& clientAddress() const noexcept CPPBOOT_LIFETIMEBOUND {
        return client_address_;
    }
    void setClientAddress(std::string address) { client_address_ = std::move(address); }

    /// Returns true once a handler should give up: the server is shutting down and has run out of
    /// patience, or the request's deadline has passed. A handler that loops or waits should check
    /// this, because nothing can stop it from outside. The server doesn't stop a handler that
    /// ignores this. It only stops caring about the result when its client's timeout ends.
    bool shouldStop() const noexcept {
        return stop_token_.stop_requested() || std::chrono::steady_clock::now() >= deadline_;
    }

    /// Requested when the server wants this handler to stop for shutdown. Use it to wake up from a
    /// wait, such as with std::condition_variable_any. It doesn't cover the deadline.
    const std::stop_token& stopToken() const noexcept CPPBOOT_LIFETIMEBOUND { return stop_token_; }

    /// When this request should be finished by. See cppboot.server.handler_deadline_seconds.
    std::chrono::steady_clock::time_point deadline() const noexcept { return deadline_; }

    void setStopToken(std::stop_token token) noexcept { stop_token_ = std::move(token); }
    void setDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
        deadline_ = deadline;
    }

private:
    friend class Router;

    Request() = default;

    /// A request has a handful of these at most, so a list is smaller and quicker than a map.
    using Values = std::vector<std::pair<std::string, std::string>>;

    static const std::string* findValue(const Values& values, std::string_view name) noexcept;

    Method method_{Method::Get};
    std::string path_;
    std::vector<std::string> segments_;
    Values query_;
    Values params_;
    std::string route_;
    std::string client_address_;
    std::stop_token stop_token_;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    RequestHeaders headers_;
    std::string body_;
};

/// An HTTP response.
class Response {
public:
    explicit Response(int status = 200) : status_(status) {}

    static Response text(std::string body, int status = 200);
    static Response json(std::string body, int status = 200);

    int status() const noexcept { return status_; }
    const std::string& body() const& noexcept CPPBOOT_LIFETIMEBOUND { return body_; }
    /// Takes the body out of a response that's about to go away, without copying it.
    std::string body() && noexcept { return std::move(body_); }
    const Headers& headers() const noexcept CPPBOOT_LIFETIMEBOUND { return headers_; }

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
