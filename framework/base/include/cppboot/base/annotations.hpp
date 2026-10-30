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
