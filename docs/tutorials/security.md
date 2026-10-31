# Add sign-in and protected routes

In this tutorial, you create a web API with sign-in, then see how it rejects requests without a valid token. It takes about 10 minutes. Follow [Getting started](getting-started.md) first, so that CppBoot is installed and on PATH.

## Create the project

```powershell
cppboot create safe-api --web --security
cd safe-api
```

Try to run it with `cppboot run`. It refuses to start:

```text
[critical] Set cppboot.security.jwt.secret to at least 32 random bytes, with the environment variable CPPBOOT_SECURITY_JWT_SECRET
```

This is deliberate. An application can't run without real protection.

Set the two values it needs. These stay out of your code, so they can't end up in Git. The first signs tokens, and the second is the password of the sample's `admin` account:

```powershell
$env:CPPBOOT_SECURITY_JWT_SECRET = "a-long-random-secret-for-this-tutorial-only"
$env:CPPBOOT_MYAPP_PASSWORD = "tutorial-password"
cppboot run
```

## Sign in

Open a second window. A request with no token is rejected:

```powershell
curl.exe -i localhost:8080/api/me
```

```text
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer

{"error":"Sign in first","status":401}
```

So is a wrong password. `Out-File` writes the request body to a file:

```powershell
'{"name":"admin","password":"wrong"}' | Out-File -Encoding ascii body.json
curl.exe --data-binary "@body.json" localhost:8080/api/login
```

```json
{"error":"Wrong name or password","status":401}
```

Now sign in with the right one, and keep the token:

```powershell
'{"name":"admin","password":"tutorial-password"}' | Out-File -Encoding ascii body.json
$token = (curl.exe -s --data-binary "@body.json" localhost:8080/api/login | ConvertFrom-Json).token
```

Send it with a request:

```powershell
curl.exe -H "Authorization: Bearer $token" localhost:8080/api/me
```

```json
{"name":"admin","roles":["admin"]}
```

## Try to break in

Change one character of the token, and the signature no longer matches:

```powershell
$forged = $token.Substring(0, $token.Length - 1) + "A"
curl.exe -H "Authorization: Bearer $forged" localhost:8080/api/me
```

```json
{"error":"Sign in first","status":401}
```

(If your token already ends in `A`, use another letter.)

Now try to guess the password, with 12 quick attempts:

```powershell
'{"name":"admin","password":"wrong"}' | Out-File -Encoding ascii body.json
1..12 | ForEach-Object { curl.exe -s -o NUL -w "%{http_code} " --data-binary "@body.json" localhost:8080/api/login }
```

After a few 401 responses, you get 429, Too Many Requests. Each client gets 10 sign-in attempts, then one more every 6 seconds. The server stops checking passwords for that client until then, so it stays fast for everyone else.

## See how it works

Open `src/auth.hpp`. The controller has three routes:

```cpp
CPPBOOT_PUBLIC CPPBOOT_RATE_LIMIT("10/minute") CPPBOOT_POST("/login")
TokenReply login(const Credentials& credentials) const;

CPPBOOT_GET("/me") Profile me(const cppboot::security::Principal& who) const;

CPPBOOT_ROLE("admin") CPPBOOT_GET("/report") Profile report(
    const cppboot::security::Principal& who) const;
```

- `login` is marked public, so anyone can call it, but each client can only try so often.
- `me` has no mark. **In a project with security, that means it needs a token.**
- `report` also needs the `admin` role.

## Add a route

Stop the server, and add this to `ApiController`, with no marks at all:

```cpp
CPPBOOT_GET("/forgotten") std::string forgotten() const { return "private data"; }
```

Run `cppboot run`, and call it with no token:

```powershell
curl.exe localhost:8080/api/forgotten
```

```json
{"error":"Sign in first","status":401}
```

You forgot to protect it, and it's protected anyway. To open a route, you have to say so with `CPPBOOT_PUBLIC`.

## Before production

The sample has one account, to keep it short. For a real application:

- Keep accounts in a [database](database.md), with hashes from `hashPassword()`.
- [Serve HTTPS](../guides/web.md#https). If you use a proxy for this, list it in `cppboot.server.trusted_proxies`, or all your users will share one sign-in limit.
- Generate a real secret with `openssl rand -base64 48`.

## Next steps

The [security guide](../guides/security.md) covers every mark, tokens and their limits, and middleware.
