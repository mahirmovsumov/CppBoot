# Modules and workers

A module is a unit of functionality with its own startup and shutdown. Modules are how an application plugs its work into the CppBoot lifecycle.

## Write a module

Derive from `cppboot::Module`, and give the module a unique name and a `start()` function:

```cpp
class CacheModule : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "cache"; }

    std::vector<std::string> dependencies() const override { return {"database"}; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("Cache ready");
        return {};
    }

    void stop() noexcept override {
        // Release what start() acquired.
    }
};
```

| Function | Required | Purpose |
|---|---|---|
| `name()` | Yes | The unique name other modules use to depend on this one |
| `start()` | Yes | Starts the module. Return `{}` on success or an `Error` on failure |
| `dependencies()` | No | The modules that must start first |
| `stop()` | No | Releases resources. It must not throw |

Register modules before calling `run()`, in any order:

```cpp
cppboot::Application app(argc, argv);
app.emplaceModule<CacheModule>();
app.emplaceModule<DatabaseModule>();
return app.run();
```

## Startup and shutdown order

Modules start with dependencies first, and modules with no ordering constraint between them keep their registration order. They stop in the reverse of the order they started.

The application refuses to start, and starts no modules, if it finds any of these:

- Two modules with the same name
- A dependency on a module that isn't registered
- A dependency cycle

## When a module fails

If `start()` returns an `Error` or throws an exception, startup stops there. Modules that already started are stopped in reverse order, and the application exits with code 1. The failed module's own `stop()` isn't called, so `start()` must clean up after itself before it fails.

## The application context

`start()` receives an `ApplicationContext`. It stays valid until the module has been destroyed, so a module can keep a reference to it.

| Function | Use |
|---|---|
| `config()` | Read settings |
| `logger()` | Write log messages |
| `workers()` | Start background threads |
| `requestShutdown()` | Ask the application to stop, from any thread |

## Run background work

Don't block in `start()`, because the application can't finish starting until it returns. Start a worker instead:

```cpp
cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
    return context.workers().spawn("poller", [&logger = context.logger()](std::stop_token token) {
        while (!token.stop_requested()) {
            logger.info("Polling");
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    });
}
```

A worker should check its `std::stop_token` regularly, because shutdown waits for every worker to return. A long sleep delays shutdown, so [the hello-world example](../../examples/hello-world/main.cpp) waits on a condition variable that wakes as soon as a stop is requested.

If a worker throws an exception, the application logs it, shuts down and exits with code 1.

## Exit codes

`Application::run()` returns the code to use as the process exit code:

| Code | Constant | Meaning |
|---|---|---|
| 0 | `kExitSuccess` | Clean shutdown |
| 1 | `kExitFailure` | A module or worker failed, or an unexpected exception occurred |
| 78 | `kExitConfigurationError` | The configuration is invalid |
