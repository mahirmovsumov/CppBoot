# Getting started

In this tutorial, you install CppBoot on Windows, create an application and add your own component to it. It takes about 15 minutes, and most of that is installing the compiler.

This tutorial builds a background service. The [next ones](../README.md#using-cppboot) add a web API, a database and sign-in.

## Install the tools

1. Install MSYS2 from <https://www.msys2.org> into `D:\msys64`.
2. From the root of the CppBoot repository, run these commands in PowerShell:

   ```powershell
   .\scripts\install-deps.ps1
   . .\scripts\dev-env.ps1
   ```

   The first script installs GCC, Clang, CMake, Ninja and the libraries CppBoot needs. The second adds them to PATH for the current PowerShell window only, so run it again in each new window.

## Build and install CppBoot

```powershell
cmake --preset release
cmake --build --preset release
cmake --install build\release --prefix D:\cppboot-sdk
$env:PATH = "D:\cppboot-sdk\bin;$env:PATH"
```

To check the installation, run `cppboot info`. It prints the version and the install folder.

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

## Next steps

- [Build a REST API](rest-api.md).
- Learn about lifetimes and scopes in the [dependency injection guide](../guides/dependency-injection.md).
- Add background work with the [modules guide](../guides/modules.md).
- See every way to set a value in the [configuration guide](../guides/configuration.md).
