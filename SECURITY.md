# Security policy

## Reporting a problem

**Please don't report a security problem in a public issue.** Use GitHub's private reporting instead: on the repository's **Security** tab, choose **Report a vulnerability**. Include what you found, how to reproduce it, and which version you tested.

## What to expect

CppBoot has one developer, so there's no guaranteed response time. Reports get an answer as soon as possible, and a fix comes before any public details.

Only the latest minor release gets fixes. See [what 1.0 promises](docs/stability.md#which-releases-get-fixes).

## Current status

CppBoot hasn't had an independent security review, so **don't rely on it to protect sensitive data yet**. C++ can't guarantee memory safety, and [the memory safety guide](docs/guides/memory-safety.md) explains what CppBoot does and doesn't check.

## What counts as a problem in CppBoot

- A route that should be protected, but can be reached without a valid token
- A way to forge a token, or to learn a secret or password hash
- A crash, memory error or runaway memory use that a remote client can cause, despite the limits being set
- A setting that silently leaves the application less protected than it says

Weak settings you chose, a leaked secret, and bugs in your own application code aren't problems in CppBoot. Neither is a denial of service from more traffic than a machine can handle, though a small request causing a large amount of work is.

## If you use a package

A package includes its own copies of libraries such as OpenSSL. When one of them gets a security fix, build and install a new package.
