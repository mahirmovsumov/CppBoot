# Getting started

In this tutorial, you install CppBoot on Windows, create an application and add a module to it. It takes about 15 minutes, and most of that is installing the compiler.

CppBoot has no web or database support yet. Today, you can use it to build background services and command-line applications.

## Install the tools

1. Install MSYS2 from <https://www.msys2.org> into `D:\msys64`.
2. From the root of the CppBoot repository, run these commands in PowerShell:

   ```powershell
   .\scripts\install-deps.ps1
   . .\scripts\dev-env.ps1
   ```

   The first script installs GCC, CMake, Ninja and the libraries CppBoot needs. The second adds them to PATH for the current PowerShell window only, so run it again in each new window.

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
[hello-world] [info] Application is running
```

Press Ctrl+C. The application logs `Shutting down` and exits with code 0.

The generated `src/main.cpp` is all the code an application needs:

```cpp
#include <cppboot/cppboot.hpp>

int main(int argc, char* argv[]) {
    cppboot::Application app(argc, argv);
    return app.run();
}
```

## Add a module

An application does its work in modules. Replace `src/main.cpp` with this:

```cpp
#include <cppboot/cppboot.hpp>

class GreetingModule : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "greeting"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        const std::string who = context.config().getString("cppboot.greeting.name", "world");
        context.logger().info("Hello, {}!", who);
        return {};
    }
};

int main(int argc, char* argv[]) {
    cppboot::Application app(argc, argv);
    app.emplaceModule<GreetingModule>();
    return app.run();
}
```

Run it with `cppboot run`, and the log includes `Hello, world!`. Press Ctrl+C to stop it.

## Change a setting

Open `cppboot.yaml` and add a `greeting` section under `cppboot`:

```yaml
cppboot:
  application:
    name: hello-world

  logging:
    level: info

  greeting:
    name: CppBoot
```

Run `cppboot run` again, and the log now says `Hello, CppBoot!`. You don't need to rebuild after changing a setting.

You can also override a setting for a single run:

```powershell
cppboot run --cppboot.greeting.name=Alex
```

## Next steps

- Add background work with the [modules guide](../guides/modules.md).
- See every way to set a value in the [configuration guide](../guides/configuration.md).
