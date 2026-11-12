# CppBoot documentation

## Using CppBoot

Start with the tutorial, then use the guides as a reference.

- [Installation](guides/installation.md): the installers for Windows and Linux, and `cppboot doctor`
- [Editors](guides/editors.md): building, testing and debugging in Visual Studio Code, and code completion in any editor
- [Getting started](tutorials/getting-started.md): install CppBoot and create your first application
- [Build a REST API](tutorials/rest-api.md): controllers, JSON and error handling
- [Store data in a database](tutorials/database.md): queries, migrations and schema changes
- [Add sign-in and protected routes](tutorials/security.md): tokens, roles and passwords
- [Modules and workers](guides/modules.md): add functionality and background work
- [Dependency injection](guides/dependency-injection.md): components, lifetimes and scopes
- [Ownership and memory safety](guides/memory-safety.md): plain references, build checks, and tools that find memory bugs
- [Deployment](guides/deployment.md): packaging, and running as a Linux service
- [Web server](guides/web.md): controllers, JSON, routes and middleware
- [Database](guides/database.md): queries, row mapping, transactions and migrations
- [Security](guides/security.md): protected routes, tokens, passwords and middleware
- [Health and metrics](guides/actuator.md): health checks and Prometheus metrics
- [Configuration](guides/configuration.md): settings, profiles and overrides
- [Command-line tool](guides/cli.md): every `cppboot` command

## Project status

- [Project status](project-status.md): what works today, what's next, and the main decisions
- [Benchmarks](benchmarks.md): speed and capacity on one core with 2 GB, and stress tests
- [What 1.0 promises](stability.md): what stays stable, and what can change
- [Release checklist](release-checklist.md): what a release needs, including what's still to decide
- [Security policy](../SECURITY.md): how to report a problem
- [Changelog](../CHANGELOG.md): what has been added so far
- [Phase 1 roadmap](roadmap/phase-1.md): the finished runtime and CLI
- [Phase 2 roadmap](roadmap/phase-2.md): the finished dependency injection
- [Phase 3 roadmap](roadmap/phase-3.md): the finished web framework
- [Phase 5 roadmap](roadmap/phase-5.md): the finished database support
- [Phase 6 roadmap](roadmap/phase-6.md): the finished security support
- [Phase 8 roadmap](roadmap/phase-8.md): the finished observability support
- [Phase 9 roadmap](roadmap/phase-9.md): the finished production readiness work

## Contributing to CppBoot

- [How CppBoot works](architecture/README.md): the architecture as a whole, with diagrams, and an index of the records below
- [System architecture](architecture/001-system-architecture.md)
- [Core runtime](architecture/002-core-runtime.md)
- [Module system](architecture/003-module-system.md)
- [Dependency injection](architecture/004-dependency-injection.md)
- [Memory management](architecture/005-memory-management.md)
- [Concurrency model](architecture/006-concurrency-model.md)
- [Platform abstraction](architecture/007-platform-abstraction.md)
- [Security architecture](architecture/008-security-architecture.md)
- [Code generation](architecture/009-code-generation.md)
- [Web server](architecture/010-web-server.md)
- [Data access](architecture/011-data-access.md)
- [Security](architecture/012-security.md)
- [Observability](architecture/013-observability.md)
- [Rate limiting](architecture/014-rate-limiting.md)
- [HTTPS](architecture/015-https.md)
- [Ownership and lifetimes](architecture/016-ownership.md)
- [Configuration](architecture/017-configuration.md)
- [JSON](architecture/018-json.md)
- [Command-line tool and SDK](architecture/019-cli-and-sdk.md)
- [C++20 coding standard](standards/cpp20-coding-standard.md)
