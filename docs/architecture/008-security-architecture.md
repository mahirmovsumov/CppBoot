# 008: Security architecture

**Status:** Accepted.

This record covers CppBoot's security model as a whole: what it trusts, what it defends against, and where each defense is described. Sign-in and protected routes are in [012: Security](012-security.md), rate limits in [014](014-rate-limiting.md) and HTTPS in [015](015-https.md).

CppBoot hasn't had an independent security review, and [SECURITY.md](../../SECURITY.md) says so.

## Principles

These apply to every part of CppBoot, and each record below follows them.

- **Deny by default.** Once a project uses security, a route needs a token unless it's marked public. A server listens on `127.0.0.1` unless told otherwise.
- **Fail closed.** A security setting that's wrong stops startup. Nothing falls back to running less protected, such as without HTTPS or without a secret.
- **No settings that weaken it.** There's no switch to turn off token checks, choose ciphers or lower the TLS version below 1.2.
- **A mark that does nothing is an error.** A security mark that wouldn't protect anything fails the build, so code never looks safer than it is.
- **Secrets stay out of files and logs.** They come from the environment, and are hidden wherever settings are printed.
- **Checks in generated code, not conventions.** What the generator can see, it enforces at build time.

## Trust boundaries

| Source | Trusted? | Treated as |
|---|---|---|
| Network clients | No | Every byte is untrusted, including headers such as `X-Forwarded-For` |
| Proxies in `trusted_proxies` | Only for the client address | Their `X-Forwarded-For` is believed, and nothing else they send |
| Settings files and environment | Yes | Written by whoever deploys the application |
| The database | Partly | Its responses are trusted, but stored password hashes have their parameters checked |
| Application code | Yes | CppBoot can't protect against the application itself |

## Defenses by layer

| Layer | Defense | Described in |
|---|---|---|
| Transport | HTTPS with TLS 1.2 or newer, forward-secret ciphers only, and `close_notify` so a cut-short response is visible | [015](015-https.md) |
| HTTP parsing | Limits on body size and headers, rejecting malformed requests, encoded null characters and line breaks in response headers | [010](010-web-server.md#limits-and-failures) |
| Resources | Connection limits, queue timeouts, bounded memory for every table that grows with input | [005](005-memory-management.md#bounded-memory), [010](010-web-server.md#overload) |
| Abuse | Per-client rate limits that work behind proxies only when the proxy is trusted | [014](014-rate-limiting.md) |
| Authentication | HS256 tokens, checked in generated code for every route not marked public | [012](012-security.md) |
| Authorization | Roles checked in the same generated code | [012](012-security.md) |
| Passwords | scrypt with per-password salts, compared in constant time | [012](012-security.md#passwords) |
| Data | Query parameters as `$1`, `$2`, never inside SQL text | [011](011-data-access.md) |
| Errors | Clients get a generic message for unexpected exceptions, and details go to the log | [010](010-web-server.md#limits-and-failures) |

## Secrets

- New projects have no secret in `cppboot.yaml`. The signing secret and sample password come from environment variables, and the application won't start without them.
- `Config::describe()` hides the values of keys containing `password`, `secret`, `token`, `credential`, `private_key`, `api_key` or `database.url`.
- When the database can't be reached, the error doesn't show the password from its URL. An end-to-end test checks this.
- Health check failures go to the log, not to the public health route, because their messages can hold addresses or credentials ([013](013-observability.md)).
- `/actuator/metrics` and `/actuator/info` need `cppboot.actuator.metrics_token` once it's set, compared in constant time. Without one, they're closed in a project that uses security, and open in one that doesn't.

## Running in production

`cppboot package` on Linux makes a systemd service that runs with as little as it can:

- Its own unprivileged account, never root.
- No capabilities, `NoNewPrivileges`, and a system call filter.
- A read-only system and no access to home folders, with only its own state folder writable.
- Network access limited to IPv4, IPv6 and local sockets.
- A settings file for secrets, owned by root and readable by the service's group only, which an upgrade never replaces.

On Windows, CppBoot doesn't install a service yet.

## Supply chain

- The installers check every download against `SHA256SUMS` before using it. MSYS2's own installer has no published checksum, and the installation guide says so.
- SDKs are built by GitHub Actions from a tag, and tested on clean machines before a release is published.
- Third-party libraries come from the operating system's or MSYS2's packages. A package that `cppboot package` builds includes copies of them, so it needs rebuilding when one gets a security fix.
- Only the owner can change the repository.

## Memory safety

C++ can't guarantee memory safety. CppBoot reduces the risk with bounded input, ownership the container manages, a hardened standard library in every build, sanitizers in CI, and stress tests under them ([005](005-memory-management.md), [016](016-ownership.md)). A memory error a remote client can cause counts as a security problem.

## Not included

- An independent security review, which should come before 1.0.
- CORS, OAuth2, refresh tokens, token revocation, asymmetric signing and secret rotation ([012](012-security.md#not-included)).
- Audit logging of sign-ins and permission failures.
- Running as a Windows service, with the same restrictions as on Linux.
- Signed releases. Checksums detect a damaged download, but not a release replaced by someone who can also replace its checksums.
