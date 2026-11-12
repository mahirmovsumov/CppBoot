#include "cppboot/codegen/parser.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "cppboot/platform/process.hpp"

namespace cppboot::codegen {

namespace {

constexpr std::string_view kServiceAnnotation = "cppboot:service";
constexpr std::string_view kJsonAnnotation = "cppboot:json";
constexpr std::string_view kRowAnnotation = "cppboot:row";
constexpr std::string_view kControllerPrefix = "cppboot:controller:";
constexpr std::string_view kRoutePrefix = "cppboot:route:";
constexpr std::string_view kMiddlewarePrefix = "cppboot:middleware:";
constexpr std::string_view kPublicAnnotation = "cppboot:public";
constexpr std::string_view kRolePrefix = "cppboot:role:";
constexpr std::string_view kRateLimitPrefix = "cppboot:rate_limit:";
constexpr std::string_view kUncheckedPrefix = "cppboot:unchecked:";
constexpr std::string_view kConfigPrefix = "cppboot:config:";
constexpr std::string_view kRequiredAnnotation = "cppboot:required";
constexpr std::string_view kNotEmptyAnnotation = "cppboot:not_empty";
constexpr std::string_view kLengthPrefix = "cppboot:length:";
constexpr std::string_view kMinPrefix = "cppboot:min:";
constexpr std::string_view kMaxPrefix = "cppboot:max:";
constexpr std::string_view kRangePrefix = "cppboot:range:";
constexpr std::string_view kAnyPrefix = "cppboot:";

/// Nested settings structs deeper than this are almost certainly a mistake.
constexpr int kMaxConfigDepth = 8;

std::string toString(CXString text) {
    const char* const characters = clang_getCString(text);
    std::string result = characters == nullptr ? "" : characters;
    clang_disposeString(text);
    return result;
}

std::string spelling(CXCursor cursor) {
    return toString(clang_getCursorSpelling(cursor));
}

std::string qualifiedName(CXType type) {
    return toString(clang_getTypeSpelling(clang_getCanonicalType(type)));
}

std::string locationOf(CXCursor cursor) {
    CXFile file = nullptr;
    unsigned line = 0;
    clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, &line, nullptr, nullptr);
    return toString(clang_getFileName(file)) + ":" + std::to_string(line);
}

Error errorAt(CXCursor cursor, const std::string& message) {
    return Error{ErrorCode::ParseError, locationOf(cursor) + ": " + message};
}

template <typename Visitor>
void visitChildren(CXCursor parent, Visitor&& visitor) {
    using VisitorType = std::remove_reference_t<Visitor>;
    clang_visitChildren(
        parent,
        [](CXCursor cursor, CXCursor, CXClientData data) {
            return (*static_cast<VisitorType*>(data))(cursor);
        },
        &visitor);
}

std::vector<std::string> annotationsOf(CXCursor cursor) {
    std::vector<std::string> annotations;
    visitChildren(cursor, [&](CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_AnnotateAttr) {
            std::string text = spelling(child);
            if (text.starts_with(kAnyPrefix)) {
                annotations.push_back(std::move(text));
            }
        }
        return CXChildVisit_Continue;
    });
    return annotations;
}

bool hasAnnotation(const std::vector<std::string>& annotations, std::string_view wanted) {
    return std::find(annotations.begin(), annotations.end(), wanted) != annotations.end();
}

std::optional<std::string> findWithPrefix(const std::vector<std::string>& annotations,
                                          std::string_view prefix) {
    for (const std::string& annotation : annotations) {
        if (annotation.starts_with(prefix)) {
            return annotation.substr(prefix.size());
        }
    }
    return std::nullopt;
}

std::vector<std::string> allWithPrefix(const std::vector<std::string>& annotations,
                                       std::string_view prefix) {
    std::vector<std::string> values;
    for (const std::string& annotation : annotations) {
        if (annotation.starts_with(prefix)) {
            values.push_back(annotation.substr(prefix.size()));
        }
    }
    return values;
}

bool hasSecurityMark(const std::vector<std::string>& annotations) {
    return hasAnnotation(annotations, kPublicAnnotation) ||
           findWithPrefix(annotations, kRolePrefix).has_value();
}

CXType withoutReference(CXType parameter_type) {
    CXType type = clang_getCanonicalType(parameter_type);
    if (type.kind == CXType_LValueReference || type.kind == CXType_RValueReference) {
        type = clang_getCanonicalType(clang_getPointeeType(type));
    }
    return type;
}

/// Returns true for a type with this name in the standard library. Standard libraries put types
/// in inline namespaces, such as std::__cxx11::basic_string, so those are skipped.
bool isStandardType(CXType type, std::string_view name) {
    const CXCursor declaration = clang_getTypeDeclaration(type);
    if (spelling(declaration) != name) {
        return false;
    }

    CXCursor parent = clang_getCursorSemanticParent(declaration);
    while (clang_getCursorKind(parent) == CXCursor_Namespace &&
           clang_Cursor_isInlineNamespace(parent) != 0) {
        parent = clang_getCursorSemanticParent(parent);
    }
    return clang_getCursorKind(parent) == CXCursor_Namespace && spelling(parent) == "std";
}

std::string withoutConst(std::string type) {
    constexpr std::string_view kConst = "const ";
    if (type.starts_with(kConst)) {
        type.erase(0, kConst.size());
    }
    return type;
}

/// Returns T if the type is std::shared_ptr<T>, taken by value or by reference.
std::optional<std::string> sharedPointerTarget(CXType parameter_type) {
    const CXType type = withoutReference(parameter_type);
    if (!isStandardType(type, "shared_ptr") || clang_Type_getNumTemplateArguments(type) != 1) {
        return std::nullopt;
    }
    return withoutConst(qualifiedName(clang_Type_getTemplateArgumentAsType(type, 0)));
}

/// Returns T if the type is T& or const T&, for a class. Copies and raw pointers are left out: a copy
/// wouldn't be the shared component, and a pointer suggests that it might be missing.
std::optional<std::string> referenceTarget(CXType parameter_type) {
    const CXType type = clang_getCanonicalType(parameter_type);
    if (type.kind != CXType_LValueReference) {
        return std::nullopt;
    }
    const CXType target = clang_getCanonicalType(clang_getPointeeType(type));
    if (target.kind != CXType_Record) {
        return std::nullopt;
    }
    return withoutConst(qualifiedName(target));
}

bool isOptionalType(CXType type) {
    return isStandardType(type, "optional");
}

