#include "cppboot/web/http.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace cppboot::web {

namespace {

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

std::optional<std::string> findHeader(const Headers& headers, std::string_view name) {
    const auto found = std::find_if(headers.begin(), headers.end(), [&](const auto& header) {
        return equalsIgnoringCase(header.first, name);
    });
    if (found == headers.end()) {
        return std::nullopt;
    }
    return found->second;
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Returns nothing for invalid encoding or an encoded null, which could fool code that later
/// passes the value to a C API.
std::optional<std::string> percentDecode(std::string_view text, bool plus_is_space) {
    std::string decoded;
    decoded.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%') {
            if (i + 2 >= text.size()) {
                return std::nullopt;
            }
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0)) {
                return std::nullopt;
            }
            decoded += static_cast<char>(high * 16 + low);
            i += 2;
        } else if (plus_is_space && text[i] == '+') {
            decoded += ' ';
        } else {
            decoded += text[i];
        }
    }
    return decoded;
}

}  // namespace

std::string_view toString(Method method) noexcept {
    switch (method) {
        case Method::Get:
            return "GET";
        case Method::Head:
            return "HEAD";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Patch:
            return "PATCH";
        case Method::Delete:
            return "DELETE";
        case Method::Options:
            return "OPTIONS";
    }
    return "UNKNOWN";
}

std::optional<Method> parseMethod(std::string_view text) noexcept {
    for (const Method method : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Patch,
                                Method::Delete, Method::Options}) {
        if (toString(method) == text) {
            return method;
        }
    }
    return std::nullopt;
}

Result<Request> Request::create(Method method, std::string_view target, Headers headers,
                                std::string body) {
    const auto invalid = [&] {
        return Error{ErrorCode::InvalidArgument, "Invalid request target: " + std::string{target}};
    };

    const auto question_mark = target.find('?');
    const std::string_view path = target.substr(0, question_mark);
    if (path.empty() || path.front() != '/') {
        return invalid();
    }

    Request request;
    request.method_ = method;
    request.path_ = path;
    request.headers_ = std::move(headers);
    request.body_ = std::move(body);

    std::size_t start = 1;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        if (end > start) {
            auto segment = percentDecode(path.substr(start, end - start), false);
            if (!segment) {
                return invalid();
            }
            request.segments_.push_back(*std::move(segment));
        }
        start = end + 1;
    }

    if (question_mark != std::string_view::npos) {
        std::string_view rest = target.substr(question_mark + 1);
        while (!rest.empty()) {
            const auto ampersand = rest.find('&');
            const std::string_view pair = rest.substr(0, ampersand);
            rest = ampersand == std::string_view::npos ? std::string_view{}
                                                       : rest.substr(ampersand + 1);
            if (pair.empty()) {
                continue;
            }

            const auto equals = pair.find('=');
            auto name = percentDecode(pair.substr(0, equals), true);
            auto value = percentDecode(
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1),
                true);
            if (!name || !value) {
                return invalid();
            }
            request.query_.try_emplace(*std::move(name), *std::move(value));
        }
    }
    return request;
}

std::optional<std::string> Request::header(std::string_view name) const {
    return findHeader(headers_, name);
}

std::optional<std::string> Request::query(std::string_view name) const {
    const auto entry = query_.find(name);
    if (entry == query_.end()) {
        return std::nullopt;
    }
    return entry->second;
}

std::optional<std::string> Request::param(std::string_view name) const {
    const auto entry = params_.find(name);
    if (entry == params_.end()) {
        return std::nullopt;
    }
    return entry->second;
}

Response Response::text(std::string body, int status) {
    Response response{status};
    response.body_ = std::move(body);
    response.setHeader("Content-Type", "text/plain; charset=utf-8");
    return response;
}

Response Response::json(std::string body, int status) {
    Response response{status};
    response.body_ = std::move(body);
    response.setHeader("Content-Type", "application/json");
    return response;
}

Response& Response::setHeader(std::string name, std::string value) {
    const auto has_line_break = [](const std::string& text) {
        return text.find_first_of("\r\n") != std::string::npos;
    };
    if (name.empty() || has_line_break(name) || has_line_break(value)) {
        throw Exception(Error{ErrorCode::InvalidArgument, "Invalid response header: " + name});
    }

    const auto found = std::find_if(headers_.begin(), headers_.end(), [&](const auto& header) {
        return equalsIgnoringCase(header.first, name);
    });
    if (found != headers_.end()) {
        found->second = std::move(value);
    } else {
        headers_.emplace_back(std::move(name), std::move(value));
    }
    return *this;
}

std::optional<std::string> Response::header(std::string_view name) const {
    return findHeader(headers_, name);
}

std::string_view statusReason(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

Response errorResponse(int status, std::string_view message) {
    const nlohmann::json body = {{"status", status}, {"error", message}};
    // Replaces invalid UTF-8, since a message may echo what a client sent.
    return Response::json(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                          status);
}

}  // namespace cppboot::web
