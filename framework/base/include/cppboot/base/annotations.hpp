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

/// Makes an interface whose functions the generator writes, from their names, for a table:
///   class CPPBOOT_REPOSITORY("products") ProductRepository
///       : public cppboot::data::Repository<Product> {
///   public:
///       virtual std::vector<Product> findByCategoryOrderByPriceDesc(const std::string& c) = 0;
///   };
/// The generated class is registered as the interface, so components take ProductRepository&.
#define CPPBOOT_REPOSITORY(table) CPPBOOT_ANNOTATE("cppboot:repository:" table)

/// Gives a repository function its SQL, rather than deriving it from the name. Parameters go to $1,
/// $2 and so on, in order: CPPBOOT_QUERY("SELECT ... WHERE price < $1")
#define CPPBOOT_QUERY(sql) CPPBOOT_ANNOTATE("cppboot:query:" sql)

/// Fills a struct from the settings under a prefix when the application starts, and makes it a
/// component that others can take as const Settings&:
///   struct CPPBOOT_CONFIG("mail") MailSettings { std::string host; int port{25}; };
/// reads mail.host and mail.port. A field keeps its value if its setting isn't set. A value of the
/// wrong type, a failed check or a setting under the prefix that matches no field stops startup.
#define CPPBOOT_CONFIG(prefix) CPPBOOT_ANNOTATE("cppboot:config:" prefix)

/// On a field of a CPPBOOT_CONFIG struct: the setting has to be set, or startup stops.
#define CPPBOOT_REQUIRED CPPBOOT_ANNOTATE("cppboot:required")

// Checks on a field of a CPPBOOT_JSON or CPPBOOT_CONFIG struct, or on a path or query parameter of
// a route function. A request that fails one gets a 400 naming the field, and a setting that fails
// one stops startup. On a std::optional, they apply only when there's a value.

/// Text or a container can't be empty: CPPBOOT_NOT_EMPTY std::string name;
#define CPPBOOT_NOT_EMPTY CPPBOOT_ANNOTATE("cppboot:not_empty")

/// Text has from min to max characters, or a container that many items:
///   CPPBOOT_LENGTH(1, 50) std::string name;
#define CPPBOOT_LENGTH(min, max) CPPBOOT_ANNOTATE("cppboot:length:" #min ":" #max)

/// A number is at least min, at most max, or both: CPPBOOT_RANGE(1, 100) int quantity{1};
#define CPPBOOT_MIN(min) CPPBOOT_ANNOTATE("cppboot:min:" #min)
#define CPPBOOT_MAX(max) CPPBOOT_ANNOTATE("cppboot:max:" #max)
#define CPPBOOT_RANGE(min, max) CPPBOOT_ANNOTATE("cppboot:range:" #min ":" #max)

/// Tells the generator that a risky field of a component is safe, and why. Its reason is required,
/// so that whoever reads the code later can check it:
///   CPPBOOT_UNCHECKED("Only ever points at string literals") std::string_view label_;
#define CPPBOOT_UNCHECKED(reason) CPPBOOT_ANNOTATE("cppboot:unchecked:" reason)

// Lets Clang warn about a reference that outlives the object it came from. Other compilers ignore
// it, so it finds some dangling references on some builds, and proves nothing.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define CPPBOOT_LIFETIMEBOUND [[clang::lifetimebound]]
#endif
#endif
#if !defined(CPPBOOT_LIFETIMEBOUND)
#define CPPBOOT_LIFETIMEBOUND
#endif
