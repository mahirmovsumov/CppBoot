# Build a REST API

In this tutorial, you build a small API for managing users, with JSON requests and responses. It takes about 10 minutes. Follow [Getting started](getting-started.md) first, so that CppBoot is installed and on PATH.

## Create the project

```powershell
cppboot create users-api --web
cd users-api
cppboot run
```

In a second window, send a request:

```powershell
curl.exe localhost:8080/api/users/7
```

```json
{"id":7,"name":"user-7"}
```

Press Ctrl+C in the first window to stop the server.

## Write the API

Replace `src/users.hpp` with this:

```cpp
#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct CPPBOOT_JSON User {
    int id{0};
    std::string name;
    std::optional<std::string> nickname;
};

struct CPPBOOT_JSON NewUser {
    std::string name;
    std::optional<std::string> nickname;
};

class CPPBOOT_SERVICE UserStore {
public:
    User add(const NewUser& details) {
        const std::lock_guard lock{mutex_};
        User user{next_id_++, details.name, details.nickname};
        users_[user.id] = user;
        return user;
    }

    std::optional<User> find(int id) const {
        const std::lock_guard lock{mutex_};
        const auto entry = users_.find(id);
        if (entry == users_.end()) {
            return std::nullopt;
        }
        return entry->second;
    }

    std::vector<User> list(std::size_t limit) const {
        const std::lock_guard lock{mutex_};
        std::vector<User> users;
        for (const auto& [id, user] : users_) {
            if (users.size() == limit) {
                break;
            }
            users.push_back(user);
        }
        return users;
    }

    bool remove(int id) {
        const std::lock_guard lock{mutex_};
        return users_.erase(id) > 0;
    }

private:
    mutable std::mutex mutex_;
    std::map<int, User> users_;
    int next_id_{1};
};

class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserStore& store) : store_(store) {}

    CPPBOOT_GET("/") std::vector<User> list(std::optional<int> limit) const {
        return store_.list(static_cast<std::size_t>(limit.value_or(100)));
    }

    CPPBOOT_GET("/{id}") User get(int id) const {
        auto user = store_.find(id);
        if (!user) {
            throw cppboot::web::HttpError(404, "No user has that ID");
        }
        return *user;
    }

    CPPBOOT_POST("/") cppboot::web::WithStatus<User> create(const NewUser& details) {
        return {201, store_.add(details)};
    }

    CPPBOOT_DELETE("/{id}") void remove(int id) {
        if (!store_.remove(id)) {
            throw cppboot::web::HttpError(404, "No user has that ID");
        }
    }

private:
    UserStore& store_;
};
```

The store needs a mutex because requests run on several threads at once, and they all share one `UserStore`. Without one, the build warns you. The controller takes the store as a plain reference, and CppBoot keeps the store alive for as long as the controller exists.

## Try it

Run `cppboot run`, then send requests from the second window. Windows PowerShell can strip quotes from `curl.exe` arguments, so save the body to a file first:

```powershell
'{"name":"Ada","nickname":"A"}' | Out-File -Encoding ascii body.json
curl.exe -i --data-binary "@body.json" localhost:8080/api/users
```

```text
HTTP/1.1 201 Created
Content-Type: application/json

{"id":1,"name":"Ada","nickname":"A"}
```

The other routes work the same way:

| Request | Result |
|---|---|
| `curl.exe localhost:8080/api/users` | 200 with a list of users |
| `curl.exe "localhost:8080/api/users?limit=1"` | 200 with one user at most |
| `curl.exe localhost:8080/api/users/1` | 200 with that user |
| `curl.exe localhost:8080/api/users/99` | 404 with `No user has that ID` |
| `curl.exe localhost:8080/api/users/abc` | 400, because the ID isn't a number |
| `curl.exe -X DELETE localhost:8080/api/users/1` | 204 with no body |

You wrote no code for routing, JSON, parameter checks or error responses.

## Next steps

The [web server guide](../guides/web.md) covers every rule for parameters and return values, settings and middleware.