bool isAnyStandardType(CXType type, std::initializer_list<std::string_view> names) {
    return std::any_of(names.begin(), names.end(),
                       [&](std::string_view name) { return isStandardType(type, name); });
}

bool isView(CXType type) {
    return isAnyStandardType(type, {"basic_string_view", "span"});
}

bool isLock(CXType type) {
    return isAnyStandardType(type, {"mutex", "recursive_mutex", "timed_mutex",
                                    "recursive_timed_mutex", "shared_mutex", "shared_timed_mutex"});
}

/// Returns true for a value that a thread can't safely change while another uses it. Atomics aren't
/// here on purpose, and neither are other classes, which may protect themselves.
bool isUnprotectedValue(CXType type) {
    switch (type.kind) {
        case CXType_Bool:
        case CXType_Char_S:
        case CXType_Char_U:
        case CXType_SChar:
        case CXType_UChar:
        case CXType_Short:
        case CXType_UShort:
        case CXType_Int:
        case CXType_UInt:
        case CXType_Long:
        case CXType_ULong:
        case CXType_LongLong:
        case CXType_ULongLong:
        case CXType_Float:
        case CXType_Double:
            return true;
        default:
            break;
    }
    return isAnyStandardType(type, {"basic_string", "vector", "map", "unordered_map", "multimap",
                                    "unordered_multimap", "set", "unordered_set", "multiset",
                                    "unordered_multiset", "list", "deque", "array", "optional"});
}

/// Returns true for a request or something taken from one, which ends with its request.
bool isRequestData(const std::string& type_name) {
    return type_name == "cppboot::web::Request" || type_name == "cppboot::web::Response" ||
           type_name == "cppboot::security::Principal";
}

std::string warningAt(CXCursor cursor, const std::string& message) {
    return locationOf(cursor) + ": warning: " + message;
}

/// A component lives as long as the application, and every thread shares it. This finds fields that
/// can't be safe with that, as far as it can see from declarations. It doesn't read function bodies,
/// so finding nothing doesn't prove that a component is safe.
std::optional<Error> checkComponentFields(CXCursor cursor, const Component& component,
                                          std::vector<std::string>& warnings) {
    const bool is_module =
        std::find(component.interfaces.begin(), component.interfaces.end(), kModuleInterface) !=
        component.interfaces.end();

    bool has_lock = false;
    bool can_change = false;
    std::vector<CXCursor> unprotected;
    std::optional<Error> error;

    visitChildren(cursor, [&](CXCursor child) {
        const CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXMethod) {
            const std::string method = spelling(child);
            const bool is_lifecycle = is_module && (method == "start" || method == "stop");
            if (clang_getCXXAccessSpecifier(child) == CX_CXXPublic &&
                clang_CXXMethod_isConst(child) == 0 && clang_CXXMethod_isStatic(child) == 0 &&
                clang_CXXMethod_isDeleted(child) == 0 && !method.starts_with("operator") &&
                !is_lifecycle) {
                can_change = true;
            }
            return CXChildVisit_Continue;
        }
        if (kind != CXCursor_FieldDecl) {
            return CXChildVisit_Continue;
        }

        const CXType field_type = clang_getCanonicalType(clang_getCursorType(child));
        const std::string field = spelling(child);
        const auto reason = findWithPrefix(annotationsOf(child), kUncheckedPrefix);
        if (reason && reason->empty()) {
            error = errorAt(child, "CPPBOOT_UNCHECKED on field '" + field + "' needs a reason");
            return CXChildVisit_Break;
        }

        if (isLock(field_type)) {
            has_lock = true;
        }
        if (clang_CXXField_isMutable(child) != 0) {
            can_change = true;
        }
        if (reason) {
            return CXChildVisit_Continue;
        }

        const bool is_borrowed = field_type.kind == CXType_LValueReference ||
                                 field_type.kind == CXType_RValueReference ||
                                 field_type.kind == CXType_Pointer;
        const CXType target =
            is_borrowed ? clang_getCanonicalType(clang_getPointeeType(field_type)) : field_type;

        if (is_borrowed && isRequestData(withoutConst(qualifiedName(target)))) {
            error = errorAt(child, "Field '" + field + "' of " + component.type + " points at " +
                                       withoutConst(qualifiedName(target)) +
                                       ", which ends with its request, while " + component.type +
                                       " lives as long as the application. Copy what you need "
                                       "instead, or explain why it's safe with CPPBOOT_UNCHECKED");
            return CXChildVisit_Break;
        }
        if (isView(field_type)) {
            warnings.push_back(warningAt(
                child, "Field '" + field + "' of " + component.type +
                           " doesn't own its data, and may outlive it. Store a copy, or explain "
                           "why it's safe with CPPBOOT_UNCHECKED"));
        }
        if (!is_borrowed && field_type.kind == CXType_Record &&
            std::find(component.dependencies.begin(), component.dependencies.end(),
                      withoutConst(qualifiedName(field_type))) != component.dependencies.end()) {
            warnings.push_back(warningAt(
                child, "Field '" + field + "' of " + component.type + " is a copy of " +
                           withoutConst(qualifiedName(field_type)) +
                           ", not the shared component. Make it a reference"));
        }
        if (clang_isConstQualifiedType(field_type) == 0 && isUnprotectedValue(field_type)) {
            unprotected.push_back(child);
        }
        return CXChildVisit_Continue;
    });

    if (error) {
        return error;
    }
    if (can_change && !has_lock) {
        for (const CXCursor field : unprotected) {
            warnings.push_back(warningAt(
                field, "Field '" + spelling(field) + "' of " + component.type +
                           " can change, with no lock in the class, but every thread shares a "
                           "component. Protect it with a std::mutex, or explain why it's safe "
                           "with CPPBOOT_UNCHECKED"));
        }
    }
    return std::nullopt;
}

/// Returns true for types that can be read from text in a URL.
bool isUrlValue(CXType type) {
    switch (type.kind) {
        case CXType_Bool:
        case CXType_Char_S:
        case CXType_Char_U:
        case CXType_SChar:
        case CXType_UChar:
        case CXType_Short:
        case CXType_UShort:
        case CXType_Int:
        case CXType_UInt:
        case CXType_Long:
        case CXType_ULong:
        case CXType_LongLong:
        case CXType_ULongLong:
        case CXType_Float:
        case CXType_Double:
            return true;
        default:
            break;
    }
    if (isOptionalType(type) && clang_Type_getNumTemplateArguments(type) >= 1) {
        return isUrlValue(clang_getCanonicalType(clang_Type_getTemplateArgumentAsType(type, 0)));
    }
    return isStandardType(type, "basic_string");
}

