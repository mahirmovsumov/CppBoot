# C++20 coding standard

## Naming

| Item | Style | Example |
|---|---|---|
| Classes | PascalCase | `ModuleManager` |
| Functions | camelCase | `resolveOrder` |
| Variables | snake_case | `exit_code` |
| Private members | snake_case with a trailing underscore | `started_` |
| Constants | `k` + PascalCase | `kExitSuccess` |
| Headers and sources | `.hpp` and `.cpp` | `worker_group.hpp` |

Don't name a variable `module`, because it is a contextual keyword in C++20.

## Namespaces

Modules use `cppboot::<module>`, such as `cppboot::config`. The types developers use most (`Application`, `Module`, `Result` and `Error`) live directly in `cppboot`.

## Formatting

Use `clang-format` with the repository's `.clang-format`: 4-space indentation and a 100-column limit.

## Rules

1. No raw owning pointers.
2. No global mutable framework state. See [002](../architecture/002-core-runtime.md) for the one exception.
3. No circular dependencies.
4. No third-party types in public headers, except `cppboot::web::Json`. See [001](../architecture/001-system-architecture.md).
5. No operating system code outside `framework/platform`.
6. No C++23 or later features.
7. Every public API has a test.
8. Every public interface has documentation.
9. No `std::shared_mutex`, which fails under contention on MinGW. Use `std::mutex`. See [013](../architecture/013-observability.md).

## Errors

Use `Result<T>` for expected failures. Throw `Exception` only for bugs and for failures that make continuing impossible. See [001](../architecture/001-system-architecture.md).
