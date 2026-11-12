# 018: JSON

**Status:** Accepted.

This record explains why CppBoot has its own JSON type, `cppboot::Json` in `cppboot-base`, which replaced nlohmann/json, and how structs convert to and from it.

## Why its own

JSON is in the path of every web request, so it's where a framework most needs to decide what happens with input it didn't expect. A general-purpose library makes those decisions for every kind of program, and a web server needs stricter ones:

- **A key written twice** in one object is accepted by most libraries, which keep the first or the last. Two readers that choose differently, such as a proxy and the application, can disagree about what a request says, which is a known way around checks.
- **Text that isn't valid UTF-8** passes through many libraries, and then breaks whatever reads it next, or is written back out as invalid JSON.
- **Nesting** is unlimited in many libraries, and a recursive reader can use up the stack on `[[[[...`.
- **Its types would appear in public headers**, because `toJson` and route results use them. Every upgrade of the library would then be an upgrade of CppBoot's interface.

Owning the type lets each of these be decided once, for every application, and keeps the interface CppBoot's own. The cost is about 1,000 lines of code, which the unit tests and a fuzz target in CI cover.

## The type

`Json` holds one of null, bool, number, string, array or object.

- **Objects keep their keys in the order they were added**, as a vector of members. Responses then list fields in the order a struct declares them, which is what people reading them expect, and the OpenAPI document reads in the order it was written. Objects in requests and responses are small, so a linear search costs less than a map's allocations.
- **Numbers keep their form.** A number written without a fraction or exponent, that fits in 64 bits, is held as an integer, so IDs and amounts above 2^53 don't lose digits as they would in a `double`. Each value has one form, so `Json::fromUnsigned(5) == Json::fromInteger(5)`.
- **Equality** compares numbers by value, however they were written, and objects by their members in any order.

## Reading is strict

`Json::parse` throws `JsonError`, and `Json::tryParse` returns an error with `ErrorCode::ParseError`, for anything that isn't valid JSON as RFC 8259 defines it, and also for:

| Input | Why it's rejected |
|---|---|
| The same key twice in one object | Two readers could disagree about which one counts |
| Text that isn't valid UTF-8, or a lone surrogate in `\u` escapes | It would break whatever reads the value next |
| Nesting deeper than 128 | It bounds the reader's stack, whatever a client sends |
| A number too large for a `double` | It can't be held without changing its value |
| Leading zeros, `NaN`, comments, trailing commas | They aren't JSON, and accepting them would make another reader disagree |

The body's size is already limited by the server before parsing ([010](010-web-server.md#limits-and-failures)), so the reader's memory is bounded too.

**Writing** always produces valid JSON. Text that isn't valid UTF-8 is written with U+FFFD in place of each bad byte, and NaN and infinity, which JSON can't represent, are written as `null`.

## Converting types

`JsonConvert<T>` converts a type to and from `Json`, with `toJson(Json&, const T&)` and `fromJson(const Json&, T&)`. `json.hpp` has it for `bool`, numbers, `std::string`, `std::optional`, `std::vector` and `std::map<std::string, T>`. A `Json` can be built from any type with a `JsonConvert`, and `json.get<T>()` converts back.

Numbers are checked when they convert: an `int` field rejects `3000000000` and `1.5`, rather than wrapping or cutting it.

### Structs

The generator writes a `JsonConvert` for each struct marked `CPPBOOT_JSON`:

```cpp
template <>
struct cppboot::JsonConvert<Book> {
    template <typename JsonType>
    static void toJson(JsonType& json, const Book& value) {
        json = JsonType::object();
        json["title"] = JsonType{value.title};
        json["price"] = JsonType{value.price};
    }

    template <typename JsonType>
    static void fromJson(const JsonType& json, Book& value) {
        if (!json.isObject()) {
            throw cppboot::JsonError("expected an object");
        }
        cppboot::detail::readJsonField(json, "title", value.title);
        if (!cppboot::validation::length(value.title, 1, 200)) {
            throw cppboot::JsonError("field 'title': must be 1 to 200 characters");
        }
        cppboot::detail::readJsonField(json, "price", value.price);
    }
};
```

- **Field names are the C++ names.** A struct is the contract with clients, so what's in the code is what's on the wire, and the OpenAPI document says the same.
- **Every field is required** unless it's a `std::optional`, which can be missing or `null`. A missing field is an error, not a default, because a client that leaves out a field it meant to send would otherwise store a zero without knowing.
- **Fields a struct doesn't have are ignored**, so a client written for a newer version of an API still works with an older one.
- **Checks run as each field is read**, with the same `cppboot::validation` functions as settings structs ([017](017-configuration.md#settings-structs)), and the message names the field. A route turns a `JsonError` from a body into 400 with that message ([010](010-web-server.md)).
- **The functions are templates**, so a field whose type is converted further down the generated file still works: a template is compiled only where it's used.

### Why generated, not written by hand or with a macro

Writing `toJson` and `fromJson` by hand for each struct is repetitive and easy to get out of step with the struct. A macro such as nlohmann's `NLOHMANN_DEFINE_TYPE_INTRUSIVE` has to list every field again, and can't see checks. The generator already reads each struct's fields with libclang, so it writes the conversion and its checks from the declaration, and the struct is the only place a field is named.

## What was left out

- **Renaming fields,** such as `snake_case` on the wire for `camelCase` in C++. One name everywhere is simpler to read, search and document. A struct whose fields need other names can have its own `JsonConvert`.
- **Streaming large documents.** Requests are bounded and small, so reading a whole body into a `Json` is simple and safe.
- **JSON Pointer, Patch and Schema.** Applications rarely need them, and they can be added on top of `Json` without changing it.
