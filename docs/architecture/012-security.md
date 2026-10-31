# 012: Security

**Status:** Accepted.

## Principles

**Deny by default.** Once a project uses security, a route needs a token unless it's marked public. The generator enforces this, because the route's check is in generated code, so a forgotten mark can't open a route.

**Fail closed.** A missing or short secret stops startup, because the generated router factory resolves `TokenService` even if every route is public. There's no setting to turn checks off. A security configuration failure must never fall back to running unprotected.

**A mark that does nothing is an error.** A `CPPBOOT_ROLE` on a function that isn't a route, or on a class that isn't a controller, would look like protection that isn't there, so it fails the build.

## When security is on

There's no setting for it. The generator turns it on when it sees a security mark, a `Principal` parameter, or a dependency on `TokenService`. A project with none of these has no authentication at all, so a web project with no security marks is open, and that's the one case where deny by default doesn't apply.

The alternatives were worse. A setting could be turned off by mistake in production, without changing any code. Always requiring tokens would make simple internal services awkward.

## Why generated checks, not middleware

Checks are in each route's generated code, rather than in middleware that reads a route table:

- A `Principal` can be passed to the function as a normal parameter, without changing `Request` or adding a place to store it.
- Each route's rule is visible in one place in the generated file.
- Middleware can't skip the check by mistake, or by throwing.

The cost is that middleware runs before authentication, and has no `Principal`.

## Tokens

Tokens are JWTs, signed with HMAC-SHA256 through OpenSSL.

| Attack | Defense |
|---|---|
| `alg: none` and algorithm switching | The signature is always checked with HS256, whatever the token says, and any other `alg` is rejected |
| Forged or altered tokens | Signatures compared in constant time with `CRYPTO_memcmp` |
| Several spellings of one token | Strict base64url: padding, other characters and nonzero unused bits are rejected |
| Attacks on the JSON parser | Nothing is parsed until the signature passes |
| Tokens that never expire | `exp` is required |
| Tokens from another service | `iss` is required and must match |
| Weak secrets | At least 32 bytes, checked at startup |

The clock leeway is 30 seconds, to allow for differences between servers.

HS256 means anything able to verify tokens can also create them. That's right for one service. Several services would need asymmetric signing such as RS256, which is a later addition.

## Passwords

Passwords use scrypt with N = 2^15, r = 8 and p = 1, and a 16-byte random salt, which uses about 32 MB. OpenSSL provides it, so there's no new dependency. Argon2id is a newer choice, but isn't available in all OpenSSL versions CppBoot supports.

The parameters are stored with each hash, in the form `scrypt$15$8$1$salt$hash`, so that they can be raised later without invalidating existing passwords. A stored hash is treated as untrusted: limits on its parameters stop a bad one from using all of the server's memory, and an invalid one returns false.

## Middleware

`CPPBOOT_MIDDLEWARE(order)` makes a class a component, and the generated router factory adds them in order. Middleware with no controllers fails the build, because nothing would run it.

## Dependencies

`cppboot-security` depends on `cppboot-web`, because it works with requests and responses. OpenSSL is private to it, and only its crypto library is linked. Web projects without security don't link it.

## Not included

HTTPS in the server, rate limiting, CORS, OAuth2, refresh tokens, token revocation, asymmetric signing, secret rotation and audit logging are left for later. For now, a proxy can provide HTTPS and rate limiting.
