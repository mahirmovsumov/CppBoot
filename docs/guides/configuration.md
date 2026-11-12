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
| Command-line argument | `--cppboot.logging.level=debug` | Other arguments are left for your application |

Nested YAML becomes dotted keys, and list items get a numeric index:

```yaml
cppboot:
  logging:
    level: info     # cppboot.logging.level
  hosts:
    - one           # cppboot.hosts.0
    - two           # cppboot.hosts.1
```

### What a settings file can contain

CppBoot reads the part of YAML that settings need, with its own reader:

| Form | Example |
|---|---|
| Nested keys, indented with spaces | `server:` then `  port: 8080` |
| Lists, one item per line | `- 10.0.0.1` |
| Lists on one line | `trusted_proxies: [10.0.0.1, "::1"]` |
| Lists of maps | `- path: /a` then `  limit: 5` |
| Quoted values | `"x: y"`, `'it''s'`, with `\n`, `\t`, `é` and other escapes in double quotes |
| Text over several lines | `\|` keeps line breaks, `>` joins lines, and `-` or `+` after either drops or keeps the final breaks |
| Comments | `port: 8080  # the default` |
| No value | `key:`, `~` or `null`, which leave the setting unset |

A file that uses anything else stops the application with the line number and what's wrong, rather than being read differently from what you meant:

- anchors and aliases (`&` and `*`), tags (`!!str`), `{a: 1}` maps and `?` keys
- a plain value over several lines, which needs `|` or `>`
- a key written twice in the same map
- tabs for indentation
- more than one document in a file

A value containing `: ` or starting with a character YAML reserves, such as `@`, needs quotes.

## Your own settings

You can use any key name for your application's settings:

```yaml
myapp:
  timeout: 30
```

```powershell
$env:CPPBOOT_MYAPP_TIMEOUT = "5"
cppboot run --myapp.timeout=7
```

The environment variable always starts with `CPPBOOT_`, so CppBoot never picks up unrelated variables such as `PATH`. An argument such as `--output=report.txt` is left for your application unless `output` is already a setting, so a command-line argument can only override a key outside `cppboot.` if that key is set in YAML or the defaults.

## Profiles

A profile selects an extra file of settings for an environment. The active profile comes from the first of these that's set:

1. `--cppboot.profile=prod`
2. `CPPBOOT_PROFILE=prod`
3. `cppboot.profile` in `cppboot.yaml` or the defaults

For the `prod` profile, CppBoot reads `cppboot-prod.yaml`. If that file doesn't exist, the application exits with code 78 rather than run with the wrong settings. Profile names can contain only letters, digits, `-` and `_`.

## Environment variable names

A key's variable is its name in capitals, with dots and hyphens as underscores, and `CPPBOOT_` in front if it doesn't start with that already:

| Key | Variable |
|---|---|
| `cppboot.server.port` | `CPPBOOT_SERVER_PORT` |
| `cppboot.server.max_connections` | `CPPBOOT_SERVER_MAX_CONNECTIONS` |
| `myapp.timeout` | `CPPBOOT_MYAPP_TIMEOUT` |

This works whether or not the key also appears in YAML. If a variable matches more than one key that's set, such as both `cppboot.a_b` and `cppboot.a.b`, the application exits with code 78.

## Settings as a struct

Mark a struct with `CPPBOOT_CONFIG` and a prefix, and CppBoot fills it from the settings under that prefix when the application starts. Create `src/mail_settings.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cppboot/cppboot.hpp>
#include <optional>
#include <string>
#include <vector>

struct RetrySettings {
    CPPBOOT_RANGE(1, 10) int attempts{3};
    std::chrono::milliseconds delay{200};
};

struct CPPBOOT_CONFIG("mail") MailSettings {
    CPPBOOT_REQUIRED CPPBOOT_NOT_EMPTY std::string host;   // mail.host
    CPPBOOT_RANGE(1, 65535) int port{25};                  // mail.port
    bool secure{true};                                     // mail.secure
    std::chrono::seconds timeout{30};                      // mail.timeout
    std::optional<std::string> user;                       // mail.user
    std::vector<std::string> copies;                       // mail.copies
    RetrySettings retry;                                   // mail.retry.attempts, mail.retry.delay
};

// A component takes it like any other component.
class CPPBOOT_SERVICE MailModule : public cppboot::Module {
public:
    explicit MailModule(const MailSettings& settings) : settings_(settings) {}

    std::string_view name() const noexcept override { return "mail"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("Mail goes to {}:{}, with {} tries", settings_.host, settings_.port,
                              settings_.retry.attempts);
        return {};
    }

private:
    const MailSettings& settings_;
};
```

