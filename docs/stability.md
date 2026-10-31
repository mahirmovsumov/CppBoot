# What 1.0 promises

This policy starts with version 1.0. **Until then, any release may change anything.**

CppBoot follows [semantic versioning](https://semver.org). A patch release fixes bugs. A minor release adds features. Only a major release removes or changes what's listed as stable here.

## Stable

| Area | What stays the same |
|---|---|
| C++ API | Everything in the headers under `cppboot/`, apart from the exceptions below |
| Marks | `CPPBOOT_SERVICE` and the other macros in `cppboot/base/annotations.hpp`, and what they mean |
| CLI | `cppboot` commands and options, and their exit codes |
| Settings | Keys starting with `cppboot.`, and their environment variables, meanings and defaults |
| Exit codes | 0, 1 and 78 from an application |
| HTTP | `/actuator` routes, the JSON form of error responses, and which status means what |
| Metrics | Names and labels of the metrics CppBoot records |
| Data | The table that records migrations, so that an upgrade can read what an older version wrote |
| Packages | The layout of a package, and the paths a Linux service installs to |

Code that compiled with one 1.x release will compile with a later one, apart from rare conflicts such as a new function with the same name as one of yours.

## Not stable

- **Binary compatibility:** rebuild your application with each new SDK. There's no stable ABI, and applications link to the framework statically.
- **Anything named `detail`,** and anything in a source folder rather than `include`.
- **Generated code:** only `cppboot::generated::registerComponents` is stable. What's inside it isn't, and neither is the generated file's layout.
- **The text of messages:** logs, errors and warnings can change, so don't read them from code. Use `ErrorCode`, `DatabaseError::Kind` and exit codes instead.
- **New generator warnings:** a new release can warn about code that was quiet before. A warning never stops a build, but a new build error can be added when a mark would do nothing, or when the code can't be safe.
- **Performance:** the [benchmarks](benchmarks.md) describe what was measured, and don't promise it.
- **Third-party libraries:** their versions can change. `cppboot::web::Json` stays `nlohmann::ordered_json`, but its version can change in a minor release.

## Changing something stable

1. A minor release marks it `[[deprecated]]`, says what to use instead, and adds it to the [changelog](../CHANGELOG.md).
2. It keeps working until the next major release, which is the only place it can be removed.

A security fix is the exception. If keeping a behavior would leave users exposed, a patch release can change it, and the changelog says so clearly.

## Platforms

| Item | Supported |
|---|---|
| Language | C++20. A new required language version needs a major release |
| Compilers | The GCC and Clang versions that CI uses at the time of release |
| Systems | Linux and Windows on x86-64 |
| Services | Linux with systemd |

MSVC isn't supported. Other compilers and systems may work, but aren't tested.

## Which releases get fixes

CppBoot has one developer, so only the latest minor release gets fixes, including security fixes. See the [security policy](../SECURITY.md).
