# Getting started

In this tutorial, you install CppBoot on Windows, create an application and add your own component to it. It takes about 15 minutes, and most of that is installing the compiler.

This tutorial builds a background service. The [next ones](../README.md#using-cppboot) add a web API, a database and sign-in.

## Install CppBoot

In PowerShell, run:

```powershell
irm https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.ps1 | iex
```

This installs MSYS2 if you don't have it, which provides the compiler and libraries, then the CppBoot SDK. It ends by running `cppboot doctor`, which should say `Everything CppBoot needs is ready.` Then open a new PowerShell window. The [installation guide](../guides/installation.md) explains what it changes, and how to install on Linux.

## Create an application

Go to the folder where you keep your projects, then run:

```powershell
cppboot create hello-world
cd hello-world
cppboot run
```

`cppboot run` builds the application and starts it. You should see:

```text
[hello-world] [info] Starting CppBoot 0.1.0 (profile: none)
[hello-world] [info] Hello from CppBoot!
[hello-world] [info] Application is running
```

Press Ctrl+C. The application logs `Shutting down` and exits with code 0.

## See how it works

`src/main.cpp` has no knowledge of your classes:

```cpp
#include <cppboot/boot.hpp>

#include "cppboot_components.generated.hpp"

int main(int argc, char* argv[]) {
    return cppboot::runApplication(argc, argv, cppboot::generated::registerComponents);
}
```

Your code goes in `src/greeting.hpp`, which has two classes marked with `CPPBOOT_SERVICE`:

- `GreetingService` is a component that other classes can use.
- `GreetingModule` takes a `GreetingService` in its constructor. It derives from `cppboot::Module`, so it also starts and stops with the application.

During the build, a code generator reads every `.hpp` file in `src`, finds the marked classes and generates the code that creates them. You never write that code.

## Add a component

Create `src/clock.hpp`:

```cpp
#pragma once

#include <cppboot/cppboot.hpp>
#include <string>

#include "greeting.hpp"

class CPPBOOT_SERVICE TimeOfDay {
public:
    std::string period() const { return "morning"; }
};

class CPPBOOT_SERVICE MorningModule : public cppboot::Module {
public:
    MorningModule(GreetingService& greetings, TimeOfDay& time)
        : greetings_(greetings), time_(time) {}

    std::string_view name() const noexcept override { return "morning"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("{} Good {}.", greetings_.greeting(), time_.period());
        return {};
    }

private:
    GreetingService& greetings_;
    TimeOfDay& time_;
};
```

Run `cppboot run` again, and the log includes `Hello from CppBoot! Good morning.`. Press Ctrl+C to stop it. You didn't have to register anything or change `main.cpp`.

## See a mistake caught early

Delete `class CPPBOOT_SERVICE TimeOfDay` and its body, and replace them with a plain class:

```cpp
class TimeOfDay {
public:
    std::string period() const { return "morning"; }
};
```

Now run `cppboot build`. The build fails and tells you what's wrong, without the application ever running:

```text
cppboot-codegen: MorningModule (.../clock.hpp:15) needs TimeOfDay, but no component provides it
```

Put `CPPBOOT_SERVICE` back to fix it.

## Change a setting

Modules can read settings through the context. In `src/clock.hpp`, replace the line that logs the message with these two lines:

```cpp
const std::string who = context.config().getString("greeting.name", "world");
context.logger().info("{} Good {}, {}.", greetings_.greeting(), time_.period(), who);
```

Then add a section to `cppboot.yaml`:

```yaml
greeting:
  name: CppBoot
```

Run `cppboot run`, and the log now ends with `Good morning, CppBoot.`. You can override a setting for a single run:

```powershell
cppboot run --greeting.name=Alex
```

## Restart on every change

Instead of running `cppboot run` after each change, run this once:

```powershell
cppboot dev
```

It builds and starts the application, and each time you save a file in `src`, it stops the application, builds it and starts it again. Press Ctrl+C to stop it.

## Test it

The project came with tests in `tests/`. Run them:

```powershell
cppboot test
```

`tests/greeting_test.cpp` creates a `GreetingService` and checks what it returns, because a component is a plain class. `tests/components_test.cpp` checks that every component can get what its constructor asks for, so it would have caught the mistake from earlier too.

## Add more with the CLI

`cppboot add` writes a marked class for you. This adds `src/billing_service.hpp`, which the next build finds:

```powershell
cppboot add service Billing
```

See the [command-line guide](../guides/cli.md#cppboot-add-kind-name) for every kind.

## Next steps

- [Build a REST API](rest-api.md).
- Learn about lifetimes and scopes in the [dependency injection guide](../guides/dependency-injection.md).
- Add background work with the [modules guide](../guides/modules.md).
- See every way to set a value in the [configuration guide](../guides/configuration.md).
