#include "cppboot/codegen/parser.hpp"

#include <clang-c/Index.h>

#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cppboot::codegen {

namespace {

constexpr std::string_view kServiceAnnotation = "cppboot:service";

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

bool hasServiceAnnotation(CXCursor cursor) {
    bool found = false;
    visitChildren(cursor, [&](CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_AnnotateAttr &&
            spelling(child) == kServiceAnnotation) {
            found = true;
            return CXChildVisit_Break;
        }
        return CXChildVisit_Continue;
    });
    return found;
}

/// Returns T if the type is std::shared_ptr<T>, taken by value or by reference.
std::optional<std::string> sharedPointerTarget(CXType parameter_type) {
    CXType type = clang_getCanonicalType(parameter_type);
    if (type.kind == CXType_LValueReference || type.kind == CXType_RValueReference) {
        type = clang_getCanonicalType(clang_getPointeeType(type));
    }

    const CXCursor declaration = clang_getTypeDeclaration(type);
    const std::string parent = spelling(clang_getCursorSemanticParent(declaration));
    if (spelling(declaration) != "shared_ptr" || (parent != "std" && parent != "__1") ||
        clang_Type_getNumTemplateArguments(type) != 1) {
        return std::nullopt;
    }

    std::string target = qualifiedName(clang_Type_getTemplateArgumentAsType(type, 0));
    constexpr std::string_view kConst = "const ";
    if (target.starts_with(kConst)) {
        target.erase(0, kConst.size());
    }
    return target;
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
            return errorAt(parameter, "Constructor parameter '" + spelling(parameter) + "' of " +
                                          component.type +
                                          " must be a std::shared_ptr to a component");
        }
        component.dependencies.push_back(*std::move(target));
    }
    return component;
}

struct Walk {
    std::vector<Component> components;
    std::optional<Error> error;
};

void visitDeclarations(CXCursor parent, Walk& walk) {
    visitChildren(parent, [&](CXCursor cursor) {
        if (clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) == 0) {
            return CXChildVisit_Continue;
        }

        const CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_Namespace) {
            visitDeclarations(cursor, walk);
        } else if (kind == CXCursor_ClassTemplate && hasServiceAnnotation(cursor)) {
            walk.error = errorAt(cursor, spelling(cursor) + " can't be a component because it is "
                                                            "a template");
        } else if ((kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) &&
                   clang_isCursorDefinition(cursor) != 0) {
            if (hasServiceAnnotation(cursor)) {
                auto component = readComponent(cursor);
                if (!component) {
                    walk.error = component.error();
                } else {
                    walk.components.push_back(std::move(component).value());
                }
            }
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

Result<std::vector<Component>> parseComponents(const std::filesystem::path& header,
                                               const std::vector<std::string>& compiler_arguments) {
    std::vector<const char*> arguments = {
        "-x", "c++", "-std=c++20", "-DCPPBOOT_CODEGEN", "-resource-dir", CPPBOOT_LIBCLANG_RESOURCE_DIR,
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

    Walk walk;
    visitDeclarations(clang_getTranslationUnitCursor(unit.get()), walk);
    if (walk.error) {
        return *std::move(walk.error);
    }
    return std::move(walk.components);
}

}  // namespace cppboot::codegen
