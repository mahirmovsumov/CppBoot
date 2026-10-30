# Phase 3: Web framework

**Status:** In progress.

## Steps

1. **Settings and logger injection:** done. Components can take `Config` and `Logger` in their constructors.
2. **HTTP server:** Boost.Asio and Beast, kept out of public headers, with a worker pool so that a slow handler can't block other requests.
3. **Routing and middleware:** path and query parameters, a request pipeline and error handling.
4. **JSON:** conversion to and from your classes, using the code generator.
5. **Controllers:** `CPPBOOT_CONTROLLER` and `CPPBOOT_GET` become real routes.

## Done when

```text
cppboot create my-api --web
cd my-api
cppboot run
curl localhost:8080/api/users/7
```

The request returns JSON from a controller that was created and injected automatically. This has to pass on Linux and Windows.
