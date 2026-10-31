#pragma once

#include <cppboot/base/annotations.hpp>
#include <cppboot/security/password.hpp>
#include <cppboot/security/token.hpp>
#include <cppboot/web/http.hpp>
#include <cppboot/web/router.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace secured {

struct CPPBOOT_JSON Credentials {
    std::string name;
    std::string password;
};

struct CPPBOOT_JSON TokenReply {
    std::string token;
};

struct CPPBOOT_JSON Profile {
    std::string name;
    std::vector<std::string> roles;
};

class CPPBOOT_CONTROLLER("/auth") AuthController {
public:
    explicit AuthController(std::shared_ptr<cppboot::security::TokenService> tokens)
        : tokens_(std::move(tokens)), hash_(cppboot::security::hashPassword("correct horse")) {}

    CPPBOOT_PUBLIC CPPBOOT_POST("/login") TokenReply login(const Credentials& credentials) const {
        if (credentials.name != "ada" ||
            !cppboot::security::verifyPassword(credentials.password, hash_)) {
            throw cppboot::web::HttpError(401, "Wrong name or password");
        }
        return {tokens_->issue(credentials.name, {"admin"})};
    }

private:
    std::shared_ptr<cppboot::security::TokenService> tokens_;
    std::string hash_;
};

class CPPBOOT_CONTROLLER("/app") AppController {
public:
    CPPBOOT_PUBLIC CPPBOOT_GET("/health") std::string health() const { return "ok"; }

    // Has no mark on purpose. Deny by default means this still needs a token.
    CPPBOOT_GET("/forgotten") std::string forgotten() const { return "secret"; }

    CPPBOOT_GET("/me") Profile me(const cppboot::security::Principal& who) const {
        return {who.subject, who.roles};
    }

    CPPBOOT_ROLE("admin") CPPBOOT_GET("/admin") std::string admin() const { return "admin area"; }

    CPPBOOT_ROLE("auditor") CPPBOOT_ROLE("owner") CPPBOOT_GET("/audit")
    std::string audit() const { return "audit area"; }
};

class CPPBOOT_CONTROLLER("/staff") CPPBOOT_ROLE("staff") StaffController {
public:
    CPPBOOT_GET("/") std::string list() const { return "staff only"; }
    CPPBOOT_ROLE("admin") CPPBOOT_GET("/promote") std::string promote() const { return "promoted"; }
    CPPBOOT_PUBLIC CPPBOOT_GET("/about") std::string about() const { return "about"; }
};

class CPPBOOT_CONTROLLER("/open") CPPBOOT_PUBLIC OpenController {
public:
    CPPBOOT_GET("/") std::string list() const { return "open"; }
    CPPBOOT_ROLE("admin") CPPBOOT_GET("/locked") std::string locked() const { return "locked"; }
};

class CPPBOOT_CONTROLLER("/limited") CPPBOOT_PUBLIC CPPBOOT_RATE_LIMIT("2/minute") LimitedController {
public:
    CPPBOOT_GET("/first") std::string first() const { return "first"; }
    CPPBOOT_GET("/second") std::string second() const { return "second"; }
    CPPBOOT_RATE_LIMIT("1/hour") CPPBOOT_GET("/strict") std::string strict() const { return "strict"; }
};

class CPPBOOT_CONTROLLER("/guarded") GuardedController {
public:
    CPPBOOT_RATE_LIMIT("1/minute") CPPBOOT_GET("/") std::string read() const { return "guarded"; }
};

class CPPBOOT_SERVICE TraceLog {
public:
    std::string read() const { return order_; }
    void add(const std::string& name) { order_ += name; }

private:
    CPPBOOT_UNCHECKED("These tests call it from one thread") std::string order_;
};

class CPPBOOT_MIDDLEWARE(20) InnerMiddleware {
public:
    explicit InnerMiddleware(std::shared_ptr<TraceLog> log) : log_(std::move(log)) {}

    cppboot::web::Response handle(const cppboot::web::Request& request,
                                  const cppboot::web::Next& next) {
        log_->add("inner ");
        return next(request);
    }

private:
    std::shared_ptr<TraceLog> log_;
};

class CPPBOOT_MIDDLEWARE(10) OuterMiddleware {
public:
    explicit OuterMiddleware(std::shared_ptr<TraceLog> log) : log_(std::move(log)) {}

    cppboot::web::Response handle(const cppboot::web::Request& request,
                                  const cppboot::web::Next& next) {
        log_->add("outer ");
        cppboot::web::Response response = next(request);
        response.setHeader("X-Middleware", "ran");
        return response;
    }

private:
    std::shared_ptr<TraceLog> log_;
};

}  // namespace secured
