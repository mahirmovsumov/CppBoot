# Ownership and memory safety

CppBoot handles ownership for the components it creates, and adds tools that find many memory bugs. **C++ can't guarantee memory safety the way Rust does, and neither can CppBoot.** Code that compiles can still have memory bugs and data races, so use these tools rather than trusting that they've caught everything.

## Plain references

Take the components a class needs as references:

```cpp
class CPPBOOT_SERVICE OrderService {
public:
    OrderService(InventoryService& inventory, const PaymentService& payment)
        : inventory_(inventory), payment_(payment) {}

private:
    InventoryService& inventory_;
    const PaymentService& payment_;
};
```

There's no ownership to think about. **The container keeps each dependency alive until the component using it has been destroyed,** whatever the lifetimes of the two.

Take a `std::shared_ptr` instead if your class needs to give someone else shared ownership, such as a background task that may outlive it. Both forms can be mixed in one constructor.

Copies and raw pointers fail the build. A copy wouldn't be the shared component, and a pointer suggests it might be missing.

## What the build checks

### Errors

| Mistake | Why |
|---|---|
| A component with a field that's a reference or pointer to `Request`, `Response` or `Principal` | A component lives as long as the application, and these end with their request |
| A dependency with no provider, or a dependency cycle | The application couldn't start |

A singleton that depends on a scoped component is rejected by `validate()`, which `runApplication` calls at startup. Marked classes are always singletons, so this only applies to manual registration.

### Warnings

These don't stop the build, because some are false alarms:

| Pattern | Why |
|---|---|
| A field of a component that can change, in a class with no lock | Every request and thread shares a component, so this is a data race |
| A `std::string_view` or `std::span` field in a component | It doesn't own its data, which may be gone |
| A field that's a copy of one of the component's dependencies | It isn't the shared component |
| A route function that returns a `std::string_view` or `std::span` | It dangles if it points at a local variable |

The check for changing fields covers numbers, `std::string` and standard containers, in a class with a public non-const function or a `mutable` field. Any `std::mutex` in the class silences it, whether or not it's used correctly. `start` and `stop` on a module don't count, because they run before and after requests.

If a field is safe, say why, and the check accepts it:

```cpp
CPPBOOT_UNCHECKED("Only ever points at string literals") std::string_view label_;
```

### What the build can't see

The generator reads declarations, not function bodies. **Finding nothing doesn't mean your code is safe.** These still compile with no warning:

```cpp
// The reference outlives the request, so the thread reads freed memory.
std::thread([&request] { process(request); }).detach();

// The view points at a temporary that's already gone.
std::string_view name = getUser().name;

// A race in a class with a lock that this function forgets to take.
void addOrder(const Order& order) { orders_.push_back(order); }
```

Never let a reference to a request, or anything from one, reach another thread. Copy what you need first.

## Find bugs while running

These checkers run your program and report bugs as they happen. They're slow, so use them in development and tests, not production.

```text
cppboot run --sanitize=address
cppboot run --sanitize=thread
```

| Option | Finds | Slows a program by about |
|---|---|---|
| `--sanitize=address` | Use after free, out-of-bounds access, leaks, and undefined behavior such as integer overflow | 2 times |
| `--sanitize=thread` | Data races | 5 to 15 times |

- `--sanitize` also works with `cppboot build`, and has to come first. Each checker gets its own folder, such as `build-address`.
- **They only need Linux.** GCC on Windows doesn't include them, so `--sanitize` fails with an error there. Use WSL or CI.
- **They only find bugs in code that runs.** Run your tests and send real requests.
- The SDK isn't built with the checkers, so they see your code more clearly than the framework's. `--sanitize=thread` can miss races inside the framework, and could report false ones. CppBoot's own CI runs its code under both.

## Defaults in new projects

Projects from `cppboot create` turn these on in every build:

- **`_GLIBCXX_ASSERTIONS`:** stops the program on mistakes such as an index past the end of a vector, rather than letting it read or write the wrong memory. This also applies in production, where a crash is safer than corrupting memory. It only works with GCC's standard library, which Clang also uses by default on Linux.
- **`-Wall -Wextra`:** compiler warnings for common mistakes.

An existing project can add the same lines to its `CMakeLists.txt`.

## Lifetime hints in Clang

CppBoot marks functions such as `Request::path()` and `Result::value()` with `CPPBOOT_LIFETIMEBOUND`. Building with Clang then warns about this mistake. GCC ignores the mark:

```cpp
const std::string& path = Request::create(Method::Get, "/a").value().path();  // Dangles
```

Use the same mark on your own functions that return a reference into an object:

```cpp
const std::string& name() const CPPBOOT_LIFETIMEBOUND { return name_; }
```
