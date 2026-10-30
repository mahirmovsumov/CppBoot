# Configuration

CppBoot combines settings from several sources into one set of flat keys, such as `cppboot.logging.level`.

## Where settings come from

If a key is set in more than one place, the source that's lower in this table wins:

| Source | Example | Notes |
|---|---|---|
| Defaults in code | `ApplicationOptions::default_settings` | |
| Base file | `cppboot.yaml` | Optional, read from the working directory |
| Profile file | `cppboot-prod.yaml` | Must exist if that profile is selected |
| Environment variable | `CPPBOOT_LOGGING_LEVEL=debug` | Only names starting with `CPPBOOT_` are read |
| Command-line argument | `--cppboot.logging.level=debug` | Only arguments starting with `--cppboot.` are read |

Nested YAML becomes dotted keys, and list items get a numeric index:

```yaml
cppboot:
  logging:
    level: info     # cppboot.logging.level
  hosts:
    - one           # cppboot.hosts.0
    - two           # cppboot.hosts.1
```

## Overridable keys

Environment variables and command-line arguments can only set keys that start with `cppboot.`. A key such as `myapp.timeout` can be set in YAML or in code, but it can't be overridden for a single run. To allow overrides, put the key under `cppboot:`.

## Profiles

A profile selects an extra file of settings for an environment. The active profile comes from the first of these that's set:

1. `--cppboot.profile=prod`
2. `CPPBOOT_PROFILE=prod`
3. `cppboot.profile` in `cppboot.yaml` or the defaults

For the `prod` profile, CppBoot reads `cppboot-prod.yaml`. If that file doesn't exist, the application exits with code 78 rather than run with the wrong settings. Profile names can contain only letters, digits, `-` and `_`.

## Environment variable names

CppBoot first looks for an existing key that matches the variable. This is how names containing underscores work: if `cppboot.yaml` sets `cppboot.runtime.graceful_shutdown_timeout`, then `CPPBOOT_RUNTIME_GRACEFUL_SHUTDOWN_TIMEOUT` overrides it.

If no key matches, each underscore becomes a dot, so `CPPBOOT_SERVER_PORT` sets `cppboot.server.port`. If a variable matches more than one key, the application exits with code 78.

## Read settings in code

```cpp
const auto& config = context.config();

std::string host = config.getString("cppboot.server.host", "127.0.0.1");

auto port = config.getInt("cppboot.server.port", 8080);
if (!port) {
    return port.error();  // The value isn't an integer.
}

auto enabled = config.getBool("cppboot.server.enabled", true);
if (!enabled) {
    return enabled.error();
}
```

`getBool` accepts `true`, `yes`, `on` and `1`, or `false`, `no`, `off` and `0`, in any letter case.

## Settings CppBoot reads

| Key | Default | Purpose |
|---|---|---|
| `cppboot.application.name` | `cppboot` | The name shown in log messages |
| `cppboot.logging.level` | `info` | `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off` |
| `cppboot.logging.file` | None | A log file to write in addition to the console |
| `cppboot.profile` | None | The active profile |
| `cppboot.runtime.shutdown_after_start` | `false` | Stop as soon as the application is running, for startup checks |

## Sensitive values

At `debug` level, CppBoot logs all settings at startup. Values are hidden if their key contains `password`, `secret`, `token`, `credential`, `private_key` or `api_key`. Use one of those words in the name of any key that holds a secret.