bool isWholeNumber(CXType type) {
    switch (type.kind) {
        // Plain char is left out, because it's a character. signed char and unsigned char are
        // what int8_t and uint8_t are.
        case CXType_SChar:
        case CXType_UChar:
        case CXType_Short:
        case CXType_UShort:
        case CXType_Int:
        case CXType_UInt:
        case CXType_Long:
        case CXType_ULong:
        case CXType_LongLong:
        case CXType_ULongLong:
            return true;
        default:
            return false;
    }
}

bool isNumber(CXType type) {
    return isWholeNumber(type) || type.kind == CXType_Float || type.kind == CXType_Double;
}

/// The value inside a std::optional, or the type itself.
CXType withoutOptional(CXType type) {
    if (isOptionalType(type) && clang_Type_getNumTemplateArguments(type) >= 1) {
        return clang_getCanonicalType(clang_Type_getTemplateArgumentAsType(type, 0));
    }
    return type;
}

/// What kind of value the checks would apply to, or nothing if the type can't be checked.
std::optional<ValueShape> shapeOf(CXType type) {
    const CXType value = withoutOptional(type);
    if (isNumber(value)) {
        return ValueShape::Number;
    }
    if (isStandardType(value, "basic_string")) {
        return ValueShape::Text;
    }
    if (isAnyStandardType(value, {"vector", "list", "deque", "array", "set", "unordered_set",
                                  "multiset", "unordered_multiset", "map", "unordered_map",
                                  "multimap", "unordered_multimap"})) {
        return ValueShape::Items;
    }
    return std::nullopt;
}

bool hasValidationMark(const std::vector<std::string>& marks) {
    return hasAnnotation(marks, kNotEmptyAnnotation) || findWithPrefix(marks, kLengthPrefix) ||
           findWithPrefix(marks, kMinPrefix) || findWithPrefix(marks, kMaxPrefix) ||
           findWithPrefix(marks, kRangePrefix);
}

/// A number as it can be written in a mark: -12, 0.5 or 1e3.
bool isNumberLiteral(std::string_view text) {
    std::size_t position = 0;
    const auto digits = [&] {
        const std::size_t start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        return position > start;
    };

    if (position < text.size() && text[position] == '-') {
        ++position;
    }
    if (!digits()) {
        return false;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        if (!digits()) {
            return false;
        }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        if (!digits()) {
            return false;
        }
    }
    return position == text.size();
}

