# Installation

The installers set up everything CppBoot needs, check it with `cppboot doctor`, and say what to fix if anything is wrong.

## Windows

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.ps1 | iex
```

This:

1. **Installs MSYS2,** if it isn't already in `C:\msys64` or `D:\msys64`. MSYS2 provides GCC, CMake, Ninja, libclang and the libraries CppBoot uses. Its installer comes over HTTPS from MSYS2's own releases on GitHub, which publish no checksum to check it against.
2. **Installs and updates those packages** with `pacman`. This updates the rest of MSYS2 too.
3. **Downloads the SDK** from the latest release, checks it against the release's checksums, and unpacks it into `%LOCALAPPDATA%\CppBoot\sdk`.
4. **Changes your account's environment:** it adds the SDK to `PATH`, and sets `CPPBOOT_TOOLCHAIN` to MSYS2's `ucrt64\bin` folder. It doesn't add MSYS2 to `PATH`, where its programs could take the place of others, such as Git's. `cppboot` adds that folder for the programs it starts.

Open a new terminal afterwards, so that it gets the new settings.

To choose options, download the script and run it:

```powershell
.\install.ps1 -Version 0.1.0 -Msys2Root D:\msys64
```

| Option | Does |
|---|---|
| `-Version` | Installs a release such as `0.1.0`, rather than the latest. Pre-releases count as the latest |
| `-InstallDir` | Installs somewhere other than `%LOCALAPPDATA%\CppBoot` |
| `-Msys2Root` | Uses, or installs, MSYS2 in that folder |
| `-Archive` | Installs an SDK archive you already downloaded |
| `-FromSource` | Builds the SDK on your machine, which takes several minutes, so that it always matches your compiler |
| `-SkipPackages` | Doesn't install or update MSYS2's packages |
| `-NoEnvironment` | Doesn't change `PATH` or `CPPBOOT_TOOLCHAIN` for your account |

## Linux

On Ubuntu 24.04:

```bash
curl -fsSL https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash
```

This installs the compiler, CMake, Ninja, libclang and libraries with `apt`, which uses `sudo`. It then downloads the SDK, checks it, unpacks it into `~/.cppboot`, and adds `~/.cppboot/bin` to `PATH` in `~/.bashrc`, `~/.profile` and `~/.zshrc`.

Options go after `bash -s --`:

```bash
curl -fsSL https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash -s -- --from-source
```

| Option | Does |
|---|---|
| `--version` | Installs a release such as `0.1.0` |
| `--prefix` | Installs somewhere other than `~/.cppboot` |
| `--archive` | Installs an SDK archive you already downloaded |
| `--from-source` | Builds the SDK on your machine |
| `--skip-packages` | Doesn't install packages with `apt` |
| `--no-path` | Doesn't change your shell's startup files |

**Downloaded SDKs are built on Ubuntu 24.04,** because the SDK's libraries have to match the system's. On any other Linux, use `--from-source`. Without `apt`, install these yourself first, then add `--skip-packages`: a C++20 compiler, CMake 3.25 or newer, Ninja, libclang, yaml-cpp, spdlog, Boost, nlohmann/json, libpq, SQLite, OpenSSL and GoogleTest.

## Check an installation

```text
cppboot doctor
cppboot doctor --full
```

`cppboot doctor` checks the installation, CMake, Ninja and the code generator. Then it creates a small project that uses CppBoot, which shows that the compiler and every library work together. `--full` also builds and runs a sample web project with a database, which takes about a minute.

Each problem comes with what to do about it. It exits with 0 if there are none, so a script can use it.

It also warns if your compiler is a different version from the one the SDK was built with. Libraries built by one version don't always link with another, which can happen after MSYS2 updates its compiler. The fix is to install the SDK again, or with `-FromSource`.

### Other copies of libraries on Windows

Some installers put their own copies of libraries on Windows, such as PostgreSQL's in `C:\Program Files\PostgreSQL`. Those are built for Microsoft's compiler, and don't link with MSYS2's. So a CppBoot project looks for PostgreSQL, SQLite and OpenSSL in MSYS2 first. To use another copy, set `PostgreSQL_ROOT`, `SQLite3_ROOT` or `OPENSSL_ROOT_DIR`.

## Upgrade and remove

Run the installer again to upgrade. It replaces the SDK only once the new one is complete.

To remove CppBoot, delete its folder, then remove it from `PATH`:

- **Windows:** delete `%LOCALAPPDATA%\CppBoot`, and remove its `bin` folder from your account's `PATH` and `CPPBOOT_TOOLCHAIN` in **Edit environment variables for your account**. MSYS2 stays, as you may use it for other things.
- **Linux:** delete `~/.cppboot`, and the `# CppBoot` lines from your shell's startup files.

## Build from a clone

To work on CppBoot itself, see the [README](../../README.md#build-on-windows).
