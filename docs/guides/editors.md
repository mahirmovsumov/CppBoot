# Editors

Every project that `cppboot create` makes has settings for Visual Studio Code in `.vscode/`, so code completion, building, testing and debugging work when you open the folder. Other editors can use the same compile commands.

## Visual Studio Code

1. [Install CppBoot](installation.md), then open a new terminal so it finds `cppboot`.
2. Create a project and open its folder:

   ```powershell
   cppboot create my-api --web
   code my-api
   ```

3. Install the extensions that VS Code suggests: C/C++ from Microsoft, for debugging, and clangd, for code completion. If you'd rather use only one for code completion, see [Code completion](#code-completion).
4. Press Ctrl+Shift+B to build. This also writes `build/compile_commands.json`, which code completion needs, so do it once before you start editing.

### What's in `.vscode/`

| File | What it does |
|---|---|
| `tasks.json` | The tasks below, which run the `cppboot` commands |
| `launch.json` | Debugs the application with gdb, after building it |
| `settings.json` | Points code completion at `build/compile_commands.json`, and hides the build folders |
| `extensions.json` | Suggests the extensions |

These are ordinary files in your project. Change them as you like, and commit them so everyone on the project gets them.

### Tasks

Run a task with **Terminal > Run Task**, or with these keys:

| Task | Keys | Runs |
|---|---|---|
| `cppboot: build` | Ctrl+Shift+B | `cppboot build` |
| `cppboot: test` | Set your own, in **Keyboard Shortcuts** under **Run Test Task** | `cppboot test` |
| `cppboot: dev` | | `cppboot dev`, which restarts the application whenever you save |
| `cppboot: run` | | `cppboot run` |

Compiler errors appear in the **Problems** panel, and clicking one opens the line. That includes errors from the code generator, such as a component that needs a class that isn't marked.

### Debugging

Press F5. It builds the project, then starts it under gdb, in the project folder so that it finds `cppboot.yaml`. Set breakpoints by clicking left of a line number.

To pass settings, add them to `args` in `launch.json`:

```json
"args": ["--cppboot.logging.level=debug", "--cppboot.server.port=9090"]
```

On Windows, gdb and the libraries the application needs are in MSYS2's folder. `launch.json` finds them through `CPPBOOT_TOOLCHAIN`, which the installer sets. If you installed CppBoot another way, set `CPPBOOT_TOOLCHAIN` to MSYS2's `ucrt64\bin` folder, such as `C:\msys64\ucrt64\bin`, and restart VS Code.

Run `cppboot doctor` if debugging doesn't start. It warns if gdb is missing.

### Code completion

Both extensions read `build/compile_commands.json`, so they know each file's include folders and settings, including the generated `cppboot_components.generated.hpp`.

Having both turned on gives two sets of suggestions. To use only clangd, which is faster on large projects, add this to `.vscode/settings.json`:

```json
"C_Cpp.intelliSenseEngine": "disabled"
```

To use only Microsoft's, disable the clangd extension for the workspace.

If suggestions are missing or wrong, build once, and after adding a header, build again so the generated file includes it.

## Other editors

Any editor that uses clangd works the same way: CLion, Neovim, Emacs, Sublime Text and others. Point it at `build/compile_commands.json`, or start clangd with `--compile-commands-dir=build`. CLion can also open `CMakeLists.txt` as a CMake project; set its CMake option `-DCMAKE_PREFIX_PATH` to the folder CppBoot is installed in, which `cppboot info` shows.
