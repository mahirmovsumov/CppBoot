#pragma once

#include <cppboot/base/annotations.hpp>
#include <memory>
#include <string>
#include <utility>

namespace demo {

class UserRepository {
public:
    virtual ~UserRepository() = default;
    virtual std::string findName(int id) const = 0;
};

class CPPBOOT_SERVICE MemoryUserRepository : public UserRepository {
public:
    std::string findName(int id) const override { return "user-" + std::to_string(id); }
};

class CPPBOOT_SERVICE UserService {
public:
    explicit UserService(std::shared_ptr<UserRepository> repository)
        : repository_(std::move(repository)) {}

    std::string greeting(int id) const { return "Hello, " + repository_->findName(id); }

private:
    std::shared_ptr<UserRepository> repository_;
};

namespace web {

class CPPBOOT_SERVICE UserController {
public:
    UserController(const std::shared_ptr<UserService>& users,
                   std::shared_ptr<const UserRepository> repository)
        : users_(users), repository_(std::move(repository)) {}

    std::string handle(int id) const { return users_->greeting(id) + " / " + repository_->findName(id); }

    const UserService* users() const { return users_.get(); }

private:
    std::shared_ptr<UserService> users_;
    std::shared_ptr<const UserRepository> repository_;
};

}  // namespace web

// Takes its dependencies as plain references, one to an interface and one const.
class CPPBOOT_SERVICE ReportService {
public:
    ReportService(UserRepository& repository, const UserService& users)
        : repository_(repository), users_(users) {}

    std::string report(int id) const { return repository_.findName(id) + "/" + users_.greeting(id); }

    const UserService* users() const { return &users_; }

private:
    UserRepository& repository_;
    const UserService& users_;
};

// Mixes both forms.
class CPPBOOT_SERVICE MixedService {
public:
    MixedService(ReportService& reports, std::shared_ptr<UserService> users)
        : reports_(reports), users_(std::move(users)) {}

    std::string mixed(int id) const { return reports_.report(id) + "/" + users_->greeting(id); }

private:
    ReportService& reports_;
    std::shared_ptr<UserService> users_;
};

// Not marked, so the generator must leave it out.
class HelperWithoutAnnotation {
public:
    explicit HelperWithoutAnnotation(int) {}
};

}  // namespace demo
