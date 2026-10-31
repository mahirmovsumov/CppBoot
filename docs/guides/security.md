# Security

`cppboot::security` adds sign-in with JSON Web Tokens (JWT), protected routes, roles and password hashing.

To start a project that uses it, run the command below. The [security tutorial](../tutorials/security.md) walks through it.

```powershell
cppboot create my-api --web --security
```

## Protected by default

**Once a project uses security, every route needs a valid token unless you mark it `CPPBOOT_PUBLIC`.** A route you forget to mark is locked, not exposed.

A project uses security as soon as any of these appears: a `CPPBOOT_PUBLIC` or `CPPBOOT_ROLE` mark, a `Principal` parameter, or a component that takes a `TokenService`. A web project with none of these has no authentication.

```cpp
class CPPBOOT_CONTROLLER("/api") ApiController {
public:
    // Anyone can call this.
    CPPBOOT_PUBLIC CPPBOOT_GET("/health") std::string health() const;

    // This has no mark, so it needs a token.
    CPPBOOT_GET("/orders") std::vector<Order> orders() const;

    // This needs a token with the admin role.
    CPPBOOT_ROLE("admin") CPPBOOT_DELETE("/orders/{id}") void remove(int id);
};
```

| Request | Response |
|---|---|
| No token, or an invalid one | 401, with a `WWW-Authenticate: Bearer` header |
| A valid token without a required role | 403 |
| A valid token with a required role | The route runs |

A 401 never says why a token was rejected, because that would help an attacker.

## Marks

| Mark | On a controller | On a function |
|---|---|---|
| `CPPBOOT_PUBLIC` | All its routes are public | This route is public |
| `CPPBOOT_ROLE("admin")` | All its routes need this role | This route needs this role |

- With several `CPPBOOT_ROLE` marks, any one of the roles is enough.
- A mark on a function replaces those on its controller, so a role on a function inside a public controller still protects that function.

These fail the build, because each one could look like protection that isn't there:

- `CPPBOOT_PUBLIC` and `CPPBOOT_ROLE` together on one function or controller
- A security mark on a function that isn't a route
- A security mark on a class that isn't a controller
- A `Principal` parameter on a public route, because nobody has signed in

## Who sent the request

Add a `Principal` parameter to a route function:

```cpp
CPPBOOT_GET("/me") Profile me(const cppboot::security::Principal& who) const {
    return {who.subject, who.roles};
}
```

`who.subject` is the signed-in user, and `who.hasRole("admin")` checks a role. Use these for rules that depend on the data, such as letting users edit only their own orders.

## Settings

| Key | Default | Purpose |
|---|---|---|
| `cppboot.security.jwt.secret` | Required | Signs tokens. At least 32 bytes |
| `cppboot.security.jwt.issuer` | `cppboot` | Tokens from a different issuer are rejected |
| `cppboot.security.jwt.lifetime_seconds` | `3600` | How long a new token stays valid |

**Set the secret in the environment, never in `cppboot.yaml`.** Anyone who has it can sign in as anyone:

```powershell
$env:CPPBOOT_SECURITY_JWT_SECRET = "at-least-32-random-bytes-go-here"
```

To generate one, run `openssl rand -base64 48`.

The application won't start if the secret is missing or too short, so it can't run without real protection. Configuration logs hide the secret.

## Sign users in

Inject `TokenService`, check the user's password, and issue a token:

```cpp
class CPPBOOT_CONTROLLER("/auth") AuthController {
public:
    AuthController(std::shared_ptr<UserRepository> users,
                   std::shared_ptr<cppboot::security::TokenService> tokens);

    CPPBOOT_PUBLIC CPPBOOT_POST("/login") TokenReply login(const Credentials& credentials) const {
        auto user = users_->findByName(credentials.name);
        // Check a hash even for an unknown name, so that timing doesn't reveal which names exist.
        const std::string& hash = user ? user->password_hash : dummy_hash_;
        if (!cppboot::security::verifyPassword(credentials.password, hash) || !user) {
            throw cppboot::web::HttpError(401, "Wrong name or password");
        }
        return {tokens_->issue(user->name, user->roles)};
    }
};
```

Clients then send the token with each request:

```text
Authorization: Bearer <token>
```

## Passwords

Store only the hash:

```cpp
std::string hash = cppboot::security::hashPassword(password);
bool matches = cppboot::security::verifyPassword(password, hash);
```

Hashes use scrypt with a random salt. Each hash and check takes about 100 ms and 32 MB, on purpose, because that's what makes guessing passwords expensive. It also means a sign-in route is expensive to call, so limit its rate before exposing it. CppBoot has no rate limiting yet, so use your proxy for this.

## Middleware

Middleware runs around every request, and is how you add things like request logs or extra headers. Mark a class with `CPPBOOT_MIDDLEWARE` and an order, and lower numbers run first:

```cpp
class CPPBOOT_MIDDLEWARE(10) RequestLog {
public:
    explicit RequestLog(std::shared_ptr<const cppboot::logging::Logger> logger);

    cppboot::web::Response handle(const cppboot::web::Request& request,
                                  const cppboot::web::Next& next) {
        cppboot::web::Response response = next(request);
        logger_->info("{} {}", request.path(), response.status());
        return response;
    }
};
```

Middleware is a component, so it can take other components in its constructor. It runs before the route's token check, so it sees every request, including those that get rejected, and it has no `Principal`.

## Actuator routes

In a project with the [actuator](actuator.md), health routes stay public, and show status only. Metrics and info stay locked until you set a separate metrics token, and a user's JWT doesn't open them.

## What tokens can and can't do

Tokens can't be revoked. A token stays valid until it expires, even if the user signs out or their account is removed, because the server keeps no record of them. Keep lifetimes short, and check the database on sensitive routes if you need an immediate effect.

The token's contents are signed but not encrypted, so anyone holding a token can read them. Don't put secrets in roles or subjects.

Tokens are sent with each request, so **use HTTPS in production**. CppBoot doesn't serve HTTPS yet, so put a TLS proxy in front of it.

## Not supported yet

- HTTPS in the server itself
- Rate limiting and CORS
- OAuth2 and OpenID Connect
- Refresh tokens, and revoking tokens
- Asymmetric token signing, such as RS256
- Rotating the secret without signing everyone out
- Audit logging
