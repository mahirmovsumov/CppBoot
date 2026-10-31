# 014: Rate limiting

**Status:** Accepted.

## The problem

A sign-in route is the cheapest way to attack a CppBoot server. Each password check takes about 100 ms of CPU and 32 MB on purpose, so a few clients can use up every worker thread. It also lets an attacker guess passwords as fast as the server can answer.

## Where the check runs

The check is in each route's generated code, before the token check and before the request body is parsed. It runs before the token check so that it also limits token guessing, even though that means an unauthenticated client can use up a guarded route's limit for its own address.

A limit can't be read wrongly at runtime, because the generator parses it. An unreadable limit fails the build, since leaving the route unlimited would look like protection that isn't there. So there's one parser, in the generator, and hand-written code uses `RateLimit{5, std::chrono::minutes{1}}`.

Limits aren't on by default. No single number suits every route, and a low default would break normal APIs in a way that's hard to diagnose. The project template does mark its sign-in route.

## The algorithm

Each client has one time, `free_at`, when its limit will be fully available again. With `interval = window / requests`, a request is allowed if `free_at - now <= window - interval`, and then `free_at` moves forward by `interval`. This is the generic cell rate algorithm, which behaves like a token bucket that holds `requests` tokens.

Compared with a counter that resets each window, it can't be beaten by sending a burst at the end of one window and another at the start of the next. Compared with a floating-point token bucket, it stores less, and its arithmetic is exact.

## Bounded memory

An attacker can choose many addresses, especially with IPv6, so the table of clients has to be bounded:

- Clients are kept in a list, most recently seen first, with a hash map to find them.
- A client unseen for a whole window has its full limit back, so forgetting it changes nothing. These are all at the end of the list, so removing them is cheap, and needs no background thread.
- When the table is full, the client seen longest ago is forgotten.

Rejected requests count as being seen, so an attacker's own entry stays near the front while it keeps trying. To reset its limit by filling the table, an attacker needs `max_clients` different addresses, or 100,000 by default, and enough requests to use them all. At about 133 bytes per client, the default bound is about 13 MB per limited route.

IPv6 addresses are limited by /64 network, because one customer can usually use all 2^64 addresses in theirs.

## Client addresses

`Request::clientAddress()` is the address of the connection, because `X-Forwarded-For` can be sent by anyone. Rate limiting by that header without checking its source would let any client reset its limit by changing it.

`cppboot.server.trusted_proxies` lists the proxies whose header can be believed. The server reads it from the end, because each proxy adds to the end, and takes the first address that isn't a trusted proxy. A client can only write what comes before the addresses its proxies added, so it can't forge a result. If an entry can't be read, the last address that could be believed is used, which at worst puts clients in the proxy's shared limit.

A range covering every address stops startup, because it would let any client choose its own address.

The peer's address and whether it's trusted are worked out once for each connection. Only requests from trusted proxies pay for parsing the header.

## Cost

Measured in Release mode on an i7-8750H:

| Case | Time for each check |
|---|---|
| Allowed, IPv4 | About 0.2 µs |
| Rejected, IPv4, including building the response | About 0.5 µs |
| IPv6 | About 1.4 µs |

A request costs about 77 µs in total on that laptop, at its [measured rate](../benchmarks.md) of about 13,000 requests per second, so a limit adds under 1% for IPv4 and about 2% for IPv6. All of a limiter's clients share one mutex, and one limiter managed about 2.7 million checks per second across 4 or 12 threads, which is far more than the server can send it.

## Limits of this design

Counts are per process and in memory, so several servers each apply the limit, and a restart clears it. Sharing counts between servers needs an outside store such as Redis, which comes after 1.0.

Limits apply per address, not per account. An attacker with many addresses can still guess one account's password slowly. Locking accounts lets an attacker lock out real users, so that's left to applications.
