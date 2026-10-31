# Phase 9: Production readiness

**Status:** In progress.

## Done

- Rate limits with `CPPBOOT_RATE_LIMIT`, and on the sign-in route of new `--security` projects
- `Request::clientAddress()`, and `cppboot.server.trusted_proxies` for servers behind a proxy

## Next, in order

1. HTTPS in the server, so that it can protect tokens and passwords without a proxy
2. Running as a Linux service, with systemd, packaging and a standalone install
3. Stress testing: long runs that look for memory leaks and failures under sustained load
4. Preparing for 1.0: a stable API policy, and a full review of the documentation

## Done when

A project created by `cppboot create` can run as a service on a Linux server with no proxy in front of it, and has survived a long stress test. This has to pass on the same CI as earlier phases.

An independent security review should also happen before 1.0, because the project's own tests can't replace one.

## Left for later

- Limits by user or account, and limits shared between servers
- CORS