```yaml
mail:
  host: smtp.example.com
  timeout: 10s
  copies:
    - audit@example.com
  retry:
    attempts: 5
```

- **A field keeps its value** if its setting isn't set, so the value in the struct is the default. Mark a field `CPPBOOT_REQUIRED` if there's no sensible default.
- **Every source works**, as for any setting: `CPPBOOT_MAIL_HOST` and `CPPBOOT_MAIL_RETRY_ATTEMPTS` set `mail.host` and `mail.retry.attempts`.
- **The struct is a component.** Anything can take it as `const MailSettings&`, and it never changes after startup, so it's safe to read from any thread.

`cppboot add config Mail` writes a struct like this to start from.

### What a field can be

| Type | Setting |
|---|---|
| `bool` | `true`, `yes`, `on` or `1`, and `false`, `no`, `off` or `0` |
| Whole numbers, such as `int` and `std::uint16_t` | A number that fits the type |
| `float`, `double` | A number |
| `std::string` | Any text |
| `std::chrono` durations | A whole number and a unit: `500ms`, `30s`, `5m`, `2h`, `1d`, and also `ns` and `us`. The value has to fit the field's unit exactly, so `1500ms` isn't accepted for `std::chrono::seconds` |
| `std::optional` of those | Empty unless the setting is set |
| `std::vector` of those | A YAML list, or one value with commas, which is how an environment variable sets it: `CPPBOOT_MAIL_COPIES=a@example.com,b@example.com` |
| Another struct of these | Its fields get keys under the field's name, such as `mail.retry.attempts` |

The checks `CPPBOOT_NOT_EMPTY`, `CPPBOOT_LENGTH`, `CPPBOOT_MIN`, `CPPBOOT_MAX` and `CPPBOOT_RANGE` work as they do for [request bodies](web.md#checking-what-clients-send).

### Mistakes stop startup

The application exits with code 78 before it serves anything, and lists everything that's wrong at once:

```text
[critical] [InvalidConfiguration] Invalid settings for MailSettings:
  mail.host: must be set, in a settings file or with the environment variable CPPBOOT_MAIL_HOST
  mail.port: must be from 1 to 65535
  mail.timeout: must be a time with its unit, such as 500ms, 30s, 5m or 2h, but is '10'
  mail.retry.atempts: isn't a setting that MailSettings reads. Did you mean mail.retry.attempts? (binding.cpp:253)
```

- **A setting that no field reads is an error**, because it's nearly always a spelling mistake, which would otherwise leave the real setting at its default without anyone noticing. This covers settings files, defaults and arguments. It can't cover environment variables, since a variable's name doesn't say which key was meant.
- **Values of sensitive settings aren't shown**, so a mistyped password doesn't reach a log.
- **Two structs can't share a prefix.** One prefix can be inside another, such as `shop` and `shop.mail`, and then each struct reads its own settings.
- Prefixes are lower-case names with dots between them, and can't start with `cppboot`.

The test that new projects come with, `tests/components_test.cpp`, fills these structs from `cppboot.yaml` and the environment too, so `cppboot test` catches settings that don't fit them before the application runs.

## Read settings in code

For a setting that doesn't belong in a struct, read it from `Config`:

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
| `cppboot.server.*` | | See the [web server guide](web.md#settings) |
| `cppboot.database.*` | | See the [database guide](database.md#set-up-the-connection) |
| `cppboot.server.tls.*` | | See [HTTPS](web.md#https) |
| `cppboot.security.jwt.*` | | See the [security guide](security.md#settings) |
| `cppboot.rate_limit.*` | | See [rate limits](security.md#rate-limit-settings) |
| `cppboot.actuator.metrics_token` | None | See the [health and metrics guide](actuator.md#who-can-see-metrics) |
| `cppboot.openapi.enabled` | `false` | Serve the API's description at `/openapi.json`. See [OpenAPI](web.md#openapi) |

## Sensitive values

At `debug` level, CppBoot logs all settings at startup. Values are hidden if their key contains `password`, `secret`, `token`, `credential`, `private_key`, `api_key` or `database.url`. Use one of those words in the name of any key that holds a secret.
