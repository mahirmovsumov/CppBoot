# Web server

`cppboot::web` serves HTTP requests. You define routes with a `Router`, and `ServerModule` runs the server as part of your application.

Today you define routes in code, as shown here. Controllers marked with `CPPBOOT_CONTROLLER`, and automatic JSON conversion, are still to come. See the [Phase 3 roadmap](../roadmap/phase-3.md).

## Example

Link to `cppboot::web`, then create a router and add the server module:

```cpp
#include <cppboot/cppboot.hpp>
#include <cppboot/web/server_module.hpp>
#include <memory>

using namespace cppboot::web;

int main(int argc, char* argv[]) {
    auto router = std::make_shared<Router>();

    router->get("/api/users/{id}", [](const Request& request) {
        return Response::json("{\"id\":" + *request.param("id") + "}");
    });

    cppboot::Application app(argc, argv);
    app.emplaceModule<ServerModule>(router);
    return app.run();
}
```

[The rest-api example](../../examples/rest-api/main.cpp) is a complete version of this. Set up the router before calling `run()`, because it can't change while the server is running.

## Routes

`Router` has `get`, `post`, `put`, `patch` and `del`, plus `add` for any method.

| Pattern | Matches | Parameter |
|---|---|---|
| `/users` | `/users` and `/users/` | |
| `/users/{id}` | `/users/7` | `request.param("id")` is `7` |
| `/users/{id}/posts/{post}` | `/users/7/posts/a%20b` | `request.param("post")` is `a b` |

If more than one route matches, a fixed segment beats a parameter, so `/users/me` wins over `/users/{id}`.

The server responds on its own when no route matches: 404 for an unknown path, and 405 with an `Allow` header for a known path with the wrong method. A `HEAD` request uses the `GET` route.

An invalid pattern, or a second route with the same method and shape, throws `cppboot::Exception` when you add it.

## Requests

```cpp
request.method();                // Method::Get
request.path();                  // "/users/7"
request.param("id");             // A decoded path parameter
request.query("page");           // A decoded query parameter, from ?page=2
request.header("Content-Type");  // Letter case doesn't matter
request.body();                  // The request body
```

`param`, `query` and `header` return a `std::optional<std::string>`.

## Responses

```cpp
return Response::json("{\"ok\":true}");
return Response::text("Created", 201);
return Response{204};

Response response = Response::text("Moved", 302);
response.setHeader("Location", "/new-path");
return response;
```

`setHeader` throws an exception if a name or value contains a line break, which stops client input from forging extra headers.

## Errors

To send an error, throw `HttpError`. Its message goes to the client, as JSON:

```cpp
throw HttpError(404, "No user has that ID");
```

```json
{"error":"No user has that ID","status":404}
```

Any other exception becomes a 500 with a generic message, so that internal details can't reach clients. `ServerModule` writes the real message to the log.

## Middleware

Middleware runs around every request, including those that match no route. The first one added is the outermost. Call `next` to continue, or return a response to stop there:

```cpp
router->use([](const Request& request, const Next& next) {
    if (!request.header("Authorization")) {
        return errorResponse(401, "Sign in first");
    }
    return next(request);
});
```

Middleware sees failures as normal responses, so it can log or change them after calling `next`.

## Settings

| Key | Default | Purpose |
|---|---|---|
| `cppboot.server.host` | `127.0.0.1` | The address to listen on. Use `0.0.0.0` to accept connections from other machines |
| `cppboot.server.port` | `8080` | The port. `0` asks the operating system for a free one |
| `cppboot.server.worker_threads` | `0` | Threads that run handlers. `0` uses one per processor core |
| `cppboot.server.max_body_bytes` | `1048576` | Larger request bodies get a 413 response |
| `cppboot.server.max_connections` | `1024` | Connections beyond this are closed at once |
| `cppboot.server.request_timeout_seconds` | `30` | The longest a client can take to send a request or read a response |
| `cppboot.server.shutdown_timeout_seconds` | `30` | How long shutdown waits for connections to finish |

## Handlers and threads

Handlers run on a pool of worker threads, so a slow handler can't stop the server from serving others. Several handlers can run at once, so shared data needs its own protection, such as a mutex.

Shutdown stops accepting connections, closes idle ones and lets running requests finish. A handler can't be interrupted, so shutdown waits for it, however long it takes. Long-running handlers should have their own time limits.

## Not supported yet

- HTTPS, so run the server behind a TLS proxy if it's exposed to a network
- WebSockets and streaming responses
- `Expect: 100-continue`
