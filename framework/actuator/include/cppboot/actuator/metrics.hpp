#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cppboot::actuator {

/// Labels split a metric into separate series, such as {"status", "200"}. Each new combination uses
/// memory for as long as the application runs, so never use values that clients choose, such as a
/// path or a user name.
using Labels = std::vector<std::pair<std::string, std::string>>;

/// A value that only goes up, such as a number of requests.
class Counter {
public:
    void increment(std::uint64_t amount = 1) noexcept {
        value_.fetch_add(amount, std::memory_order_relaxed);
    }
    std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

/// A value that goes up and down, such as requests in progress.
class Gauge {
public:
    void set(double value) noexcept { value_.store(value, std::memory_order_relaxed); }
    void add(double amount) noexcept { value_.fetch_add(amount, std::memory_order_relaxed); }
    double value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0};
};

/// Counts values in ranges, which shows their spread, such as how long requests take.
class Histogram {
public:
    /// The upper bounds must be sorted. A range for everything larger is added automatically.
    explicit Histogram(std::vector<double> upper_bounds);

    void observe(double value) noexcept;

    const std::vector<double>& upperBounds() const noexcept { return upper_bounds_; }
    /// The number of values at or below each bound, in order, then the total count.
    std::vector<std::uint64_t> cumulativeCounts() const;
    double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

private:
    std::vector<double> upper_bounds_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
    std::atomic<double> sum_{0};
};

/// Holds the application's metrics, and writes them in Prometheus text format. It's safe to use from
/// any thread. A metric's reference stays valid for as long as this exists, so hold on to it where
/// speed matters, rather than looking it up for every use.
class Metrics {
public:
    /// Typical request times, in seconds.
    static const std::vector<double>& defaultDurationBuckets();

    Metrics();
    ~Metrics();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    /// These return the existing metric for a name and labels, or create it. An invalid name, or a
    /// name already used for another kind of metric, throws cppboot::Exception.
    Counter& counter(std::string_view name, std::string_view help, const Labels& labels = {});
    Gauge& gauge(std::string_view name, std::string_view help, const Labels& labels = {});
    Histogram& histogram(std::string_view name, std::string_view help, const Labels& labels = {},
                         const std::vector<double>& upper_bounds = defaultDurationBuckets());

    /// Adds a gauge whose value is read when metrics are collected. The function must be quick, and
    /// must not use this object.
    void gaugeFunction(std::string_view name, std::string_view help,
                       std::function<double()> read);

    /// Returns every metric in Prometheus text format.
    std::string render() const;

private:
    struct Family;

    Family& family(std::string_view name, std::string_view help, std::string_view type);

    // Not a std::shared_mutex, which fails under contention on MinGW: its read lock can return an
    // error, and a failed lock would let threads corrupt this map. This is only held briefly.
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Family>, std::less<>> families_;
};

}  // namespace cppboot::actuator
