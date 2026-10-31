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

/// Makes a class a component that runs around every request. It needs this public function:
///   cppboot::web::Response handle(const cppboot::web::Request&, const cppboot::web::Next&);
/// Lower numbers run first: class CPPBOOT_MIDDLEWARE(10) RequestLog { ... };
#define CPPBOOT_MIDDLEWARE(order) CPPBOOT_ANNOTATE("cppboot:middleware:" #order)

/// Once a project uses security, every route needs a valid token unless it has this mark. It can go
/// on a controller or on a route function.
#define CPPBOOT_PUBLIC CPPBOOT_ANNOTATE("cppboot:public")

/// Limits a controller or route function to users with this role. With several marks, any one of
/// the roles is enough. A mark on a function replaces those on its controller.
#define CPPBOOT_ROLE(role) CPPBOOT_ANNOTATE("cppboot:role:" role)

/// Limits how often each client can call a route, such as "5/minute". The units are second, minute
/// and hour. On a controller, it gives each of its routes that limit, and a mark on a function
/// replaces it.
#define CPPBOOT_RATE_LIMIT(limit) CPPBOOT_ANNOTATE("cppboot:rate_limit:" limit)
