# Third-party software

CppBoot is licensed under the [Apache License 2.0](LICENSE). It uses these projects, which keep their own licenses. Where CppBoot copies one of them, its license has to go with the copy.

| Project | License | How CppBoot uses it | Copied into |
|---|---|---|---|
| [Boost](https://www.boost.org) (Asio and Beast) | Boost Software License 1.0 | The web server | Applications, as compiled code |
| [OpenSSL](https://www.openssl.org) | Apache License 2.0 | HTTPS, tokens and password hashing | Packages, as shared libraries |
| [libpq](https://www.postgresql.org) | PostgreSQL License | PostgreSQL access | Packages, as a shared library |
| [SQLite](https://www.sqlite.org) | Public domain | SQLite access | Packages, as a shared library |
| [libclang](https://llvm.org) | Apache License 2.0 with LLVM Exceptions | The code generator, at build time only | The SDK, with Clang's built-in headers in `lib/cppboot/clang` |
| [GoogleTest](https://github.com/google/googletest) | BSD 3-Clause | CppBoot's own tests | Nothing |

Applications also link to the C++ standard library and runtime of the compiler that builds them, such as GCC's `libstdc++`, which has the GCC Runtime Library Exception. A package copies those as shared libraries.

## If you ship a package

`cppboot package` copies the shared libraries your application needs into the package, so the package includes software from the projects above. If you give a package to someone else, include their licenses with it. On Linux, these are usually in `/usr/share/doc/<package>/copyright`, and in MSYS2 they're in `share/licenses`.

This list covers what CppBoot itself uses. Your application's own dependencies have their own licenses.
