#include "cppboot/actuator/metrics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <mutex>
#include <variant>

#include "cppboot/base/exceptions.hpp"

namespace cppboot::actuator {

namespace {

bool isValidName(std::string_view name, bool allow_colon) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [&](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
               (allow_colon && c == ':');
    });
}

std::string escape(std::string_view text, bool escape_quotes) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        if (c == '\\') {
            result += "\\\\";
        } else if (c == '\n') {
            result += "\\n";
        } else if (c == '"' && escape_quotes) {
            result += "\\\"";
        } else {
            result += c;
        }
    }
    return result;
}

/// Writes labels in a fixed form, which also identifies a series within its family.
std::string labelText(const Labels& labels) {
    std::string text;
    for (const auto& [name, value] : labels) {
        if (!isValidName(name, false)) {
            throw Exception(Error{ErrorCode::InvalidArgument, "Invalid metric label: " + name});
        }
        text += text.empty() ? "" : ",";
        text += name + "=\"" + escape(value, true) + "\"";
    }
    return text;
}

std::string formatNumber(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "+Inf" : "-Inf";
    }
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    static_cast<void>(error);
    return std::string{buffer.data(), end};
}

std::string joinLabels(const std::string& labels, const std::string& extra) {
    if (labels.empty()) {
        return "{" + extra + "}";
    }
    return "{" + labels + "," + extra + "}";
}

std::string bracedLabels(const std::string& labels) {
    return labels.empty() ? std::string{} : "{" + labels + "}";
}

}  // namespace

Histogram::Histogram(std::vector<double> upper_bounds)
    : upper_bounds_(std::move(upper_bounds)),
      counts_(std::make_unique<std::atomic<std::uint64_t>[]>(upper_bounds_.size() + 1)) {
    if (!std::is_sorted(upper_bounds_.begin(), upper_bounds_.end())) {
        throw Exception(Error{ErrorCode::InvalidArgument, "Histogram bounds must be sorted"});
    }
}

void Histogram::observe(double value) noexcept {
    const auto bound = std::lower_bound(upper_bounds_.begin(), upper_bounds_.end(), value);
    const auto index = static_cast<std::size_t>(bound - upper_bounds_.begin());
    counts_[index].fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(value, std::memory_order_relaxed);
}

std::vector<std::uint64_t> Histogram::cumulativeCounts() const {
    std::vector<std::uint64_t> cumulative;
    cumulative.reserve(upper_bounds_.size() + 1);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i <= upper_bounds_.size(); ++i) {
        total += counts_[i].load(std::memory_order_relaxed);
        cumulative.push_back(total);
    }
    return cumulative;
}

struct Metrics::Family {
    using Series = std::variant<std::unique_ptr<Counter>, std::unique_ptr<Gauge>,
                                std::unique_ptr<Histogram>, std::function<double()>>;

    std::string help;
    std::string type;
    std::map<std::string, Series> series;
};

const std::vector<double>& Metrics::defaultDurationBuckets() {
    static const std::vector<double> kBuckets = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1,
                                                 0.25,  0.5,   1,    2.5,   5,    10};
    return kBuckets;
}

Metrics::Metrics() = default;
Metrics::~Metrics() = default;

Metrics::Family& Metrics::family(std::string_view name, std::string_view help,
                                 std::string_view type) {
    if (!isValidName(name, true)) {
        throw Exception(
            Error{ErrorCode::InvalidArgument, "Invalid metric name: " + std::string{name}});
    }

    const auto found = families_.find(name);
    if (found != families_.end()) {
        if (found->second->type != type) {
            throw Exception(Error{ErrorCode::InvalidArgument,
                                  "Metric " + std::string{name} + " is already a " +
                                      found->second->type});
        }
        return *found->second;
    }

    auto created = std::make_unique<Family>();
    created->help = help;
    created->type = type;
    return *families_.emplace(std::string{name}, std::move(created)).first->second;
}

