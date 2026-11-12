# 017: Configuration

**Status:** Accepted.

This record explains how settings are read, merged and handed to code, and why each part works the way it does. The [configuration guide](../guides/configuration.md) explains how to use them.

## The shape: flat keys, read once

`cppboot::config::Config` holds every setting as a flat key and a text value, such as `cppboot.server.port` and `8080`. It's built once, by `Config::load`, before anything else in the application exists, and never changes after that.

- **Flat keys** make every source look the same. YAML, environment variables and arguments all end up as `key = text`, so precedence is one rule: a later source replaces a key. A tree would need rules for merging a map with a value, or a list with a shorter list.
- **Text values** leave the type to the reader. `getInt`, `getBool` and settings structs convert when they read, and report a value that doesn't fit as an error with the key's name.
- **Read once, never changed** means `Config` needs no lock, and every thread can read it ([006](006-concurrency-model.md#whats-safe-from-any-thread)). Settings that change while the application runs would need every reader to cope with a change at any moment, which almost nothing in a server can do safely. Restarting is the way to change settings.

## Sources and precedence

`ConfigSources` lists the inputs, from lowest to highest precedence:

```mermaid
flowchart LR
    defaults["1 Defaults<br/>ApplicationOptions"] --> base["2 cppboot.yaml"] --> profile["3 cppboot-PROFILE.yaml"] --> env["4 CPPBOOT_ variables"] --> args["5 --key=value"]
```

`Config::load` works through them in that order:

1. It copies the defaults.
2. It reads `cppboot.yaml` if it exists. A missing base file is fine, because an application can run on defaults.
3. It reads the arguments, because the profile can come from one, and chooses the profile: `--cppboot.profile`, then `CPPBOOT_PROFILE`, then `cppboot.profile` from the steps above.
4. It reads `cppboot-<profile>.yaml`. **A selected profile's file must exist.** Running without it would quietly use the base settings, such as a development database, in production.
5. It applies the environment, then the arguments.

Each failure is a `Result` error, which `Application` reports as `ErrorCode::InvalidConfiguration` with exit code 78, the `EX_CONFIG` code of `sysexits.h`. A service manager can tell from it that restarting won't help ([002](002-core-runtime.md#exit-codes)).

### Why this order

It follows how settings are usually managed: the code knows safe defaults, the project's files describe each environment, and the machine it runs on overrides with the environment, which is where secrets and anything an orchestrator sets belong. An argument is the most specific, typed by a person for one run, so it wins.

Spring Boot uses the same order, so developers who know it don't have to learn another.

## Environment variables

A key's variable is the key in capitals, with dots and hyphens as underscores, and `CPPBOOT_` in front unless it's already there: `environmentVariableFor("myapp.timeout")` is `CPPBOOT_MYAPP_TIMEOUT`.

- **Only `CPPBOOT_` variables are read.** Reading every variable would let `PATH` or `HOME` become settings by accident.
- **The mapping loses information.** `cppboot.a_b` and `cppboot.a.b` both become `CPPBOOT_A_B`. So a variable is matched against the keys that are already set: if it matches one, it sets that key; if it matches two, loading fails, rather than guessing.
- **A variable for a key that nothing else sets** is still found when that key is read. `Config::lookup` checks the stored values, then the variable for the key being read. This is what lets `CPPBOOT_MAIL_HOST` set `mail.host` with no YAML at all. Such a variable is also stored with each underscore as a dot, for code that lists keys, but that guess can be wrong, so `keysStartingWith` doesn't list keys that only a variable sets.

## Arguments

`--cppboot.some.key=value` always becomes a setting. `--other.key=value` becomes one only if `other.key` is already set by an earlier source. Any other argument stays for the application, so `--output=report.txt` doesn't turn into a setting just because it looks like one.

## The YAML reader

Settings files are read by CppBoot's own reader, in `framework/config/src/yaml.cpp`, which replaced yaml-cpp. It reads the part of YAML that settings use: nested maps, lists written either way, quoted and plain values, `|` and `>` blocks, and comments. Then `flatten` turns the tree into keys, with list items as `.0`, `.1` and so on.

### Why not a full YAML library

- **Settings use a small part of YAML.** Anchors, tags, complex keys and several documents per file appear in almost no settings file.
- **The rest of YAML causes mistakes.** A reader that accepts everything also accepts what the writer didn't mean: a plain value that wraps onto the next line and joins it, a key written twice where the second wins, or a tag that changes a value's type.
- **One less third-party dependency** in a part of the framework that every application uses, with no types of it in public headers anyway.

### Rejecting what it doesn't read

Everything outside the subset is an error with the line number, never read some other way: anchors and aliases, tags, `{a: 1}` maps, `?` keys, plain values over several lines, a key written twice in one map, tab indentation and more than one document. A setting that's silently read differently from what it says is worse than one that stops startup.

The reader also bounds its own work. Nesting is limited to 64 levels, so a hostile or broken file can't use up the stack, and the reader is fuzzed in CI with the JSON reader ([008](008-security-architecture.md)).

## Settings structs

A struct marked `CPPBOOT_CONFIG("prefix")` is filled from the settings under that prefix while the application starts, and registered as a component. Anything can then take it as `const MailSettings&`.

### How it's filled

The generator writes a function for each struct, which uses `config::SettingsBinder`:

```cpp
cppboot::config::SettingsBinder binder{config, "mail", "MailSettings"};
binder.bind("host", settings.host);
binder.require("host");
binder.check("host", cppboot::validation::notEmpty(settings.host), "must not be empty");
binder.bind("port", settings.port);
binder.check("port", cppboot::validation::range(settings.port, 1, 65535), "must be from 1 to 65535");
binder.ignoreGroup("retry");   // a nested struct reads its own keys, through its own binder
binder.finish();
```

- **`bind`** converts the text to the field's type, and leaves the field as it was if the setting isn't set. So the value written in the struct is the default, in plain C++, where a reader expects it.
- **Conversion is strict.** A number has to fit the field's type, `bool` accepts the same words as `getBool`, and a duration needs a unit and has to fit the field's unit exactly, so `1500ms` isn't silently cut to `1s`.
- **A list** is read from `key.0`, `key.1`, as YAML gives it, or from one value with commas, because that's the only way an environment variable can set one.
- **`finish`** throws `Exception` with `ErrorCode::InvalidConfiguration` if anything was reported. It reports everything at once, so fixing a file takes one run rather than one per mistake.

### Unknown keys are errors

`finish` also reports any key under the prefix that no field reads, with a suggestion when one is close: `mail.retry.atempts: isn't a setting that MailSettings reads. Did you mean mail.retry.attempts?`.

A misspelled key is the most common settings mistake, and the most dangerous, because it leaves the real setting at its default and nothing looks wrong. Rejecting it costs only the ability to keep unused keys under a struct's prefix, which nobody needs.

This covers settings files, defaults and arguments. It can't cover environment variables, since a variable's name doesn't say where the dots of its key go, so a misspelled variable is ignored.

### Why at startup, and why exit code 78

Filling happens inside `registerComponents`, before any component is created, so no component ever sees settings that failed a check. `runApplication` catches the exception and returns 78, the same as for any other settings problem. A missing setting found when a request first needs it, hours later, is worse in every way.

### Prefixes

Each struct has its own prefix, and two can't share one, which the generator checks across the whole program. One prefix can be inside another, such as `shop` and `shop.mail`: the outer struct's binder ignores the inner one's group. Prefixes can't start with `cppboot`, which is the framework's own.

## Sensitive values

`isSensitiveKey` decides which values stay out of logs and messages: a key that contains `password`, `secret`, `token`, `credential`, `private_key`, `api_key` or `database.url`.

- `Config::describe`, which logs every setting at `debug` level, shows `******` for them.
- `SettingsBinder` leaves the value out of a message about a sensitive key, so a mistyped password never reaches a log.

Deciding by name means that a secret stays hidden without anyone remembering to mark it, as long as its key says what it is. The guide asks developers to name keys that way.

## What was left out

- **Reloading settings while running.** See [the shape](#the-shape-flat-keys-read-once) above.
- **Other file formats,** such as `.properties` or TOML. One format means one reader to secure and one set of rules to learn.
- **Settings from a remote server,** such as a configuration service. The environment covers what orchestrators provide, and an application that needs a service can read it in a module.
- **Variable substitution** such as `${HOME}` inside YAML. It makes a file's meaning depend on the machine in a way that's hard to see, and the environment already overrides any key directly.