/// A limit that generated code can use as written. A whole number has to fit in 64 bits, because
/// a larger one isn't a valid literal.
bool isUsableLimit(std::string_view text, bool whole_only) {
    if (!isNumberLiteral(text)) {
        return false;
    }
    if (text.find_first_of(".eE") != std::string_view::npos) {
        return !whole_only;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && (!whole_only || value >= 0);
}

long double limitValue(const std::string& text) {
    return std::strtold(text.c_str(), nullptr);
}

/// Splits "1:100" from CPPBOOT_RANGE(1, 100) or CPPBOOT_LENGTH(1, 100).
std::pair<std::string, std::string> splitLimits(const std::string& text) {
    const auto colon = text.find(':');
    return {text.substr(0, colon), colon == std::string::npos ? "" : text.substr(colon + 1)};
}

/// Reads the checks marked on a field or a parameter. `what` names it for messages, such as
/// "Field 'age' of User". A check that can't apply to the type fails the build, because one that
/// did nothing would look like protection that isn't there.
Result<std::vector<Check>> readChecks(CXCursor cursor, const std::vector<std::string>& marks,
                                      CXType type, const std::string& what) {
    std::vector<Check> checks;
    if (!hasValidationMark(marks)) {
        return checks;
    }

    const std::optional<ValueShape> shape = shapeOf(clang_getCanonicalType(type));
    if (!shape) {
        return errorAt(cursor, what + " can't be checked. Checks work on numbers, text and "
                                      "containers, and on a std::optional of those");
    }
    const bool is_number = *shape == ValueShape::Number;

    const std::vector<std::string> lengths = allWithPrefix(marks, kLengthPrefix);
    const std::vector<std::string> minimums = allWithPrefix(marks, kMinPrefix);
    const std::vector<std::string> maximums = allWithPrefix(marks, kMaxPrefix);
    const std::vector<std::string> ranges = allWithPrefix(marks, kRangePrefix);
    if (lengths.size() > 1 || minimums.size() > 1 || maximums.size() > 1 || ranges.size() > 1) {
        return errorAt(cursor, what + " has the same check more than once");
    }
    if (!ranges.empty() && (!minimums.empty() || !maximums.empty())) {
        return errorAt(cursor, what + " has CPPBOOT_RANGE as well as CPPBOOT_MIN or CPPBOOT_MAX. "
                                      "Use one or the other");
    }

    if (hasAnnotation(marks, kNotEmptyAnnotation)) {
        if (is_number) {
            return errorAt(cursor, what + " is a number, so CPPBOOT_NOT_EMPTY doesn't apply. Use "
                                          "CPPBOOT_MIN or CPPBOOT_RANGE");
        }
        checks.push_back(Check{CheckKind::NotEmpty, *shape, "", ""});
    }

    if (!lengths.empty()) {
        if (is_number) {
            return errorAt(cursor, what + " is a number, so CPPBOOT_LENGTH doesn't apply. Use "
                                          "CPPBOOT_RANGE");
        }
        auto [min, max] = splitLimits(lengths.front());
        if (!isUsableLimit(min, true) || !isUsableLimit(max, true) ||
            limitValue(min) > limitValue(max)) {
            return errorAt(cursor, "CPPBOOT_LENGTH on " + what + " needs two whole numbers written "
                                   "out, the smaller first, such as CPPBOOT_LENGTH(1, 50)");
        }
        checks.push_back(Check{CheckKind::Length, *shape, std::move(min), std::move(max)});
    }

    const auto number_check = [&](const char* macro, CheckKind kind, std::string min,
                                  std::string max) -> std::optional<Error> {
        if (!is_number) {
            return errorAt(cursor, what + " isn't a number, so " + macro + " doesn't apply. Use "
                                          "CPPBOOT_LENGTH or CPPBOOT_NOT_EMPTY");
        }
        const bool min_ok = kind == CheckKind::Max || isUsableLimit(min, false);
        const bool max_ok = kind == CheckKind::Min || isUsableLimit(max, false);
        if (!min_ok || !max_ok ||
            (kind == CheckKind::Range && limitValue(min) > limitValue(max))) {
            return errorAt(cursor, std::string{macro} + " on " + what +
                                       " needs numbers written out, the smaller first, such as "
                                       "CPPBOOT_RANGE(1, 100)");
        }
        checks.push_back(Check{kind, ValueShape::Number, std::move(min), std::move(max)});
        return std::nullopt;
    };

    if (!minimums.empty()) {
        if (auto error = number_check("CPPBOOT_MIN", CheckKind::Min, minimums.front(), "")) {
            return *std::move(error);
        }
    }
    if (!maximums.empty()) {
        if (auto error = number_check("CPPBOOT_MAX", CheckKind::Max, "", maximums.front())) {
            return *std::move(error);
        }
    }
    if (!ranges.empty()) {
        auto [min, max] = splitLimits(ranges.front());
        if (auto error = number_check("CPPBOOT_RANGE", CheckKind::Range, std::move(min),
                                      std::move(max))) {
            return *std::move(error);
        }
    }
    return checks;
}

std::vector<std::string> pathParameterNames(std::string_view path) {
    std::vector<std::string> names;
    std::size_t position = 0;
    while (true) {
        const auto open = path.find('{', position);
        const auto close = path.find('}', open);
        if (open == std::string_view::npos || close == std::string_view::npos) {
            return names;
        }
        names.emplace_back(path.substr(open + 1, close - open - 1));
        position = close + 1;
    }
}

std::string joinPaths(std::string_view base, std::string_view path) {
    while (!base.empty() && base.back() == '/') {
        base.remove_suffix(1);
    }
    if (path == "/") {
        return base.empty() ? "/" : std::string{base};
    }
    return std::string{base} + std::string{path};
}

Result<Component> readComponent(CXCursor cursor) {
    Component component;
    component.type = qualifiedName(clang_getCursorType(cursor));
    component.location = locationOf(cursor);

    if (component.type.find("(anonymous") != std::string::npos) {
        return errorAt(cursor, component.type +
                                   " can't be a component because generated code can't name it");
    }
    if (clang_CXXRecord_isAbstract(cursor) != 0) {
        return errorAt(cursor, component.type +
                                   " can't be a component because it is abstract. Mark the class "
                                   "that implements it instead");
    }

    std::vector<CXCursor> constructors;
    bool has_declared_constructor = false;
    visitChildren(cursor, [&](CXCursor child) {
        const CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXBaseSpecifier &&
            clang_getCXXAccessSpecifier(child) == CX_CXXPublic) {
            const CXType base = clang_getCursorType(child);
            const CXCursor declaration = clang_getTypeDeclaration(clang_getCanonicalType(base));
            if (clang_CXXRecord_isAbstract(declaration) != 0) {
                component.interfaces.push_back(qualifiedName(base));
            }
        } else if (kind == CXCursor_Constructor &&
                   clang_CXXConstructor_isCopyConstructor(child) == 0 &&
                   clang_CXXConstructor_isMoveConstructor(child) == 0) {
            has_declared_constructor = true;
            if (clang_getCXXAccessSpecifier(child) == CX_CXXPublic &&
                clang_CXXMethod_isDeleted(child) == 0) {
                constructors.push_back(child);
            }
        }
        return CXChildVisit_Continue;
    });

    if (constructors.size() > 1) {
        return errorAt(cursor, component.type +
                                   " has more than one public constructor, so the generator can't "
                                   "tell which one to use");
    }
    if (constructors.empty()) {
        if (has_declared_constructor) {
            return errorAt(cursor, component.type + " has no public constructor");
        }
        return component;
    }

    const CXCursor constructor = constructors.front();
    const int count = clang_Cursor_getNumArguments(constructor);
    for (int i = 0; i < count; ++i) {
        const CXCursor parameter = clang_Cursor_getArgument(constructor, static_cast<unsigned>(i));
        auto target = sharedPointerTarget(clang_getCursorType(parameter));
        if (!target) {
            target = referenceTarget(clang_getCursorType(parameter));
        }
        if (!target) {
            return errorAt(parameter, "Constructor parameter '" + spelling(parameter) + "' of " +
                                          component.type +
                                          " must be a reference or a std::shared_ptr to a component");
        }
        component.dependencies.push_back(*std::move(target));
    }
    return component;
}

/// With `checked`, the checks marked on fields are read. Otherwise they're an error, because nothing
/// would run them.
Result<MappedStruct> readMappedStruct(CXCursor cursor, std::string_view macro, bool checked) {
    const std::string macro_name{macro};

    MappedStruct mapped;
    mapped.type = qualifiedName(clang_getCursorType(cursor));
    mapped.location = locationOf(cursor);

    if (mapped.type.find("(anonymous") != std::string::npos) {
        return errorAt(cursor, mapped.type + " can't be marked with " + macro_name +
                                   " because generated code can't name it");
    }

    std::optional<Error> error;
    visitChildren(cursor, [&](CXCursor child) {
        const CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXBaseSpecifier) {
            error = errorAt(child, mapped.type + " can't have base classes, because " + macro_name +
                                       " only converts its own fields");
        } else if (kind == CXCursor_FieldDecl) {
            if (clang_getCXXAccessSpecifier(child) != CX_CXXPublic) {
                error = errorAt(child, "Field '" + spelling(child) + "' of " + mapped.type +
                                           " must be public for " + macro_name);
            } else {
                const std::string field = "Field '" + spelling(child) + "' of " + mapped.type;
                const std::vector<std::string> marks = annotationsOf(child);
                const CXType type = clang_getCanonicalType(clang_getCursorType(child));

                if (hasAnnotation(marks, kRequiredAnnotation)) {
                    error = errorAt(child, field + " has CPPBOOT_REQUIRED, which is for settings. "
                                                   "A field read from JSON is already required "
                                                   "unless it's a std::optional");
                } else if (!checked && hasValidationMark(marks)) {
                    error = errorAt(child, field + " has a check, but checks only run when a "
                                                   "struct is read from JSON or from settings. "
                                                   "Mark the struct with CPPBOOT_JSON too, or "
                                                   "remove the check");
                } else if (auto checks = readChecks(child, marks, type, field); !checks) {
                    error = checks.error();
                } else {
                    mapped.fields.push_back(StructField{spelling(child), isOptionalType(type),
                                                        std::move(checks).value()});
                }
            }
        }
        return error ? CXChildVisit_Break : CXChildVisit_Continue;
    });

    if (error) {
        return *std::move(error);
    }
    return mapped;
}

