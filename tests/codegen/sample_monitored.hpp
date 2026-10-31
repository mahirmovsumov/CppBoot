#pragma once

#include <cppboot/actuator/actuator.hpp>
#include <cppboot/base/annotations.hpp>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace monitored {

class CPPBOOT_SERVICE QueueHealth : public cppboot::actuator::HealthIndicator {
public:
    std::string name() const override { return "queue"; }

    void check() override {
        if (broken.load()) {
            throw std::runtime_error("The queue is down");
        }
    }

    std::atomic<bool> broken{false};
};

class CPPBOOT_SERVICE DiskHealth : public cppboot::actuator::HealthIndicator {
public:
    std::string name() const override { return "disk"; }
    void check() override {}
};

class CPPBOOT_CONTROLLER("/orders") OrderController {
public:
    explicit OrderController(std::shared_ptr<cppboot::actuator::Metrics> metrics)
        : created_(metrics->counter("orders_created_total", "Orders created")) {}

    CPPBOOT_POST("/") void create() { created_.increment(); }

private:
    cppboot::actuator::Counter& created_;
};

}  // namespace monitored
