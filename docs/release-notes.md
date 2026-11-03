The first pre-release of CppBoot, a native application framework for C++20 inspired by Spring Boot. **It isn't stable yet:** the API can change, and it hasn't had an independent security review.

## Install

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.ps1 | iex
```

Ubuntu 24.04:

```bash
curl -fsSL https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash
```

Then create a project:

```text
cppboot create hello-world --web
cd hello-world
cppboot run
```

The [installation guide](https://github.com/mahirmovsumov/CppBoot/blob/main/docs/guides/installation.md) covers other systems, and what the installers change.

## What's in it

Web APIs with HTTPS and rate limits, PostgreSQL and SQLite, token sign-in, health checks and metrics, packaging as a Linux service, and a code generator that wires everything at build time. The [changelog](https://github.com/mahirmovsumov/CppBoot/blob/main/CHANGELOG.md) lists everything.

## Files

| File | For |
|---|---|
| `cppboot-sdk-*-windows-x64.zip` | Windows, with MSYS2's UCRT64 tools |
| `cppboot-sdk-*-linux-x64.tar.gz` | Ubuntu 24.04 on x86-64 |
| `install.ps1`, `install.sh` | The installers, which check these files against `SHA256SUMS` |