Counter& Metrics::counter(std::string_view name, std::string_view help, const Labels& labels) {
    const std::string key = labelText(labels);
    const std::lock_guard lock{mutex_};

    Family& entries = family(name, help, "counter");
    auto [series, inserted] = entries.series.try_emplace(key, std::unique_ptr<Counter>{});
    auto& metric = std::get<std::unique_ptr<Counter>>(series->second);
    if (inserted) {
        metric = std::make_unique<Counter>();
    }
    return *metric;
}

Gauge& Metrics::gauge(std::string_view name, std::string_view help, const Labels& labels) {
    const std::string key = labelText(labels);
    const std::lock_guard lock{mutex_};

    Family& entries = family(name, help, "gauge");
    auto [series, inserted] = entries.series.try_emplace(key, std::unique_ptr<Gauge>{});
    auto* const metric = std::get_if<std::unique_ptr<Gauge>>(&series->second);
    if (metric == nullptr) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Metric " + std::string{name} + " is read from a function"});
    }
    if (inserted) {
        *metric = std::make_unique<Gauge>();
    }
    return **metric;
}

Histogram& Metrics::histogram(std::string_view name, std::string_view help, const Labels& labels,
                              const std::vector<double>& upper_bounds) {
    const std::string key = labelText(labels);
    const std::lock_guard lock{mutex_};

    Family& entries = family(name, help, "histogram");
    auto [series, inserted] = entries.series.try_emplace(key, std::unique_ptr<Histogram>{});
    auto& metric = std::get<std::unique_ptr<Histogram>>(series->second);
    if (inserted) {
        try {
            metric = std::make_unique<Histogram>(upper_bounds);
        } catch (...) {
            entries.series.erase(series);
            throw;
        }
    }
    return *metric;
}

void Metrics::gaugeFunction(std::string_view name, std::string_view help,
                            std::function<double()> read) {
    const std::lock_guard lock{mutex_};
    family(name, help, "gauge").series.insert_or_assign(std::string{}, std::move(read));
}

std::string Metrics::render() const {
    const std::lock_guard lock{mutex_};

    std::string text;
    for (const auto& [name, family] : families_) {
        text += "# HELP " + name + " " + escape(family->help, false) + "\n";
        text += "# TYPE " + name + " " + family->type + "\n";

        for (const auto& [labels, series] : family->series) {
            if (const auto* counter = std::get_if<std::unique_ptr<Counter>>(&series)) {
                text += name + bracedLabels(labels) + " " + std::to_string((*counter)->value()) + "\n";
            } else if (const auto* gauge = std::get_if<std::unique_ptr<Gauge>>(&series)) {
                text += name + bracedLabels(labels) + " " + formatNumber((*gauge)->value()) + "\n";
            } else if (const auto* read = std::get_if<std::function<double()>>(&series)) {
                text += name + bracedLabels(labels) + " " + formatNumber((*read)()) + "\n";
            } else {
                const Histogram& histogram = *std::get<std::unique_ptr<Histogram>>(series);
                const std::vector<std::uint64_t> counts = histogram.cumulativeCounts();
                const std::vector<double>& bounds = histogram.upperBounds();
                for (std::size_t i = 0; i < bounds.size(); ++i) {
                    text += name + "_bucket" +
                            joinLabels(labels, "le=\"" + formatNumber(bounds[i]) + "\"") + " " +
                            std::to_string(counts[i]) + "\n";
                }
                text += name + "_bucket" + joinLabels(labels, "le=\"+Inf\"") + " " +
                        std::to_string(counts.back()) + "\n";
                text += name + "_sum" + bracedLabels(labels) + " " + formatNumber(histogram.sum()) +
                        "\n";
                text += name + "_count" + bracedLabels(labels) + " " +
                        std::to_string(counts.back()) + "\n";
            }
        }
    }
    return text;
}

}  // namespace cppboot::actuator