bool isChronoDuration(CXType type) {
    const CXCursor declaration = clang_getTypeDeclaration(type);
    if (spelling(declaration) != "duration") {
        return false;
    }
    CXCursor parent = clang_getCursorSemanticParent(declaration);
    while (clang_getCursorKind(parent) == CXCursor_Namespace &&
           clang_Cursor_isInlineNamespace(parent) != 0) {
        parent = clang_getCursorSemanticParent(parent);
    }
    if (clang_getCursorKind(parent) != CXCursor_Namespace || spelling(parent) != "chrono") {
        return false;
    }
    parent = clang_getCursorSemanticParent(parent);
    while (clang_getCursorKind(parent) == CXCursor_Namespace &&
           clang_Cursor_isInlineNamespace(parent) != 0) {
        parent = clang_getCursorSemanticParent(parent);
    }
    return clang_getCursorKind(parent) == CXCursor_Namespace && spelling(parent) == "std";
}

/// Whether a declaration is somewhere inside namespace std.
bool isInStandardLibrary(CXCursor declaration) {
    for (CXCursor parent = clang_getCursorSemanticParent(declaration);
         clang_getCursorKind(parent) != CXCursor_TranslationUnit &&
         clang_isInvalid(clang_getCursorKind(parent)) == 0;
         parent = clang_getCursorSemanticParent(parent)) {
        if (clang_getCursorKind(parent) == CXCursor_Namespace && spelling(parent) == "std" &&
            clang_getCursorKind(clang_getCursorSemanticParent(parent)) ==
                CXCursor_TranslationUnit) {
            return true;
        }
    }
    return false;
}

/// One value that a setting's text can be read as.
bool isSettingScalar(CXType type) {
    return type.kind == CXType_Bool || isNumber(type) || isStandardType(type, "basic_string") ||
           isChronoDuration(type);
}

/// A scalar, or a std::optional or std::vector of one.
bool isSettingValue(CXType type) {
    if (isSettingScalar(type)) {
        return true;
    }
    if ((isOptionalType(type) || isStandardType(type, "vector")) &&
        clang_Type_getNumTemplateArguments(type) >= 1) {
        return isSettingScalar(
            clang_getCanonicalType(clang_Type_getTemplateArgumentAsType(type, 0)));
    }
    return false;
}

/// Reads the fields of a settings struct, going into fields that are structs themselves, whose
/// fields get keys such as "retry.attempts".
std::optional<Error> readConfigFields(CXCursor cursor, const std::string& struct_type,
                                      const std::string& path_prefix, int depth,
                                      std::vector<ConfigField>& fields) {
    std::optional<Error> error;
    visitChildren(cursor, [&](CXCursor child) {
        const CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXBaseSpecifier) {
            error = errorAt(child, struct_type + " can't have base classes, because CPPBOOT_CONFIG "
                                                 "only reads its own fields");
            return CXChildVisit_Break;
        }
        if (kind != CXCursor_FieldDecl) {
            return CXChildVisit_Continue;
        }

        const std::string name = spelling(child);
        const std::string field = "Field '" + name + "' of " + struct_type;
        const std::string path = path_prefix + name;
        const std::vector<std::string> marks = annotationsOf(child);
        const CXType type = clang_getCanonicalType(clang_getCursorType(child));
        const bool required = hasAnnotation(marks, kRequiredAnnotation);

        if (clang_getCXXAccessSpecifier(child) != CX_CXXPublic) {
            error = errorAt(child, field + " must be public for CPPBOOT_CONFIG");
        } else if (clang_isConstQualifiedType(type) != 0) {
            error = errorAt(child, field + " can't be const, because it's set when the "
                                           "application starts");
        } else if (isSettingValue(type)) {
            if (required && isOptionalType(type)) {
                error = errorAt(child, field + " is a std::optional, which says it may be "
                                               "missing, so it can't also be CPPBOOT_REQUIRED");
            } else if (auto checks = readChecks(child, marks, type, field); !checks) {
                error = checks.error();
            } else {
                fields.push_back(ConfigField{path, required, std::move(checks).value()});
            }
        } else if (const CXCursor nested = clang_getTypeDeclaration(type);
                   type.kind == CXType_Record && !isInStandardLibrary(nested) &&
                   clang_isCursorDefinition(nested) != 0) {
            const std::string nested_type = qualifiedName(type);
            if (required || hasValidationMark(marks)) {
                error = errorAt(child, field + " is a struct, so put the marks on the fields of " +
                                           nested_type + " instead");
            } else if (depth >= kMaxConfigDepth) {
                error = errorAt(child, field + " is nested too deeply for settings");
            } else {
                error = readConfigFields(nested, nested_type, path + ".", depth + 1, fields);
            }
        } else {
            error = errorAt(child, field + " can't be read from settings. Use a bool, a number, a "
                                           "std::string, a std::chrono duration, a std::optional "
                                           "or std::vector of those, or a struct of them");
        }
        return error ? CXChildVisit_Break : CXChildVisit_Continue;
    });
    return error;
}

/// A prefix is one or more names with dots between them, such as "mail" or "shop.mail".
bool isValidConfigPrefix(std::string_view prefix) {
    bool segment_started = false;
    for (const char c : prefix) {
        if (c == '.') {
            if (!segment_started) {
                return false;
            }
            segment_started = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            segment_started = true;
        } else {
            return false;
        }
    }
    return segment_started;
}

Result<ConfigStruct> readConfigStruct(CXCursor cursor, std::string_view prefix) {
    ConfigStruct settings;
    settings.type = qualifiedName(clang_getCursorType(cursor));
    settings.location = locationOf(cursor);
    settings.prefix = std::string{prefix};

    if (settings.type.find("(anonymous") != std::string::npos) {
        return errorAt(cursor, settings.type + " can't be marked with CPPBOOT_CONFIG because "
                                               "generated code can't name it");
    }
    if (!isValidConfigPrefix(prefix)) {
        return errorAt(cursor, "The prefix of " + settings.type + " must be lower-case names with "
                                   "dots between them, such as \"mail\" or \"shop.mail\"");
    }
    if (prefix == "cppboot" || prefix.starts_with("cppboot.")) {
        return errorAt(cursor, "The prefix of " + settings.type + " can't start with \"cppboot\", "
                                   "which is for CppBoot's own settings");
    }

    if (auto error = readConfigFields(cursor, settings.type, "", 0, settings.fields)) {
        return *std::move(error);
    }
    if (settings.fields.empty()) {
        return errorAt(cursor, settings.type + " has no fields to read settings into");
    }
    return settings;
}

