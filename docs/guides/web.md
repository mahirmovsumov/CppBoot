# Web server

`cppboot::web` serves HTTP requests. There are two ways to use it. Most applications should write controllers, and you can define routes by hand when you need more control.

To start a project with a controller, run `cppboot create my-api --web`. The [REST API tutorial](../tutorials/rest-api.md) builds a complete one.

## Controllers

Mark a class with `CPPBOOT_CONTROLLER` and a base path, and mark its functions with `CPPBOOT_GET`, `CPPBOOT_POST`, `CPPBOOT_PUT`, `CPPBOOT_PATCH` or `CPPBOOT_DELETE`:

```cpp
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserStore& store);

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
request.clientAddress();         // The client's IP address
```

`param`, `query` and `header` return a `std::optional<std::string>`.

Behind a proxy, `clientAddress` is the proxy's address unless you set `cppboot.server.trusted_proxies`, as the [security guide](security.md#behind-a-proxy) explains. Don't read the client's address from `X-Forwarded-For` yourself, because any client can send that header.

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

In a project with controllers, mark a class with `CPPBOOT_MIDDLEWARE` instead. See the [security guide](security.md#middleware).

## Rate limits

With hand-written routes, add a limit as middleware, which covers every route:

```cpp
#include <cppboot/web/rate_limit.hpp>

router->use(rateLimit({100, std::chrono::seconds{1}}));
```

For one route, keep a `RateLimiter` and call `check`, which returns the response to send if the client is over its limit:

```cpp
auto limiter = std::make_shared<RateLimiter>(RateLimit{5, std::chrono::minutes{1}});

router->post("/login", [limiter](const Request& request) {
    if (auto rejected = limiter->check(request)) {
        return *std::move(rejected);
    }
    return Response::text("Signed in");
});
```

With controllers, use the `CPPBOOT_RATE_LIMIT` mark. The [security guide](security.md#rate-limits) explains how limits work.

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
| `cppboot.server.trusted_proxies` | None | Proxies whose `X-Forwarded-For` header can be believed, separated by commas, such as `10.0.0.5, 10.1.0.0/16`. An invalid entry, or a range covering every address, stops startup |

## HTTPS

Give the server a certificate and its private key, and every connection uses HTTPS:

```yaml
cppboot:
  server:
    host: 0.0.0.0
    port: 8443
    tls:
      certificate_file: /etc/letsencrypt/live/example.com/fullchain.pem
      private_key_file: /etc/letsencrypt/live/example.com/privkey.pem
```

| Key | Default | Purpose |
|---|---|---|
| `cppboot.server.tls.certificate_file` | None | The certificate, followed by any intermediate certificates, in PEM format |
| `cppboot.server.tls.private_key_file` | None | The private key, in PEM format, with no password |
| `cppboot.server.tls.min_version` | `1.2` | The oldest TLS version to accept: `1.2` or `1.3` |

**Keep the key readable only by the account that runs the server.** Anyone who has it can pretend to be your server. Its file path is hidden in configuration logs, but CppBoot can't protect the file itself.

To try it on your own machine, create a certificate that only you will trust:

```powershell
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out cert.pem -days 30 -subj /CN=localhost -addext subjectAltName=DNS:localhost
cppboot run --cppboot.server.tls.certificate_file=cert.pem --cppboot.server.tls.private_key_file=key.pem
curl.exe --cacert cert.pem https://localhost:8080/
```

### Mistakes stop startup

Once any `tls` setting is present, the server either serves HTTPS or doesn't start. It never falls back to plain HTTP. These all stop startup: one file without the other, a missing or unreadable file, a key that doesn't match the certificate, a key with a password, and an unknown version.

An expired certificate is logged as an error, but the server still starts.

### What it accepts

TLS 1.3 and 1.2 only. On TLS 1.2, it only accepts ciphers with forward secrecy and authenticated encryption, and refuses compression and renegotiation. There are no settings to weaken these.

A client that sends plain HTTP to an HTTPS port gets its connection closed, with no response. The server only listens on one port, so to redirect visitors from `http://`, use a proxy or a small separate listener.

To tell browsers to always use HTTPS, add this header with [middleware](#middleware). Browsers remember it for the time you give, so make sure HTTPS works first:

```cpp
cppboot::web::Response response = next(request);
response.setHeader("Strict-Transport-Security", "max-age=31536000");
return response;
```

### Renewing a certificate

The server reads its certificate once, at startup. **After renewing, restart it,** or clients will start refusing it when the old certificate expires. A graceful restart lets running requests finish, but new connections fail until the server is back.

### What it costs

Every connection costs more with HTTPS, so a server's limits are lower. The [benchmarks](../benchmarks.md#https) have the details:

- **Memory:** an open connection used 76 to 95 KB, against 4 to 6 KB for plain HTTP. With a 2 GB limit on one core, 25,000 open HTTPS connections worked and 30,000 killed the server, so set `cppboot.server.max_connections` below what your memory can hold.
- **New connections:** these cost the most, because of the handshake. Certificates using an elliptic curve (ECDSA) are faster than RSA, so prefer them.
- **Reused connections:** these cost much less. Small responses lost about 20% of their throughput, so clients that keep connections open are much cheaper to serve.

All encryption runs on the server's one network thread, so more cores don't make it faster yet. A flood of handshakes slows requests on open connections, and rate limits can't help, because they apply after the handshake.

### Built in, or a proxy?

Serving HTTPS directly means one less thing to run. A proxy such as nginx or Caddy is still better if you want automatic certificate renewal, HTTP/2, redirects from `http://`, or several sites on one address. If you use one, list it in `cppboot.server.trusted_proxies`.

## Handlers and threads

Handlers run on a pool of worker threads, so a slow handler can't stop the server from serving others. Several handlers can run at once, so shared data needs its own protection, such as a mutex.

Shutdown stops accepting connections, closes idle ones and lets running requests finish. A handler can't be interrupted, so shutdown waits for it, however long it takes. Long-running handlers should have their own time limits.

## Not supported yet

- HTTP/2
- Loading a new certificate without a restart, several certificates on one server, and client certificates
- WebSockets and streaming responses
- `Expect: 100-continue`
