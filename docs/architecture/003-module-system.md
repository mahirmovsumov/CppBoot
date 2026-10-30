# 003: Module system

**Status:** Accepted

## Contract

A module implements `cppboot::Module`:

- `name()` returns a unique name.
- `dependencies()` lists the names of the modules that must start first.
- `start(ApplicationContext&)` returns `Result<void>` and may throw.
- `stop()` is `noexcept` and runs only if `start()` succeeded.

## Ordering

`ModuleManager::resolveOrder()` produces a dependency-first order. Modules with no ordering constraint between them keep their registration order, so startup is the same on every run.

Startup fails before any module starts if there is a null module, a duplicate name, a missing dependency or a cycle.

## Failure

Startup stops at the first module that fails. Modules that already started are stopped in reverse order, and the failed module isn't stopped.

## Registration

In Phase 1, applications register modules explicitly with `Application::addModule()` or `emplaceModule()`. Generated registration code will call the same functions later, so the runtime doesn't depend on the generator.