Result<std::vector<RouteParameter>> readRouteParameters(CXCursor method, const std::string& path) {
    const std::vector<std::string> path_names = pathParameterNames(path);
    std::vector<std::string> unbound = path_names;
    std::vector<RouteParameter> parameters;
    bool has_body = false;

    const int count = clang_Cursor_getNumArguments(method);
    for (int i = 0; i < count; ++i) {
        const CXCursor parameter = clang_Cursor_getArgument(method, static_cast<unsigned>(i));
        const std::string name = spelling(parameter);
        const CXType full_type = clang_getCanonicalType(clang_getCursorType(parameter));
        const CXType type = withoutReference(full_type);
        const std::string type_name = withoutConst(qualifiedName(type));

        if (full_type.kind == CXType_LValueReference &&
            clang_isConstQualifiedType(type) == 0) {
            return errorAt(parameter, "Route parameter '" + name +
                                          "' can't be a non-const reference");
        }

        const std::vector<std::string> marks = annotationsOf(parameter);
        if (hasAnnotation(marks, kRequiredAnnotation)) {
            return errorAt(parameter, "Route parameter '" + name + "' has CPPBOOT_REQUIRED, which "
                                      "is for settings. A parameter is required unless it's a "
                                      "std::optional");
        }
        const bool is_url_value = isUrlValue(type);
        if (hasValidationMark(marks) && (!is_url_value || name.empty())) {
            // A check here would never run, and would look as if it did.
            return errorAt(parameter, "Route parameter '" + name + "' has a check, but only path "
                                      "and query parameters can have one. For a request body, put "
                                      "checks on the fields of its struct");
        }

        if (type_name == "cppboot::web::Request") {
            parameters.push_back(RouteParameter{ParameterSource::Request, name, type_name});
            continue;
        }
        if (type_name == "cppboot::security::Principal") {
            parameters.push_back(RouteParameter{ParameterSource::Principal, name, type_name});
            continue;
        }
        if (name.empty()) {
            return errorAt(parameter, "Route parameters need a name");
        }

        const auto path_name = std::find(unbound.begin(), unbound.end(), name);
        const bool is_path = path_name != unbound.end();
        if (is_path && (!is_url_value || isOptionalType(type))) {
            return errorAt(parameter,
                           "Path parameter '" + name + "' must be text, a number or a bool");
        }
        if (is_path || is_url_value) {
            auto checks = readChecks(parameter, marks, type,
                                     std::string{is_path ? "Path" : "Query"} + " parameter '" +
                                         name + "'");
            if (!checks) {
                return checks.error();
            }
            if (is_path) {
                unbound.erase(path_name);
            }
            parameters.push_back(
                RouteParameter{is_path ? ParameterSource::Path : ParameterSource::Query, name,
                               type_name, std::move(checks).value()});
        } else {
            if (has_body) {
                return errorAt(parameter, "A route can only read one parameter from the request "
                                          "body, but '" + name + "' is a second one");
            }
            has_body = true;
            parameters.push_back(RouteParameter{ParameterSource::Body, name, type_name});
        }
    }

    if (!unbound.empty()) {
        return errorAt(method, "Route " + path + " has path parameter '" + unbound.front() +
                                   "', but its function has no parameter with that name");
    }
    return parameters;
}

/// Reads one route annotation, which looks like "cppboot:route:Get:/{id}".
Result<Route> readRoute(CXCursor method, std::string_view annotation, std::string_view base_path) {
    const auto colon = annotation.find(':');
    const std::string_view route_path = annotation.substr(colon + 1);

    Route route;
    route.method = annotation.substr(0, colon);
    route.function = spelling(method);
    route.location = locationOf(method);

    if (route_path.empty() || route_path.front() != '/') {
        return errorAt(method, "The route path of " + route.function + " must start with '/'");
    }
    if (clang_getCXXAccessSpecifier(method) != CX_CXXPublic) {
        return errorAt(method, "Route function " + route.function + " must be public");
    }
    if (clang_CXXMethod_isStatic(method) != 0) {
        return errorAt(method, "Route function " + route.function + " can't be static");
    }

    route.path = joinPaths(base_path, route_path);

    auto parameters = readRouteParameters(method, route.path);
    if (!parameters) {
        return parameters.error();
    }
    route.parameters = std::move(parameters).value();

    const CXType result = clang_getCanonicalType(clang_getCursorResultType(method));
    if (result.kind == CXType_Void) {
        route.return_kind = ReturnKind::Nothing;
    } else if (withoutConst(qualifiedName(withoutReference(result))) == "cppboot::web::Response") {
        route.return_kind = ReturnKind::Response;
    } else {
        route.return_kind = ReturnKind::Value;
    }
    return route;
}

/// Applies a route's security marks. A mark on a function replaces those on its controller.
std::optional<Error> applySecurityMarks(CXCursor method, const std::vector<std::string>& method_marks,
                                        const std::vector<std::string>& class_marks, Route& route) {
    const std::vector<std::string> method_roles = allWithPrefix(method_marks, kRolePrefix);
    const std::vector<std::string> class_roles = allWithPrefix(class_marks, kRolePrefix);
    const bool method_public = hasAnnotation(method_marks, kPublicAnnotation);

    if (method_public && !method_roles.empty()) {
        return errorAt(method, "Route function " + route.function +
                                   " can't be both CPPBOOT_PUBLIC and CPPBOOT_ROLE");
    }

    route.is_public = method_public ||
                      (method_roles.empty() && hasAnnotation(class_marks, kPublicAnnotation));
    if (!route.is_public) {
        route.roles = method_roles.empty() ? class_roles : method_roles;
    }

    const bool takes_principal =
        std::any_of(route.parameters.begin(), route.parameters.end(),
                    [](const RouteParameter& parameter) {
                        return parameter.source == ParameterSource::Principal;
                    });
    if (route.is_public && takes_principal) {
        return errorAt(method, "Public route function " + route.function +
                                   " can't take a Principal, because nobody has signed in");
    }
    if (std::find(route.roles.begin(), route.roles.end(), "") != route.roles.end()) {
        return errorAt(method, "Route function " + route.function + " has an empty role name");
    }
    return std::nullopt;
}

