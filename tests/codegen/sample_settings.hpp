#pragma once

#include <chrono>
#include <cppboot/base/annotations.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shop {

// An ordinary struct. Its fields get keys under the field that holds it, such as shop.retry.attempts.
struct RetrySettings {
    CPPBOOT_RANGE(1, 10) int attempts{3};
    std::chrono::milliseconds delay{200};
};

struct CPPBOOT_CONFIG("shop") ShopSettings {
    CPPBOOT_REQUIRED CPPBOOT_NOT_EMPTY std::string name;
    CPPBOOT_RANGE(1, 65535) int port{8080};
    bool open{true};
    CPPBOOT_RANGE(0, 0.5) double tax{0.2};
    std::optional<std::string> motto;
    CPPBOOT_LENGTH(0, 3) std::vector<std::string> hosts;
    std::chrono::seconds timeout{30};
    RetrySettings retry;
    std::uint8_t level{1};
    int pin_secret{0};
};

// Its prefix is inside the one above, which leaves these settings to this struct.
struct CPPBOOT_CONFIG("shop.mail") MailSettings {
    std::string host{"localhost"};
    std::uint16_t port{25};
};

struct CPPBOOT_JSON NewOrder {
    CPPBOOT_NOT_EMPTY CPPBOOT_LENGTH(1, 8) std::string product;
    CPPBOOT_RANGE(1, 100) int quantity{1};
    CPPBOOT_MIN(0) std::optional<double> discount;
    CPPBOOT_LENGTH(1, 2) std::vector<std::string> tags;
};

struct CPPBOOT_JSON Basket {
    NewOrder first;
};

// A component takes its settings like any other component.
class CPPBOOT_SERVICE Greeter {
public:
    Greeter(const ShopSettings& shop, const MailSettings& mail) : shop_(shop), mail_(mail) {}

    std::string greet() const { return "Welcome to " + shop_.name + " via " + mail_.host; }

private:
    const ShopSettings& shop_;
    const MailSettings& mail_;
};

class CPPBOOT_CONTROLLER("/orders") OrderController {
public:
    CPPBOOT_POST("/") NewOrder create(const NewOrder& order) const { return order; }

    CPPBOOT_POST("/baskets") Basket createBasket(const Basket& basket) const { return basket; }

    CPPBOOT_GET("/") std::string list(CPPBOOT_RANGE(1, 50) int size,
                                      CPPBOOT_MIN(1) std::optional<int> page) const {
        return std::to_string(size) + "/" + std::to_string(page.value_or(1));
    }

    CPPBOOT_GET("/{code}") std::string get(CPPBOOT_LENGTH(3, 3) const std::string& code) const {
        return code;
    }
};

}  // namespace shop
