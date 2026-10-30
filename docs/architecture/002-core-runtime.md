# 002: Core runtime

**Status:** Accepted

## Lifecycle

```text
Initialized -> Configuring -> RegisteringModules -> CreatingContext
            -> Starting -> Running -> Stopping -> Terminated
```

Any state before `Failed` can move to `Failed`, and `Failed` can move only to `Terminated`. `Application::run()` always performs cleanup, whichever path it took.

## Exit codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `kExitSuccess` | Clean shutdown |
| 1 | `kExitFailure` | Startup failure, worker failure or unexpected exception |
| 78 | `kExitConfigurationError` | Invalid configuration, matching `EX_CONFIG` from `sysexits.h` |

## Shutdown order

1. Stop modules in the reverse of their start order.
2. Request all workers to stop, then wait for them.
3. Remove the signal handler.
4. Flush logs.
5. Destroy modules, then the context, logger and configuration.

Requests can't be drained yet because there is no web module. A configurable shutdown timeout will arrive with it, since a timeout means nothing until there's work to cut short.

## Configuration

Sources from lowest to highest precedence:

1. Defaults supplied in code
2. `cppboot.yaml`, which is optional
3. `cppboot-<profile>.yaml`, which must exist if that profile is selected
4. Environment variables starting with `CPPBOOT_`
5. Command-line arguments of the form `--cppboot.some.key=value`, or `--other.key=value` if `other.key` is already set

The profile comes from `--cppboot.profile`, then `CPPBOOT_PROFILE`, then the `cppboot.profile` setting.

An environment variable first looks for an existing key that it matches, so `CPPBOOT_RUNTIME_GRACEFUL_SHUTDOWN_TIMEOUT` sets `cppboot.runtime.graceful_shutdown_timeout`, and `CPPBOOT_MYAPP_TIMEOUT` sets an existing `myapp.timeout`. If there's no match, each underscore becomes a dot. If it matches more than one key, startup fails.

Setting `cppboot.runtime.shutdown_after_start` to `true` makes the application stop as soon as it's running, which is useful for startup checks in tests and CI.

`Config::describe()` hides values whose keys contain a sensitive word such as `password`, `secret` or `token`.

## Global state

The framework has none, with one exception: operating system signal handlers receive no user data pointer, so the platform layer keeps the minimum needed. Loggers aren't added to spdlog's global registry.
