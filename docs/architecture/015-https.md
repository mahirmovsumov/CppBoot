# 015: HTTPS

**Status:** Accepted.

## Principles

**HTTPS or nothing.** There's no `enabled` setting. Setting any `cppboot.server.tls` key means HTTPS was wanted, so a mistake such as a missing file stops startup. An `enabled` flag would add a way to set up certificates and still serve plain HTTP without noticing.

**No settings that weaken it.** The only choice is the minimum version, and it can only go up from TLS 1.2. Ciphers aren't configurable, so nobody has to know which ones are safe. If one becomes unsafe, that's a CppBoot release.

## How it fits the server

Boost.Beast's `ssl_stream` wraps the same `tcp_stream` that plain connections use. The code that reads requests and writes responses is one template for both, so behavior such as limits, timeouts and keep-alive can't differ between them. A connection holds a `std::variant` of the two.

OpenSSL is private to `cppboot-web`, and its headers appear only in `tls.cpp`. Every web project now links to it, including those that don't use HTTPS. Choosing at build time would have added a build option, and a binary that can't serve HTTPS when its settings ask for it.

## Settings applied

| Setting | Reason |
|---|---|
| TLS 1.2 or newer | Older versions have known weaknesses |
| ECDHE with AES-GCM or ChaCha20 on TLS 1.2 | Forward secrecy and authenticated encryption. TLS 1.3 only has good choices, so its defaults stay |
| No compression | Allows attacks that recover secrets, such as CRIME |
| No renegotiation | It's complex, costs the server CPU, and has had vulnerabilities |
| The server's cipher preference | Stops a client from choosing the weakest cipher both sides allow |
| No session cache | Tickets let clients resume without the server storing anything |
| `SSL_MODE_RELEASE_BUFFERS` | Frees buffers on idle connections |

## Failure modes

**Keys with passwords.** By default, OpenSSL asks for a key's password on the terminal, which would hang a service forever with no error. A password callback returning nothing makes this an ordinary startup failure.

**Slow handshakes.** A handshake costs the server work before it knows anything about the client, so it gets at most 10 seconds, or `request_timeout` if that's shorter. `max_connections` limits how many can be waiting.

**Failed handshakes aren't logged.** Anyone can cause them, so logging them would let anyone fill the log, and only the client can do anything useful about them.

**Closing.** A response is followed by a TLS `close_notify`, so that a client can tell a complete response from one an attacker cut short. Closing waits for the client's reply for at most 2 seconds. A connection that has already failed skips this, because OpenSSL doesn't allow it.

**Expired certificates.** These are logged but don't stop startup. Clients are already refusing the server, so failing to start wouldn't help them.

## Known costs

All TLS work runs on network threads, where each connection stays on one, so that connection state needs no locks. The [benchmarks](../benchmarks.md#https) show the effects:

- **Limited use of more cores.** Handshakes and encryption spread across [network threads](../benchmarks.md#network-threads), up to 4 by default.
- **Handshakes delay connections on the same thread.** A flood of them slows requests on open connections, about 9 times less with 4 threads than with one. Rate limits can't help, because they apply after the handshake. On one core, nothing helps but limiting connections.
- **Memory.** An open connection uses 76 to 95 KB. Turning off the session cache barely changed this, and where the rest goes hasn't been measured. This is the largest limit on how many connections a small server can hold, at about 25,000 with 2 GB, against 150,000 for plain HTTP.

## Left for later

- Loading a renewed certificate without a restart
- HTTP/2, which also needs a different server model
- Several certificates on one server (SNI), and client certificates
- OCSP stapling
