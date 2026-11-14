#include "cppboot/web/router.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace cppboot::web {

Router& Router::add(Method method, std::string_view pattern, Handler handler) {
    const auto invalid = [&](const std::string& reason) {
        return Exception(Error{ErrorCode::InvalidArgument,
                               "Invalid route '" + std::string{pattern} + "': " + reason});
    };

    if (pattern.empty() || pattern.front() != '/') {
        throw invalid("it must start with '/'");
    }
    if (!handler) {
        throw invalid("it has no handler");
    }

    std::vector<Segment> segments;
    std::size_t start = 1;
    while (start <= pattern.size()) {
        const auto end = std::min(pattern.find('/', start), pattern.size());
        if (end > start) {
            const std::string_view text = pattern.substr(start, end - start);
            const bool opens = text.front() == '{';
            const bool closes = text.back() == '}';
            if (opens != closes || (opens && text.size() < 3)) {
                throw invalid("a parameter must look like {name}");
            }

            std::string name{opens ? text.substr(1, text.size() - 2) : text};
            if (opens && std::any_of(segments.begin(), segments.end(), [&](const Segment& other) {
                    return other.is_parameter && other.text == name;
                })) {
                throw invalid("parameter '" + name + "' appears twice");
            }
            segments.push_back(Segment{std::move(name), opens});
        }
        start = end + 1;
    }

    for (const Route& route : routes_) {
        const bool same_shape =
            route.method == method &&
            std::equal(route.segments.begin(), route.segments.end(), segments.begin(),
                       segments.end(), [](const Segment& left, const Segment& right) {
                           return left.is_parameter == right.is_parameter &&
                                  (left.is_parameter || left.text == right.text);
                       });
        if (same_shape) {
            throw invalid("this method and path are already registered");
        }
    }

    const auto parameter_count = static_cast<std::size_t>(
        std::count_if(segments.begin(), segments.end(),
                      [](const Segment& segment) { return segment.is_parameter; }));
    routes_.push_back(Route{method, std::string{pattern}, std::move(segments), parameter_count,
                            std::move(handler)});
    return *this;
}

Router& Router::get(std::string_view pattern, Handler handler) {
    return add(Method::Get, pattern, std::move(handler));
}

Router& Router::post(std::string_view pattern, Handler handler) {
    return add(Method::Post, pattern, std::move(handler));
}

Router& Router::put(std::string_view pattern, Handler handler) {
    return add(Method::Put, pattern, std::move(handler));
}

Router& Router::patch(std::string_view pattern, Handler handler) {
    return add(Method::Patch, pattern, std::move(handler));
}

Router& Router::del(std::string_view pattern, Handler handler) {
    return add(Method::Delete, pattern, std::move(handler));
}

Router& Router::use(Middleware middleware) {
    middleware_.push_back(std::move(middleware));
    return *this;
}

void Router::onUnhandledException(std::function<void(std::string_view)> handler) {
    on_unhandled_ = std::move(handler);
}

Router::Match Router::findRoute(const Request& request) const {
    const std::vector<std::string>& path = request.pathSegments();

    // A literal segment beats a parameter, and earlier segments count first. Both routes have as
    // many segments as the path.
    const auto is_more_specific = [](const Route& candidate, const Route& best) {
        for (std::size_t i = 0; i < candidate.segments.size(); ++i) {
            if (candidate.segments[i].is_parameter != best.segments[i].is_parameter) {
                return best.segments[i].is_parameter;
            }
        }
        return false;
    };

    // HEAD uses the GET route when it has none of its own, as HTTP expects.
    const auto search = [&](Method method) -> const Route* {
        const Route* best = nullptr;
        for (const Route& route : routes_) {
            if (route.method != method || route.segments.size() != path.size()) {
                continue;
            }

            bool matches = true;
            for (std::size_t i = 0; i < path.size() && matches; ++i) {
                matches = route.segments[i].is_parameter || route.segments[i].text == path[i];
            }
            if (matches && (best == nullptr || is_more_specific(route, *best))) {
                best = &route;
            }
        }
        return best;
    };

    Match match{search(request.method()), {}};
    if (match.route == nullptr && request.method() == Method::Head) {
        match.route = search(Method::Get);
    }
    if (match.route != nullptr) {
        return match;
    }

    for (const Method method : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Patch,
                                Method::Delete, Method::Options}) {
        if (search(method) != nullptr ||
            (method == Method::Head && search(Method::Get) != nullptr)) {
            match.allowed.push_back(method);
        }
    }
    return match;
}

Response Router::dispatch(const Request& request, const Match& match) const {
    if (match.route != nullptr) {
        return match.route->handler(request);
    }

    if (match.allowed.empty()) {
        return errorResponse(404, statusReason(404));
    }

    std::string allowed;
    for (const Method method : match.allowed) {
        allowed += allowed.empty() ? "" : ", ";
        allowed += toString(method);
    }
    Response response = errorResponse(405, statusReason(405));
    response.setHeader("Allow", allowed);
    return response;
}

Response Router::run(std::size_t index, const Request& request, const Match& match) const {
    // Handled at every level, so that outer middleware sees each failure as a normal response.
    try {
        if (index == middleware_.size()) {
            return dispatch(request, match);
        }
        // One pointer, so that the std::function for next holds it without allocating.
        const struct {
            const Router& router;
            const Match& match;
            std::size_t next_index;
        } rest{*this, match, index + 1};
        return middleware_[index](request, [&rest](const Request& next_request) {
            return rest.router.run(rest.next_index, next_request, rest.match);
        });
    } catch (const HttpError& error) {
        return errorResponse(error.status(), error.what());
    } catch (const std::exception& exception) {
        if (on_unhandled_) {
            on_unhandled_(exception.what());
        }
    } catch (...) {
        if (on_unhandled_) {
            on_unhandled_("unknown exception");
        }
    }
    return errorResponse(500, statusReason(500));
}

Response Router::handle(Request request) const {
    const Match match = findRoute(request);
    if (match.route != nullptr) {
        request.route_ = match.route->pattern;
        request.params_.clear();
        request.params_.reserve(match.route->parameter_count);
        for (std::size_t i = 0; i < match.route->segments.size(); ++i) {
            const Segment& segment = match.route->segments[i];
            if (segment.is_parameter) {
                // A route can't name a parameter twice, so each name is added once.
                request.params_.emplace_back(segment.text, request.segments_[i]);
            }
        }
    }
    return run(0, request, match);
}

}  // namespace cppboot::web
