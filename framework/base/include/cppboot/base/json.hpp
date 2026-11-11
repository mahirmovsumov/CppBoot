#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "cppboot/base/annotations.hpp"
#include "cppboot/base/result.hpp"

namespace cppboot {

/// Thrown for JSON that can't be read, or that doesn't have the shape the code expects.
class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json;

/// Converts a type to and from Json. CPPBOOT_JSON structs get a specialization from the generator,
/// and this file has them for numbers, bool, text, std::optional, std::vector and std::map.
template <typename T>
struct JsonConvert;

struct JsonMember;

/// A JSON value. Objects keep their keys in the order they were added.
///
/// Reading JSON is strict: it has to be valid UTF-8, nested at most 128 deep, and an object can't
/// have the same key twice, because two readers could otherwise disagree about which one counts.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::vector<JsonMember>;

    // Defined after JsonMember, because building the variant needs Object's element type complete.
    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(const char* text);
    Json(std::string text);
    Json(std::string_view text);

    /// Any type with a JsonConvert, such as numbers, containers and CPPBOOT_JSON structs.
    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Json> &&
                 requires(Json& json, const std::remove_cvref_t<T>& value) {
                     JsonConvert<std::remove_cvref_t<T>>::toJson(json, value);
                 })
    Json(const T& value) {
        JsonConvert<std::remove_cvref_t<T>>::toJson(*this, value);
    }

    Json(const Json& other);
    Json(Json&& other) noexcept;
    Json& operator=(const Json& other);
    Json& operator=(Json&& other) noexcept;
    ~Json();

    static Json array();
    static Json object();
    static Json fromInteger(std::int64_t value);
    static Json fromUnsigned(std::uint64_t value);
    /// NaN and infinity have no JSON form, and are written as null.
    static Json fromDouble(double value);

    /// Reads JSON text, and throws JsonError if it isn't valid.
    static Json parse(std::string_view text);
    /// Reads JSON text, and returns an error with ErrorCode::ParseError if it isn't valid.
    static Result<Json> tryParse(std::string_view text);

    /// Compact JSON text. Text that isn't valid UTF-8 is written with U+FFFD in place of each bad
    /// byte, so the output is always valid JSON.
    std::string dump() const;

    Type type() const noexcept;
    bool isNull() const noexcept { return type() == Type::Null; }
    bool isBool() const noexcept { return type() == Type::Bool; }
    bool isNumber() const noexcept { return type() == Type::Number; }
    /// A number written without a fraction or exponent, which fits in 64 bits.
    bool isInteger() const noexcept;
    bool isString() const noexcept { return type() == Type::String; }
    bool isArray() const noexcept { return type() == Type::Array; }
    bool isObject() const noexcept { return type() == Type::Object; }

    /// These throw JsonError if the value is another type, or a number out of range.
    bool asBool() const;
    double asDouble() const;
    std::int64_t asInt64() const;
    std::uint64_t asUint64() const;
    const std::string& asString() const CPPBOOT_LIFETIMEBOUND;

    /// An array's items, or an object's members. Throw JsonError for another type.
    const Array& items() const CPPBOOT_LIFETIMEBOUND;
    const Object& members() const CPPBOOT_LIFETIMEBOUND;

    /// The number of items or members, or 0 for any other type.
    std::size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    /// An object's value for a key, or nullptr if it has none or isn't an object.
    const Json* find(std::string_view key) const noexcept CPPBOOT_LIFETIMEBOUND;
    bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

    /// An object's value for a key. Throws JsonError if it's missing, or this isn't an object.
    const Json& at(std::string_view key) const CPPBOOT_LIFETIMEBOUND;
    /// An array's item. Throws JsonError if it's out of range, or this isn't an array.
    const Json& at(std::size_t index) const CPPBOOT_LIFETIMEBOUND;

    /// An object's value for a key, added as null if it's missing. Null becomes an empty object.
    Json& operator[](std::string_view key) CPPBOOT_LIFETIMEBOUND;
    const Json& operator[](std::string_view key) const CPPBOOT_LIFETIMEBOUND { return at(key); }
    const Json& operator[](std::size_t index) const CPPBOOT_LIFETIMEBOUND { return at(index); }

    /// Adds an item to an array. Null becomes an empty array first.
    void push(Json item);

    /// Converts to T, and throws JsonError if the value doesn't fit.
    template <typename T>
    T get() const {
        T value{};
        JsonConvert<T>::fromJson(*this, value);
        return value;
    }

    template <typename T>
    void getTo(T& value) const {
        JsonConvert<T>::fromJson(*this, value);
    }

    /// Numbers are equal if their values are, however they were written. Objects are equal if they
    /// have the same members, in any order.
    friend bool operator==(const Json& left, const Json& right);

