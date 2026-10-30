#pragma once

// Marks a declaration for cppboot-codegen. These macros compile to nothing in a normal build, so
// they add no runtime cost and work with every compiler. The generator defines CPPBOOT_CODEGEN
// and reads the attributes through libclang.
#if defined(CPPBOOT_CODEGEN)
#define CPPBOOT_ANNOTATE(text) [[clang::annotate(text)]]
#else
#define CPPBOOT_ANNOTATE(text)
#endif

/// Registers a class as a component: class CPPBOOT_SERVICE UserService { ... };
#define CPPBOOT_SERVICE CPPBOOT_ANNOTATE("cppboot:service")

/// Generates JSON conversion for a struct: struct CPPBOOT_JSON User { int id; std::string name; };
#define CPPBOOT_JSON CPPBOOT_ANNOTATE("cppboot:json")

/// Generates reading a struct from a query result, with fields matched to columns by name:
/// struct CPPBOOT_ROW User { int id; std::string name; };
#define CPPBOOT_ROW CPPBOOT_ANNOTATE("cppboot:row")

/// Makes a class a component whose marked functions handle requests under a base path:
/// class CPPBOOT_CONTROLLER("/api/users") UserController { ... };
#define CPPBOOT_CONTROLLER(path) CPPBOOT_ANNOTATE("cppboot:controller:" path)

/// Makes a controller function handle a route: CPPBOOT_GET("/{id}") User get(int id);
#define CPPBOOT_GET(path) CPPBOOT_ANNOTATE("cppboot:route:Get:" path)
#define CPPBOOT_POST(path) CPPBOOT_ANNOTATE("cppboot:route:Post:" path)
#define CPPBOOT_PUT(path) CPPBOOT_ANNOTATE("cppboot:route:Put:" path)
#define CPPBOOT_PATCH(path) CPPBOOT_ANNOTATE("cppboot:route:Patch:" path)
#define CPPBOOT_DELETE(path) CPPBOOT_ANNOTATE("cppboot:route:Delete:" path)
