# 004: Dependency injection

**Status:** Accepted.

## Decisions

### The container works without the generator

Generated code only calls the public API, such as `registerSingleton` and `bind`. Components can always be registered by hand, and the runtime has no dependency on libclang.

### Dependencies are declared, not discovered

C++20 can't list a constructor's parameters. The container could guess them with template tricks, but those break on overloads and templates and produce poor errors. Instead, `Deps<A, B>` declares them, and a `static_assert` checks them against the constructor. The generator writes these declarations automatically.

### `cppboot-di` depends only on `cppboot-base`

This keeps it usable without the application runtime, and it keeps the core free of it. An application that doesn't use injection doesn't pay for it.

### `cppboot-boot` connects the two

`runApplication()` lives in a small library above both the core and the container. It registers the components, validates them, and adds every component in the `cppboot::Module` group to the application. The container is declared first, so components outlive the modules using them.

### `shared_ptr` for all components

Components have shared ownership because a singleton can be used by many others. The cost is an atomic reference count when a pointer is copied, which is small next to the work components do. Hot paths should hold a pointer rather than resolve repeatedly.

### Resolution state lives in a `Resolver`

Each top-level `resolve()` creates a `Resolver` that carries the chain of types being created, which is what detects cycles. This avoids thread-local storage and meets the rule against global mutable state.

## Thread safety

- The container has one recursive mutex, held while a singleton is created, so that two threads can't create it twice.
- A scope has its own recursive mutex, held while it creates a scoped component.
- Singletons are always created with no active scope, so the container's lock is never held while waiting for a scope's lock. The locks can't deadlock against each other.
- Transients are created with no lock held by the container.

## Captive dependencies

A singleton that holds a scoped component would keep using it after its scope ended. There are two defenses:

- `validate()` rejects this for declared dependencies, including through transients.
- At runtime, a singleton's factory runs with no active scope, so resolving a scoped component from it throws.

A singleton that holds a transient is allowed, and that transient simply lives as long as the singleton.

## Destruction

Singletons and scoped components are destroyed in the reverse of their creation order. Dependencies are created first, so components are destroyed before what they depend on.

## Not included yet

- Choosing between several implementations of one interface by name
- Creating singletons at startup, other than modules
- Injecting settings and the logger into component constructors
- Marking classes as transient or scoped for the generator