/// Reads a limit such as "5/minute".
std::optional<RouteRateLimit> parseRateLimit(std::string_view text) {
    constexpr std::uint32_t kMostRequests = 1'000'000;

    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }

    RouteRateLimit limit;
    const char* const end = text.data() + slash;
    const auto [parsed_end, error] = std::from_chars(text.data(), end, limit.requests);
    if (error != std::errc{} || parsed_end != end || limit.requests == 0 ||
        limit.requests > kMostRequests) {
        return std::nullopt;
    }

    const std::string_view unit = text.substr(slash + 1);
    if (unit == "second") {
        limit.window_seconds = 1;
    } else if (unit == "minute") {
        limit.window_seconds = 60;
    } else if (unit == "hour") {
        limit.window_seconds = 3600;
    } else {
        return std::nullopt;
    }
    return limit;
}

/// Applies a route's rate limit. A mark on a function replaces the one on its controller.
std::optional<Error> applyRateLimitMark(CXCursor method, const std::vector<std::string>& method_marks,
                                        const std::vector<std::string>& class_marks, Route& route) {
    const std::vector<std::string> method_limits = allWithPrefix(method_marks, kRateLimitPrefix);
    const std::vector<std::string> class_limits = allWithPrefix(class_marks, kRateLimitPrefix);
    const std::vector<std::string>& limits = method_limits.empty() ? class_limits : method_limits;

    if (limits.empty()) {
        return std::nullopt;
    }
    if (limits.size() > 1) {
        return errorAt(method, "Route function " + route.function + " has more than one rate limit");
    }

    route.rate_limit = parseRateLimit(limits.front());
    if (!route.rate_limit) {
        // A limit that can't be read must not leave the route with no limit.
        return errorAt(method, "The rate limit '" + limits.front() + "' of " + route.function +
                                   " must look like \"5/minute\", with 1 to 1000000 requests per "
                                   "second, minute or hour");
    }
    return std::nullopt;
}

Result<Controller> readController(CXCursor cursor, std::string_view base_path,
                                  const std::vector<std::string>& class_marks,
                                  std::vector<std::string>& warnings) {
    Controller controller;
    controller.type = qualifiedName(clang_getCursorType(cursor));

    if (!base_path.empty() && base_path.front() != '/') {
        return errorAt(cursor, "The base path of " + controller.type + " must start with '/'");
    }
    if (hasAnnotation(class_marks, kPublicAnnotation) &&
        findWithPrefix(class_marks, kRolePrefix)) {
        return errorAt(cursor,
                       controller.type + " can't be both CPPBOOT_PUBLIC and CPPBOOT_ROLE");
    }

    std::optional<Error> error;
    visitChildren(cursor, [&](CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_CXXMethod) {
            const std::vector<std::string> marks = annotationsOf(child);

            // A security mark that does nothing would look like protection that isn't there.
            if (hasSecurityMark(marks) && !findWithPrefix(marks, kRoutePrefix)) {
                error = errorAt(child, "Function " + spelling(child) +
                                           " has a security mark but isn't a route");
                return CXChildVisit_Break;
            }
            if (findWithPrefix(marks, kRateLimitPrefix) && !findWithPrefix(marks, kRoutePrefix)) {
                error = errorAt(child, "Function " + spelling(child) +
                                           " has a rate limit but isn't a route");
                return CXChildVisit_Break;
            }

            for (const std::string& annotation : marks) {
                if (!annotation.starts_with(kRoutePrefix)) {
                    continue;
                }
                auto route = readRoute(
                    child, std::string_view{annotation}.substr(kRoutePrefix.size()), base_path);
                if (!route) {
                    error = route.error();
                    return CXChildVisit_Break;
                }
                if (auto mark_error =
                        applySecurityMarks(child, marks, class_marks, route.value())) {
                    error = std::move(mark_error);
                    return CXChildVisit_Break;
                }
                if (auto mark_error =
                        applyRateLimitMark(child, marks, class_marks, route.value())) {
                    error = std::move(mark_error);
                    return CXChildVisit_Break;
                }
                if (isView(clang_getCanonicalType(clang_getCursorResultType(child)))) {
                    warnings.push_back(warningAt(
                        child, "Route function " + route.value().function +
                                   " returns a view, which dangles if it points at a local "
                                   "variable. Return a std::string or a std::vector"));
                }
                controller.routes.push_back(std::move(route).value());
            }
        }
        return CXChildVisit_Continue;
    });

    if (error) {
        return *std::move(error);
    }
    if (controller.routes.empty()) {
        return errorAt(cursor, controller.type + " has no routes. Mark a function with "
                                                 "CPPBOOT_GET or a similar macro");
    }
    return controller;
}

/// Finds route and security marks on a class that isn't a controller, where they'd otherwise do
/// nothing. A forgotten controller mark must not look like a protected route.
std::optional<Error> findStrayMark(CXCursor cursor, const std::vector<std::string>& class_marks) {
    if (hasSecurityMark(class_marks)) {
        return errorAt(cursor, "Security marks must be on a class marked with CPPBOOT_CONTROLLER");
    }
    if (findWithPrefix(class_marks, kRateLimitPrefix)) {
        return errorAt(cursor, "A rate limit must be on a class marked with CPPBOOT_CONTROLLER");
    }

    std::optional<Error> error;
    visitChildren(cursor, [&](CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_CXXMethod) {
            const std::vector<std::string> marks = annotationsOf(child);
            if (findWithPrefix(marks, kRoutePrefix) || hasSecurityMark(marks) ||
                findWithPrefix(marks, kRateLimitPrefix)) {
                error = errorAt(child, "Route function " + spelling(child) +
                                           " must be in a class marked with CPPBOOT_CONTROLLER");
                return CXChildVisit_Break;
            }
        }
        return CXChildVisit_Continue;
    });
    return error;
}

Result<Middleware> readMiddleware(CXCursor cursor, std::string_view order_text) {
    Middleware middleware;
    middleware.type = qualifiedName(clang_getCursorType(cursor));
    middleware.location = locationOf(cursor);

    const char* const end = order_text.data() + order_text.size();
    const auto [parsed_end, error] = std::from_chars(order_text.data(), end, middleware.order);
    if (error != std::errc{} || parsed_end != end) {
        return errorAt(cursor, "The order of " + middleware.type + " must be a whole number");
    }

    bool has_handle = false;
    visitChildren(cursor, [&](CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_CXXMethod && spelling(child) == "handle" &&
            clang_getCXXAccessSpecifier(child) == CX_CXXPublic) {
            has_handle = true;
        }
        return CXChildVisit_Continue;
    });
    if (!has_handle) {
        return errorAt(cursor, middleware.type + " needs a public function named handle");
    }
    return middleware;
}

