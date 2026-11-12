#include <gtest/gtest.h>

#include <chrono>
#include <cppboot/base/exceptions.hpp>
#include <cppboot/config/binding.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using cppboot::ErrorCode;
using cppboot::config::Config;
using cppboot::config::SettingsBinder;
using namespace std::chrono_literals;

namespace {

Config makeConfig(std::map<std::string, std::string> settings,
                  std::map<std::string, std::string> environment = {}) {
    cppboot::config::ConfigSources sources;
    sources.defaults = std::move(settings);
    sources.environment = std::move(environment);
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-binding-test-none";
    return Config::load(sources).value();
}

/// What finish() reports, or nothing if the settings were fine.
std::string problems(const SettingsBinder& binder) {
    try {
        binder.finish();
    } catch (const cppboot::Exception& exception) {
        EXPECT_EQ(exception.error().code(), ErrorCode::InvalidConfiguration);
        return exception.error().message();
    }
    return {};
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(SettingsBinderTest, ReadsDurationsInEveryUnit) {
    const Config config = makeConfig({{"t.a", "5ns"}, {"t.b", "5us"}, {"t.c", "5ms"}, {"t.d", "5s"},
                                      {"t.e", "5m"},  {"t.f", "5h"},  {"t.g", "2d"},  {"t.h", "0s"}});
    SettingsBinder binder{config, "t", "T"};
    std::chrono::nanoseconds a{}, b{}, c{}, d{}, e{}, f{}, g{}, h{1};
    binder.bind("a", a);
    binder.bind("b", b);
    binder.bind("c", c);
    binder.bind("d", d);
    binder.bind("e", e);
    binder.bind("f", f);
    binder.bind("g", g);
    binder.bind("h", h);
    EXPECT_EQ(problems(binder), "");
    EXPECT_EQ(a, 5ns);
    EXPECT_EQ(b, 5us);
    EXPECT_EQ(c, 5ms);
    EXPECT_EQ(d, 5s);
    EXPECT_EQ(e, 5min);
    EXPECT_EQ(f, 5h);
    EXPECT_EQ(g, 48h);
    EXPECT_EQ(h, 0s);
}

TEST(SettingsBinderTest, RejectsDurationsItCannotHoldExactly) {
    const Config config = makeConfig({{"t.fine", "1500ms"}, {"t.coarse", "1500ms"},
                                      {"t.none", "15"},     {"t.negative", "-5s"},
                                      {"t.fraction", "1.5s"}, {"t.huge", "99999999999999d"},
                                      {"t.word", "soon"}});
    SettingsBinder binder{config, "t", "T"};
    std::chrono::milliseconds fine{};
    std::chrono::seconds coarse{7}, none{7}, negative{7}, fraction{7}, huge{7}, word{7};
    binder.bind("fine", fine);
    binder.bind("coarse", coarse);
    binder.bind("none", none);
    binder.bind("negative", negative);
    binder.bind("fraction", fraction);
    binder.bind("huge", huge);
    binder.bind("word", word);

    EXPECT_EQ(fine, 1500ms);
    // A value that can't be read leaves the field as it was.
    for (const auto value : {coarse, none, negative, fraction, huge, word}) {
        EXPECT_EQ(value, 7s);
    }
    const std::string report = problems(binder);
    EXPECT_TRUE(contains(report, "t.coarse: must be a time in the unit that this setting counts "
                                 "in, but is '1500ms'"))
        << report;
    for (const char* key : {"t.none", "t.negative", "t.fraction", "t.huge", "t.word"}) {
        EXPECT_TRUE(contains(report, std::string{key} + ": must be a time with its unit")) << key;
    }
}

TEST(SettingsBinderTest, WholeNumbersHaveToFitTheirField) {
    const Config config = makeConfig({{"n.small", "200"}, {"n.negative", "-1"}, {"n.big", "9223372036854775807"},
                                      {"n.most", "18446744073709551615"}, {"n.fraction", "1.5"},
                                      {"n.spaces", " 5"}, {"n.empty", ""}});
    SettingsBinder binder{config, "n", "N"};
    std::int8_t small = 0;
    unsigned negative = 0;
    std::int64_t big = 0;
    std::uint64_t most = 0;
    int fraction = 0;
    int spaces = 0;
    int empty = 0;
    binder.bind("small", small);
    binder.bind("negative", negative);
    binder.bind("big", big);
    binder.bind("most", most);
    binder.bind("fraction", fraction);
    binder.bind("spaces", spaces);
    binder.bind("empty", empty);

    EXPECT_EQ(big, 9223372036854775807LL);
    EXPECT_EQ(most, 18446744073709551615ULL);
    const std::string report = problems(binder);
    EXPECT_TRUE(contains(report, "n.small: must be a whole number from -128 to 127, but is '200'"))
        << report;
    EXPECT_TRUE(contains(report, "n.negative: must be a whole number from 0 to 4294967295"));
    EXPECT_TRUE(contains(report, "n.fraction: must be a whole number"));
    EXPECT_TRUE(contains(report, "n.spaces: must be a whole number"));
    EXPECT_TRUE(contains(report, "n.empty: must be a whole number"));
    EXPECT_FALSE(contains(report, "n.big"));
    EXPECT_FALSE(contains(report, "n.most"));
}

TEST(SettingsBinderTest, ReadsBoolsDecimalsTextAndOptionals) {
    const Config config = makeConfig({{"v.on", "YES"}, {"v.off", "0"}, {"v.rate", "1e-3"},
                                      {"v.text", " kept as written "}, {"v.maybe", "7"},
                                      {"v.bad_rate", "inf"}, {"v.bad_flag", "2"}});
    SettingsBinder binder{config, "v", "V"};
    bool on = false;
    bool off = true;
    double rate = 0;
    std::string text;
    std::optional<int> maybe;
    std::optional<int> absent;
    float bad_rate = 1;
    bool bad_flag = true;
    binder.bind("on", on);
    binder.bind("off", off);
    binder.bind("rate", rate);
    binder.bind("text", text);
    binder.bind("maybe", maybe);
    binder.bind("absent", absent);
    binder.bind("bad_rate", bad_rate);
    binder.bind("bad_flag", bad_flag);

    EXPECT_TRUE(on);
    EXPECT_FALSE(off);
    EXPECT_EQ(rate, 0.001);
    EXPECT_EQ(text, " kept as written ");
    EXPECT_EQ(maybe, 7);
    EXPECT_FALSE(absent);
    const std::string report = problems(binder);
    EXPECT_TRUE(contains(report, "v.bad_rate: must be a number, but is 'inf'")) << report;
    EXPECT_TRUE(contains(report, "v.bad_flag: must be true or false, but is '2'")) << report;
}

TEST(SettingsBinderTest, ReadsListsFromItemsOrFromOneValueWithCommas) {
    const Config config = makeConfig({{"l.items.0", "1"}, {"l.items.1", "2"}, {"l.joined", "3, 4 ,5"},
                                      {"l.one", "6"}, {"l.none", ""}, {"l.bad.0", "1"}, {"l.bad.1", "x"}});
    SettingsBinder binder{config, "l", "L"};
    std::vector<int> items, joined, one, none{9}, absent{9}, bad{9};
    binder.bind("items", items);
    binder.bind("joined", joined);
    binder.bind("one", one);
    binder.bind("none", none);
    binder.bind("absent", absent);
    binder.bind("bad", bad);
    binder.require("items");
    binder.require("joined");
    binder.require("absent");

    EXPECT_EQ(items, (std::vector<int>{1, 2}));
    EXPECT_EQ(joined, (std::vector<int>{3, 4, 5}));
    EXPECT_EQ(one, (std::vector<int>{6}));
    // Set to nothing is an empty list, and not set keeps what was there.
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(absent, (std::vector<int>{9}));
    EXPECT_EQ(bad, (std::vector<int>{9}));

    const std::string report = problems(binder);
    EXPECT_TRUE(contains(report, "l.bad.1: must be a whole number")) << report;
    EXPECT_TRUE(contains(report, "l.absent: must be set")) << report;
    EXPECT_FALSE(contains(report, "l.items:")) << report;
    EXPECT_FALSE(contains(report, "l.joined:")) << report;
}

TEST(SettingsBinderTest, AnEnvironmentVariableCountsAsSet) {
    const Config config = makeConfig({}, {{"CPPBOOT_APP_API_KEY", "k"}});
    SettingsBinder binder{config, "app", "App"};
    std::string api_key;
    std::string other;
    binder.bind("api_key", api_key);
    binder.bind("other", other);
    binder.require("api_key");
    binder.require("other");

    EXPECT_EQ(api_key, "k");
    const std::string report = problems(binder);
    EXPECT_FALSE(contains(report, "app.api_key")) << report;
    EXPECT_TRUE(contains(report, "app.other: must be set, in a settings file or with the "
                                 "environment variable CPPBOOT_APP_OTHER"))
        << report;
}

TEST(SettingsBinderTest, ChecksAreSkippedForASettingThatAlreadyHasAProblem) {
    const Config config = makeConfig({{"c.port", "x"}});
    SettingsBinder binder{config, "c", "C"};
    int port = 0;
    binder.bind("port", port);
    binder.check("port", false, "must be from 1 to 10");
    binder.check("size", false, "must be at least 1");
    binder.check("size", false, "must be even");
    binder.check("size", true, "never shown");

    const std::string report = problems(binder);
    EXPECT_FALSE(contains(report, "c.port: must be from")) << report;
    EXPECT_TRUE(contains(report, "c.size: must be at least 1")) << report;
    EXPECT_FALSE(contains(report, "must be even")) << report;
}

TEST(SettingsBinderTest, FlagsKeysThatNothingReadsAndSuggestsTheClosest) {
    const Config config = makeConfig({{"k.timeout", "1"}, {"k.timout", "2"}, {"k.zzz", "3"},
                                      {"k.list.0", "a"}, {"k.other.inner", "4"}, {"kk.unrelated", "5"},
                                      {"k.timeout.extra", "6"}});
    SettingsBinder binder{config, "k", "K"};
    int timeout = 0;
    std::vector<std::string> list;
    binder.bind("timeout", timeout);
    binder.bind("list", list);
    binder.ignoreGroup("other");

    const std::string report = problems(binder);
    EXPECT_TRUE(contains(report, "k.timout: isn't a setting that K reads. Did you mean k.timeout?"))
        << report;
    EXPECT_TRUE(contains(report, "k.zzz: isn't a setting that K reads\n") ||
                report.ends_with("k.zzz: isn't a setting that K reads"))
        << report;
    // A value can't also have settings inside it.
    EXPECT_TRUE(contains(report, "k.timeout.extra: isn't a setting")) << report;
    for (const char* fine : {"k.list", "k.other", "kk.unrelated", "k.timeout:"}) {
        EXPECT_FALSE(contains(report, fine)) << fine << "\n" << report;
    }
}

TEST(SettingsBinderTest, NothingWrongMeansNoException) {
    const Config config = makeConfig({{"ok.name", "x"}});
    SettingsBinder binder{config, "ok", "Ok"};
    std::string name;
    binder.bind("name", name);
    binder.require("name");
    EXPECT_NO_THROW(binder.finish());
}
