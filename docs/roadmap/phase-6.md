# Phase 6: Security

**Status:** Complete on Windows. It's finished once CI passes on Linux.

## Done

- `CPPBOOT_MIDDLEWARE`: middleware in projects that use controllers
- `TokenService`: JWT signing and verification with HS256 through OpenSSL
- `hashPassword` and `verifyPassword`: salted scrypt hashes
- `CPPBOOT_PUBLIC` and `CPPBOOT_ROLE`, with every other route protected by default
- `Principal` parameters, which say who sent a request
- A build failure for any security mark that would do nothing
- A startup failure for a missing or short secret
- `cppboot create --web --security`

## Done when

A project made by `cppboot create --web --security` signs users in, and rejects requests with no token, altered tokens and missing roles. A route with no marks is protected. This has to pass on Linux and Windows.

Generated routes are tested for each case on every CI run. The tutorial's `curl` requests were checked by hand on Windows.

## Left for later

- HTTPS in the server, rate limiting and CORS
- OAuth2 and OpenID Connect
- Refresh tokens, and revoking tokens
- Asymmetric signing, and rotating secrets
- Audit logging
- `cppboot create` combining `--security` with a database option

## Next

Phase 8 is observability: health and metrics endpoints.
