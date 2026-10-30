# Web server

`cppboot::web` serves HTTP requests. There are two ways to use it. Most applications should write controllers, and you can define routes by hand when you need more control, or need middleware.

To start a project with a controller, run `cppboot create my-api --web`. The [REST API tutorial](../tutorials/rest-api.md) builds a complete one.

## Controllers

Mark a class with `CPPBOOT_CONTROLLER` and a base path, and mark its functions with `CPPBOOT_GET`, `CPPBOOT_POST`, `CPPBOOT_PUT`, `CPPBOOT_PATCH` or `CPPBOOT_DELETE`:

```cpp
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(std::shared_ptr<UserStore> store);

    CPPBOOT_GET("/{id}") User get(int id) const;
    CPPBOOT_POST("/") cppboot::web::WithStatus<User> create(const NewUser& details);
};
```

A controller is a component, so its constructor can take other components, settings and the logger. There's one instance, and several requests can call it at once, so shared data needs its own protection.

### Function parameters

CppBoot decides where each parameter comes from:

| Parameter | Source | If it's invalid or missing |
|---|---|---|
| Its name matches `{name}` in the path | The path | 400 |
| Text, a number or a `bool` | The query string, by parameter name | 400 |
| `std::optional` of those | The query string, and can be missing | 400 if invalid |
| `const cppboot::web::Request&` | The whole request | |
| Any other type | The request body, parsed from JSON | 400 |

A function can have one body parameter. A path parameter with no matching function parameter fails the build, as does a mistake such as a private route function.

### Return values

| Return type | Response |
|---|---|
| `void` | 204 with no body |
| `cppboot::web::Response` | Sent as it is |
| `cppboot::web::WithStatus<T>` | The value as JSON, with that status |
| Anything else | The value as JSON, with status 200 |

To send an error, throw `HttpError`, as described under [Errors](#errors).

## JSON

Mark a struct with `CPPBOOT_JSON`, and CppBoot generates its conversion to and from JSON:

```cpp
struct CPPBOOT_JSON User {
    int id{0};
    std::string name;
    std::optional<std::string> nickname;
    Address address;            // Another CPPBOOT_JSON struct
    std::vector<int> scores;
};
```

- JSON keys match the field names, in the same order.
- Every field is required when reading, unless it's a `std::optional`.
- An empty `std::optional` is written as `null`.
- Fields can be other marked structs, and standard containers of supported types.
- The struct needs a default constructor and public fields, and can't have base classes.

`cppboot::web::Json` is `nlohmann::ordered_json`, for working with JSON directly. See the [nlohmann/json documentation](https://json.nlohmann.me).

Generated conversions are only visible to generated code, so functions in your headers can't convert a marked struct themselves. To send one, return it from a controller function.

## Defining routes by hand

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
- Middleware in a project that uses controllers, because the generated code creates the router
- WebSockets and streaming responses
- `Expect: 100-continue`
