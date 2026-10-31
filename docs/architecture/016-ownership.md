# 016: Ownership and lifetimes

**Status:** Accepted.

## The goal, and its limit

Developers should write ordinary C++ with no ownership types in sight, and the framework should handle ownership where it can. The idea came from Rust, but the result isn't Rust-like, and the documentation doesn't call it that.

Rust checks every borrow because the language carries lifetimes in its types. C++ doesn't, so no library can check that a reference copied inside a function stays valid. A framework can only:

1. Make the code it controls safe, which is the creation and destruction of components.
2. Reject what it can see in declarations.
3. Provide tools that find other bugs while a program runs.

Claiming more would make developers trust guarantees that aren't there.

## References in constructors

A component takes `T&`, `const T&` or `std::shared_ptr<T>`. The generator records a dependency on `T` for all of them, so the rest of the system didn't change.

`Deps<...>` keeps its meaning, and generated and manual registration use the same path. `detail::Injected<T>` is passed to the constructor and converts to whichever form it takes. It relies on the constructor's parameters rather than on the generator, so manual registration gets the same behavior.

### Lifetimes are the container's job

The reverse creation order of singletons would make references to singletons safe, but only those. A transient would die when its `shared_ptr` went out of scope, right after the constructor, leaving a dangling reference with no warning.

So every instance's deleter holds the `shared_ptr` of each of its dependencies, and they're released only after the instance is destroyed. This gives one rule with no exceptions for components created from `Deps<...>`, including a component still held after its container is gone: **a dependency outlives whatever depends on it.** A factory creates its own object, so this is up to the factory.

Dependencies are created inside braces, because the order of function arguments isn't specified, which would make creation and destruction order differ between compilers.

The cost is one small extra allocation for each instance, rather than one combined with `make_shared`. Singletons only do this once.

## Why not the rest of the proposal

Some ideas were considered and left out:

- **`container.bind<T>()` for registration.** Marked classes already need no registration.
- **Per-request controllers.** This adds work to every request, and controllers rarely hold request data. A pointer to a request stored in a controller is a build error instead.
- **Choosing `unique_ptr` or `shared_ptr` automatically.** Developers don't see this now, and singletons are resolved once, so there's no runtime cost to save.
- **`extend_lifetime`.** There's nothing to extend, because a dependency already lives long enough. A class that gives away ownership takes a `std::shared_ptr`.

## Field checks

The generator looks at each component's fields, without reading function bodies, because a component lives as long as the application and every thread shares it.

Pointing at a request is an error, because it's almost never right. Other patterns are warnings, because they have real false alarms. A warning that's right most of the time still has to be silenced sometimes, so `CPPBOOT_UNCHECKED` needs a reason, which leaves the claim where a reviewer can see it.

The check for shared state has known gaps. A lock in the class silences it, even if the lock is never used, and other classes are assumed to protect themselves. It's a prompt for developers, and not proof of anything.

## Runtime checkers

`cppboot build --sanitize=` uses a separate build folder, so that switching doesn't rebuild everything. Installed SDK libraries aren't instrumented. AddressSanitizer works with that, though it misses errors inside those libraries. ThreadSanitizer may miss races there, and could report false ones. Shipping sanitized copies of the SDK would fix this, and can be added if false reports turn up. Both have an end-to-end test, on Linux, that they find a real bug through the CLI.

## Hardened standard library

`_GLIBCXX_ASSERTIONS` is on in every build of a new project, including release builds, where a stopped program is safer than corrupted memory. It doesn't change the ABI, so it's safe with the SDK's libraries, which are built without it.

## Lifetime hints

`CPPBOOT_LIFETIMEBOUND` wraps `[[clang::lifetimebound]]`. It's on `Result::value()` as well as request and response accessors, because a chain such as `create().value().path()` is only caught if every step has it. It only helps under Clang.
