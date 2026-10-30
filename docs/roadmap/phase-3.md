# Phase 3: Web framework

**Status:** Complete on Windows. It's finished once CI passes on Linux.

## Steps

1. **Settings and logger injection:** done. Components can take `Config` and `Logger` in their constructors.
2. **HTTP server:** done. It uses Boost.Asio and Beast, kept out of public headers, with a worker pool so that a slow handler can't block other requests.
3. **Routing and middleware:** done. It covers path and query parameters, a request pipeline and error handling.
4. **JSON:** done. `CPPBOOT_JSON` generates conversion to and from your structs.
5. **Controllers:** done. `CPPBOOT_CONTROLLER` and `CPPBOOT_GET` become real routes, created and injected automatically.

## Done when

```text
cppboot create my-api --web
cd my-api
cppboot run
curl localhost:8080/api/users/7
```

The request returns JSON from a controller that was created and injected automatically. The end-to-end test builds and starts this project on every CI run, and the `curl` request was checked by hand on Windows. Real HTTP requests are covered on every platform by the server tests, and generated routes by the controller tests.

## Left for later

- Middleware in projects that use controllers
- HTTPS, CORS and rate limiting
- Request validation beyond types
- WebSockets, which are in the original plan but not needed for 1.0
- OpenAPI documentation

## Next

Phase 5 is the database, starting with PostgreSQL and connection pooling.