private:
    friend class JsonParser;

    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array,
                 Object>
        value_;
};

struct JsonMember {
    std::string key;
    Json value;
};

inline Json::Json() = default;
inline Json::Json(std::nullptr_t) {}
inline Json::Json(bool value) : value_(value) {}
inline Json::Json(const char* text) : value_(std::string{text}) {}
inline Json::Json(std::string text) : value_(std::move(text)) {}
inline Json::Json(std::string_view text) : value_(std::string{text}) {}
inline Json::Json(const Json& other) = default;
inline Json::Json(Json&& other) noexcept = default;
inline Json& Json::operator=(const Json& other) = default;
inline Json& Json::operator=(Json&& other) noexcept = default;
inline Json::~Json() = default;

namespace detail {

/// Reads a CPPBOOT_JSON struct's field, naming it in any error. Generated code calls these.
template <typename T>
void readJsonField(const Json& object, std::string_view key, T& value) {
    const Json* const field = object.find(key);
    if (field == nullptr) {
        throw JsonError(object.isObject() ? "missing field '" + std::string{key} + "'"
                                          : "expected an object");
    }
    try {
        field->getTo(value);
    } catch (const JsonError& error) {
        throw JsonError("field '" + std::string{key} + "': " + error.what());
    }
}

/// The same for a std::optional field, which can be missing.
template <typename T>
void readOptionalJsonField(const Json& object, std::string_view key, std::optional<T>& value) {
    if (!object.isObject()) {
        throw JsonError("expected an object");
    }
    if (object.contains(key)) {
        readJsonField(object, key, value);
    }
}

}  // namespace detail

template <>
struct JsonConvert<Json> {
    static void toJson(Json& json, const Json& value) { json = value; }
    static void fromJson(const Json& json, Json& value) { value = json; }
};

template <>
struct JsonConvert<bool> {
    static void toJson(Json& json, bool value) { json = Json{value}; }
    static void fromJson(const Json& json, bool& value) { value = json.asBool(); }
};

template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
struct JsonConvert<T> {
    static void toJson(Json& json, T value) {
        if constexpr (std::is_signed_v<T>) {
            json = Json::fromInteger(value);
        } else {
            json = Json::fromUnsigned(value);
        }
    }

    static void fromJson(const Json& json, T& value) {
        if constexpr (std::is_signed_v<T>) {
            const std::int64_t number = json.asInt64();
            if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
                throw JsonError("number out of range");
            }
            value = static_cast<T>(number);
        } else {
            const std::uint64_t number = json.asUint64();
            if (number > std::numeric_limits<T>::max()) {
                throw JsonError("number out of range");
            }
            value = static_cast<T>(number);
        }
    }
};

template <typename T>
    requires std::is_floating_point_v<T>
struct JsonConvert<T> {
    static void toJson(Json& json, T value) { json = Json::fromDouble(static_cast<double>(value)); }
    static void fromJson(const Json& json, T& value) { value = static_cast<T>(json.asDouble()); }
};

template <>
struct JsonConvert<std::string> {
    static void toJson(Json& json, const std::string& value) { json = Json{value}; }
    static void fromJson(const Json& json, std::string& value) { value = json.asString(); }
};

template <typename T>
struct JsonConvert<std::optional<T>> {
    static void toJson(Json& json, const std::optional<T>& value) {
        json = value ? Json{*value} : Json{};
    }
    static void fromJson(const Json& json, std::optional<T>& value) {
        if (json.isNull()) {
            value.reset();
        } else {
            value = json.get<T>();
        }
    }
};

template <typename T>
struct JsonConvert<std::vector<T>> {
    static void toJson(Json& json, const std::vector<T>& value) {
        json = Json::array();
        for (const T& item : value) {
            json.push(Json{item});
        }
    }
    static void fromJson(const Json& json, std::vector<T>& value) {
        value.clear();
        std::size_t index = 0;
        for (const Json& item : json.items()) {
            try {
                value.push_back(item.get<T>());
            } catch (const JsonError& error) {
                throw JsonError("item " + std::to_string(index) + ": " + error.what());
            }
            ++index;
        }
    }
};

template <typename T>
struct JsonConvert<std::map<std::string, T>> {
    static void toJson(Json& json, const std::map<std::string, T>& value) {
        json = Json::object();
        for (const auto& [key, item] : value) {
            json[key] = Json{item};
        }
    }
    static void fromJson(const Json& json, std::map<std::string, T>& value) {
        value.clear();
        for (const JsonMember& member : json.members()) {
            detail::readJsonField(json, member.key, value[member.key]);
        }
    }
};

}  // namespace cppboot
