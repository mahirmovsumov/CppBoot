#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/web/http.hpp"

namespace cppboot::web {

using Handler = std::function<Response(const Request&)>;
using Next = std::function<Response(const Request&)>;

/// Runs around every request. Call next to continue, or return a response to stop there.
using Middleware = std::function<Response(const Request&, const Next&)>;

/// Matches requests to handlers. Set it up before the server starts, because it isn't safe to
/// change while requests are running. Handling a request is safe from any number of threads.
class Router {
public:
    /// Adds a route such as "/users/{id}". An invalid pattern or a route with the same shape and
    /// method as an existing one throws cppboot::Exception.
    Router& add(Method method, std::string_view pattern, Handler handler);

    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& patch(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);

    /// Adds middleware. The first added is the outermost, and it runs for every request,
    /// including those that match no route.
    Router& use(Middleware middleware);

    /// Receives a description of each unexpected exception. Clients only see a generic message.
    void onUnhandledException(std::function<void(std::string_view)> handler);

    /// Returns the response to a request. This doesn't throw: an HttpError becomes its response,
    /// and any other exception becomes a 500.
    Response handle(Request request) const;

private:
    struct Segment {
        std::string text;
        bool is_parameter;
    };

    struct Route {
        Method method;
        std::string pattern;
        std::vector<Segment> segments;
        Handler handler;
    };

    struct Match {
        const Route* route;
        std::vector<Method> allowed;
    };

    Match findRoute(const Request& request) const;
    Response run(std::size_t index, const Request& request, const Match& match) const;
    Response dispatch(const Request& request, const Match& match) const;

    std::vector<Route> routes_;
    std::vector<Middleware> middleware_;
    std::function<void(std::string_view)> on_unhandled_;
};

}  // namespace cppboot::web
