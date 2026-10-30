#pragma once

#include <cppboot/base/annotations.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace api {

struct CPPBOOT_JSON Address {
    std::string city;
};

struct CPPBOOT_JSON User {
    int id{0};
    std::string name;
    Address address;
    std::optional<std::string> nickname;
    std::vector<int> scores;
};

struct CPPBOOT_JSON NewUser {
    std::string name;
    std::optional<std::string> nickname;
};

class CPPBOOT_SERVICE UserStore {
public:
    User find(int id) const {
        return User{id, "user-" + std::to_string(id), Address{"Baku"}, std::nullopt, {1, 2}};
    }
};

class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(std::shared_ptr<UserStore> store) : store_(std::move(store)) {}

    CPPBOOT_GET("/{id}") User get(int id) const { return store_->find(id); }

    CPPBOOT_GET("/") std::vector<User> list(int limit, std::optional<std::string> prefix) const {
        std::vector<User> users;
        for (int id = 1; id <= limit; ++id) {
            User user = store_->find(id);
            user.name = prefix.value_or("") + user.name;
            users.push_back(std::move(user));
        }
        return users;
    }

    CPPBOOT_POST("/") cppboot::web::Response create(const NewUser& body) const {
        auto response = cppboot::web::Response::text(body.name + "/" + body.nickname.value_or("-"), 201);
        response.setHeader("Location", "/api/users/1");
        return response;
    }

    CPPBOOT_POST("/copies") cppboot::web::WithStatus<NewUser> copy(const NewUser& body) const {
        return {201, body};
    }

    CPPBOOT_DELETE("/{id}") void remove(int id) {
        if (id == 0) {
            throw cppboot::web::HttpError(409, "User 0 can't be removed");
        }
    }

    CPPBOOT_GET("/echo/{word}") std::string echo(const std::string& word,
                                                 const cppboot::web::Request& request) const {
        return word + ":" + request.path();
    }

private:
    std::shared_ptr<UserStore> store_;
};

}  // namespace api
