# Release checklist

Work through this for each release. A major release needs all of it, and later releases can skip what hasn't been affected.

## Decisions only the owner can make

These block 1.0:

- [x] **A license.** Apache License 2.0, with `NOTICE` and `THIRD_PARTY.md`.
- [ ] **An independent security review.** The project's own tests can't replace one. It should cover token handling, password storage, HTTP parsing limits, TLS settings and the service's limits.
- [x] **Making the repository public.** Done, with the owner as the only account that can push.
- [ ] **How people get the SDK.** Building from source is the only way today, and it needs the compiler and libraries installed.

## Before each release

- [ ] All CI jobs pass on the commit being released, including `linux-service` and both `linux-stress` jobs.
- [ ] The Stress workflow has passed on that commit, or a very close one, with at least 30 minutes for each run.
- [ ] `scripts/check-docs.py` finds no problems.
- [ ] A person has followed the [getting started tutorial](tutorials/getting-started.md) on a clean machine.
- [ ] The Benchmark workflow has run, and `docs/benchmarks.md` still describes the results.
- [ ] Dependencies have no unfixed security problems that affect CppBoot, especially OpenSSL.

## Making the release

1. Set the version in the top `CMakeLists.txt`.
2. Change `Unreleased` in `CHANGELOG.md` to the version and date.
3. Update the status in `README.md` and `docs/project-status.md`.
4. Commit, and let CI pass on that commit.
5. Tag it `vX.Y.Z`, and push the tag.

## After a security fix in a dependency

A package includes its own copies of its libraries, so anyone using a package has to build and install a new one. Say so in the release notes.
