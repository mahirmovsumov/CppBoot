# Phase 9: Production readiness

**Status:** Complete. CI passes on Linux with GCC, Clang and the sanitizers, and on Windows with GCC. Version 1.0 hasn't been released, because the [release checklist](../release-checklist.md) has decisions only the owner can make.

## Done

- Rate limits with `CPPBOOT_RATE_LIMIT`, and on the sign-in route of new `--security` projects
- `Request::clientAddress()`, and `cppboot.server.trusted_proxies` for servers behind a proxy
- HTTPS in the server, with settings under `cppboot.server.tls`
- Components can take plain references, which the container keeps valid, with build checks and checkers for memory bugs
- `cppboot package`, with a standalone package, and on Linux a systemd service with limited access. CI installs a real one and tests it
- [Stress tests](../benchmarks.md#stress-tests), which found and led to fixes for two ways that overload could stop a server from serving
- [What 1.0 promises](../stability.md), a security policy and a review of the documentation

## Done when

A project created by `cppboot create` can run as a service on a Linux server with no proxy in front of it, and has survived a long stress test. This has to pass on the same CI as earlier phases.

`linux-service` in CI installs a real application as a service, and the Stress workflow passed for 30 minutes in each of 5 builds, with no unexpected results.

## Not done

An independent security review should happen before 1.0, because the project's own tests can't replace one. It hasn't happened yet.

## Left for later

- Limits by user or account, and limits shared between servers
- CORS
- HTTP/2, and loading a renewed certificate without a restart
- A standalone download of the SDK, which still needs a compiler and libraries installed
- Windows services, and upgrades with no downtime
- Stress runs of days rather than minutes

## Next

The next step is the owner's: the decisions in the [release checklist](../release-checklist.md). After 1.0, Phase 7 adds enterprise integration, such as Redis and Kafka.