struct Walk {
    Declarations declarations;
    std::optional<Error> error;
};

void readClass(CXCursor cursor, Walk& walk) {
    const std::vector<std::string> annotations = annotationsOf(cursor);
    const auto controller_path = findWithPrefix(annotations, kControllerPrefix);
    const auto middleware_order = findWithPrefix(annotations, kMiddlewarePrefix);
    const bool is_service = hasAnnotation(annotations, kServiceAnnotation);

    if (is_service || controller_path || middleware_order) {
        auto component = readComponent(cursor);
        if (!component) {
            walk.error = component.error();
            return;
        }
        if (auto unsafe = checkComponentFields(cursor, component.value(), walk.declarations.warnings)) {
            walk.error = std::move(unsafe);
            return;
        }
        walk.declarations.components.push_back(std::move(component).value());
    }

    if (middleware_order) {
        auto middleware = readMiddleware(cursor, *middleware_order);
        if (!middleware) {
            walk.error = middleware.error();
            return;
        }
        walk.declarations.middleware.push_back(std::move(middleware).value());
    }

    if (controller_path) {
        auto controller =
            readController(cursor, *controller_path, annotations, walk.declarations.warnings);
        if (!controller) {
            walk.error = controller.error();
            return;
        }
        walk.declarations.controllers.push_back(std::move(controller).value());
    } else if (auto stray = findStrayMark(cursor, annotations)) {
        walk.error = std::move(stray);
        return;
    }

    if (const auto config_prefix = findWithPrefix(annotations, kConfigPrefix)) {
        if (is_service || controller_path || middleware_order) {
            walk.error = errorAt(cursor, spelling(cursor) + " can't be both CPPBOOT_CONFIG and "
                                                            "another kind of component");
            return;
        }
        auto settings = readConfigStruct(cursor, *config_prefix);
        if (!settings) {
            walk.error = settings.error();
            return;
        }
        walk.declarations.config_types.push_back(std::move(settings).value());
    }

    const bool is_json = hasAnnotation(annotations, kJsonAnnotation);
    if (is_json) {
        auto json_type = readMappedStruct(cursor, "CPPBOOT_JSON", true);
        if (!json_type) {
            walk.error = json_type.error();
            return;
        }
        walk.declarations.json_types.push_back(std::move(json_type).value());
    }

    if (hasAnnotation(annotations, kRowAnnotation)) {
        // Checks on a struct that's also read from JSON are fine here, and reading a row skips them.
        auto row_type = readMappedStruct(cursor, "CPPBOOT_ROW", is_json);
        if (!row_type) {
            walk.error = row_type.error();
            return;
        }
        walk.declarations.row_types.push_back(std::move(row_type).value());
    }
}

void visitDeclarations(CXCursor parent, Walk& walk) {
    visitChildren(parent, [&](CXCursor cursor) {
        if (clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) == 0) {
            return CXChildVisit_Continue;
        }

        const CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_Namespace) {
            visitDeclarations(cursor, walk);
        } else if (kind == CXCursor_ClassTemplate && !annotationsOf(cursor).empty()) {
            walk.error = errorAt(cursor, spelling(cursor) + " can't be marked because it is a "
                                                            "template");
        } else if ((kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) &&
                   clang_isCursorDefinition(cursor) != 0) {
            readClass(cursor, walk);
            if (!walk.error) {
                visitDeclarations(cursor, walk);
            }
        }
        return walk.error ? CXChildVisit_Break : CXChildVisit_Continue;
    });
}

std::string collectErrors(CXTranslationUnit unit) {
    std::string errors;
    const unsigned count = clang_getNumDiagnostics(unit);
    for (unsigned i = 0; i < count; ++i) {
        const CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
        if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
            errors += toString(
                clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions()));
            errors += '\n';
        }
        clang_disposeDiagnostic(diagnostic);
    }
    return errors;
}

}  // namespace

std::string clangResourceDirectory() {
    if (auto executable = platform::executablePath()) {
        const auto installed =
            executable.value().parent_path().parent_path() / "lib" / "cppboot" / "clang";
        std::error_code ignored;
        if (std::filesystem::exists(installed / "include" / "stddef.h", ignored)) {
            return installed.generic_string();
        }
    }
    return CPPBOOT_LIBCLANG_RESOURCE_DIR;
}

Result<Declarations> parseDeclarations(const std::filesystem::path& header,
                                       const std::vector<std::string>& compiler_arguments,
                                       std::vector<std::filesystem::path>* included_files) {
    const std::string resource_directory = clangResourceDirectory();
    std::vector<const char*> arguments = {
        "-x", "c++", "-std=c++20", "-DCPPBOOT_CODEGEN", "-resource-dir", resource_directory.c_str(),
    };
    for (const std::string& argument : compiler_arguments) {
        arguments.push_back(argument.c_str());
    }

    const std::unique_ptr<void, decltype(&clang_disposeIndex)> index{clang_createIndex(0, 0),
                                                                     clang_disposeIndex};
    const std::string path = header.string();

    CXTranslationUnit raw_unit = nullptr;
    const CXErrorCode code = clang_parseTranslationUnit2(
        index.get(), path.c_str(), arguments.data(), static_cast<int>(arguments.size()), nullptr, 0,
        CXTranslationUnit_SkipFunctionBodies, &raw_unit);
    if (code != CXError_Success) {
        return Error{ErrorCode::ParseError, "libclang can't parse " + path + " (error " +
                                                std::to_string(static_cast<int>(code)) + ")"};
    }
    const std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>,
                          decltype(&clang_disposeTranslationUnit)>
        unit{raw_unit, clang_disposeTranslationUnit};

    if (const std::string errors = collectErrors(unit.get()); !errors.empty()) {
        return Error{ErrorCode::ParseError, "Can't compile " + path + ":\n" + errors};
    }

    if (included_files != nullptr) {
        clang_getInclusions(
            unit.get(),
            [](CXFile file, CXSourceLocation*, unsigned, CXClientData data) {
                const std::string name = toString(clang_getFileName(file));
                if (!name.empty()) {
                    static_cast<std::vector<std::filesystem::path>*>(data)->emplace_back(name);
                }
            },
            included_files);
    }

    Walk walk;
    visitDeclarations(clang_getTranslationUnitCursor(unit.get()), walk);
    if (walk.error) {
        return *std::move(walk.error);
    }
    return std::move(walk.declarations);
}

}  // namespace cppboot::codegen
