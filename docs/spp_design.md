# SPP (Safe Programming Plus) Library - System Design Document

## 1. Overview

SPP (Safe Programming Plus) is a C++20/23 header-only library that combines Rust's memory safety guarantees with OCaml's functional programming elegance. It serves as a modern alternative to the C++ Standard Library, focusing on compile-time safety, zero-cost abstractions, and explicit ownership semantics.

**Core Philosophy**: "Safety without Sacrifice" - Provide memory safety and functional programming constructs without compromising performance or control.

## 2. Design Principles

### 2.1 Safety First
- Compile-time ownership tracking (inspired by Rust's borrow checker via static analysis)
- No implicit copies or moves
- Explicit lifetime annotations
- Bounds checking in debug builds (optional in release)

### 2.2 Functional Programming
- Immutable data structures as defaults
- Algebraic Data Types (ADTs) with pattern matching
- Higher-order functions and function composition
- Pure functions marked with `[[nodiscard]]` and `constexpr`

### 2.3 Zero-Cost Abstractions
- Template metaprogramming for compile-time dispatch
- Type erasure only when necessary
- Custom allocator support for all containers
- Minimal runtime overhead

### 2.4 Explicitness
- No exceptions (use `Result<T, E>` and `Option<T>`)
- No RTTI (compile-time type information via concepts)
- Explicit constructors and conversions
- Clear ownership transfer semantics

### 2.5 Interoperability
- Seamless integration with existing C++ code
- C ABI compatibility for FFI
- STL adapters for gradual migration
- Support for coroutines and async/await

## 3. Core Components

### 3.1 Ownership System

#### 3.1.1 Smart Pointers
```cpp
template<typename T, typename Allocator = Mdefault>
class Box;  // Unique ownership, movable but not copyable

template<typename T, typename Allocator = Mdefault>
class Rc;   // Reference counted shared ownership

template<typename T, typename Allocator = Mdefault>
class Arc;  // Atomic reference counted for thread safety

template<typename T>
class Ref<'a>;  // Borrowed reference with lifetime annotation
```

#### 3.1.2 Lifetime Annotations
- Compile-time lifetime tracking via template parameters
- Lifetime elision rules similar to Rust
- Static analysis tools for lifetime verification

### 3.2 Type System

#### 3.2.1 Algebraic Data Types
```cpp
// Sum types (variants)
template<typename... Ts>
class Variant;

// Product types (tuples)
template<typename... Ts>
class Tuple;

// Optional values
template<typename T>
class Option;

// Result type for error handling
template<typename T, typename E>
class Result;
```

#### 3.2.2 Pattern Matching
```cpp
spp::match(value) {
    spp::case_<int>([](int x) { /* handle int */ }),
    spp::case_<std::string>([](const std::string& s) { /* handle string */ }),
    spp::default_([]() { /* default case */ })
};
```

#### 3.2.3 Type Classes (Concepts)
```cpp
template<typename T>
concept Cloneable = requires(T t) {
    { t.clone() } -> std::same_as<T>;
};

template<typename T>
concept Display = requires(T t, std::ostream& os) {
    { os << t } -> std::same_as<std::ostream&>;
};
```

### 3.3 Containers

#### 3.3.1 Immutable Collections
```cpp
template<typename T, typename Allocator = Mdefault>
class Vec;  // Persistent vector with structural sharing

template<typename K, typename V, typename Allocator = Mdefault>
class Map;  // Persistent hash map (HAMT-based)

template<typename T, typename Allocator = Mdefault>
class List; // Persistent linked list

template<typename T, size_t N>
class Array; // Fixed-size stack array
```

#### 3.3.2 Mutable Collections
```cpp
template<typename T, typename Allocator = Mdefault>
class MutVec;  // Mutable vector (unique ownership)

template<typename K, typename V, typename Allocator = Mdefault>
class MutMap;  // Mutable hash map
```

#### 3.3.3 Views and Slices
```cpp
template<typename T>
class Slice<'a>;  // Non-owning view into contiguous memory

template<typename Char>
class StringView<'a>;  // String slice
```

### 3.4 Allocator System

#### 3.4.1 Allocator Types
```cpp
struct Mdefault;      // Global heap allocator
struct Mregion<'a>;   // Region-based allocator (stack-like)
struct Mpool;         // Memory pool allocator
struct Marena;        // Arena allocator
struct Mstatic;       // Static memory allocator
```

#### 3.4.2 Allocator-Aware Containers
- All containers parameterized by allocator type
- Compile-time allocator selection
- Custom allocator support via Allocator concept

### 3.5 Error Handling

#### 3.5.1 Result Type
```cpp
template<typename T, typename E = Error>
class Result {
    // Monadic operations: map, and_then, or_else
    // Early return with spp::try macro
};
```

#### 3.5.2 Error Types
```cpp
class Error {
    std::string message;
    ErrorCode code;
    std::vector<Error> causes;
};

// Domain-specific error types
class IoError : public Error { /* ... */ };
class ParseError : public Error { /* ... */ };
```

### 3.6 Concurrency

#### 3.6.1 Async/Await
```cpp
template<typename T>
class Task;  // Coroutine-based async task

template<typename T>
class Future; // Future/promise pattern

// Async runtime with work-stealing scheduler
class AsyncRuntime;
```

#### 3.6.2 Concurrent Data Structures
```cpp
template<typename T>
class ConcurrentVec;  // Lock-free or fine-grained locking

template<typename K, typename V>
class ConcurrentMap;
```

#### 3.6.3 Channels
```cpp
template<typename T>
class Sender;
template<typename T>
class Receiver;

// MPSC, SPMC, MPMC channel variants
```

### 3.7 Reflection and Metaprogramming

#### 3.7.1 Compile-time Reflection
```cpp
template<typename T>
constexpr auto type_info = spp::reflect<T>();

// Field iteration
spp::for_each_field<T>([](auto name, auto type, auto value) {
    // Process field
});
```

#### 3.7.2 Serialization
```cpp
template<typename T>
concept Serializable = requires(T t, Serializer& s) {
    { s.serialize(t) } -> std::same_as<void>;
};

// JSON, MessagePack, Protocol Buffers support
```

## 4. Memory Model

### 4.1 Ownership Rules
1. **Unique Ownership**: `Box<T>` has exclusive ownership, movable but not copyable
2. **Shared Ownership**: `Rc<T>` and `Arc<T>` for shared references with reference counting
3. **Borrowed References**: `Ref<'a, T>` for non-owning references with lifetime annotations
4. **Slices**: `Slice<'a, T>` for contiguous memory views

### 4.2 Lifetime System
- Lifetime parameters as template arguments
- Lifetime elision for common patterns
- Compile-time verification of reference validity

### 4.3 Memory Safety Guarantees
- No use-after-free (enforced by ownership system)
- No double-free (enforced by RAII)
- No data races (enforced by `Arc<T>` and `Mutex<T>`)
- Bounds checking (debug builds, optional in release)

## 5. Module Structure (Inspired by spclib)

### 5.1 Core Modules
```
spp/
├── spp.hpp                    # Main header (includes all or modular headers)
├── core/
│   ├── ownership.hpp          # Box, Rc, Arc, Ref
│   ├── types.hpp             # Option, Result, Variant, Tuple
│   ├── allocator.hpp         # Allocator system
│   └── traits.hpp            # Concepts and type traits
├── containers/
│   ├── vec.hpp               # Vector implementations
│   ├── map.hpp               # Map implementations
│   ├── string.hpp            # String types
│   └── slice.hpp             # Slice and StringView
├── functional/
│   ├── match.hpp             # Pattern matching
│   ├── iterator.hpp          # Functional iterators
│   └── compose.hpp           # Function composition
├── async/
│   ├── task.hpp              # Async tasks
│   ├── runtime.hpp           # Async runtime
│   └── channel.hpp           # Channels
├── io/
│   ├── file.hpp              # File I/O
│   ├── stream.hpp            # Stream utilities
│   └── serialization.hpp     # Serialization formats
├── concurrency/
│   ├── mutex.hpp             # Synchronization primitives
│   ├── atomic.hpp            # Atomic operations
│   └── concurrent.hpp        # Concurrent data structures
└── reflection/
    ├── reflect.hpp           # Compile-time reflection
    └── meta.hpp              # Metaprogramming utilities
```

### 5.2 Optional Modules
```
spp/ext/
├── stl_adapter.hpp           # STL compatibility layer
├── c_api.hpp                 # C interoperability
├── http.hpp                  # HTTP client/server
└── json.hpp                  # JSON parsing/serialization
```

### 5.3 Build Configuration
```cpp
// Single-header mode
#define SPP_IMPLEMENTATION
#include "spp.hpp"

// Modular mode
#include "spp/core/ownership.hpp"
#include "spp/containers/vec.hpp"

// Configuration macros
#define SPP_ENABLE_BOUNDS_CHECK   // Enable bounds checking
#define SPP_DISABLE_EXCEPTIONS    // Disable exception support
#define SPP_CUSTOM_ALLOCATOR      // Use custom default allocator
```

## 6. Compiler and Platform Support

### 6.1 Compiler Requirements
- GCC 11+ (C++20 support)
- Clang 14+ (C++20 support)
- MSVC 2022+ (C++20 support)

### 6.2 Standard Library Dependencies
- Minimal dependencies: `<type_traits>`, `<utility>`, `<memory>` (for std::allocator)
- Optional: Coroutine TS support for async/await
- No Boost dependencies

### 6.3 Platform Support
- Linux (glibc, musl)
- macOS
- Windows (MSVC, MinGW)
- WebAssembly (Emscripten)

## 7. Performance Characteristics

### 7.1 Compile Time
- Header-only design with optional compilation firewall
- Forward declarations to reduce template instantiation overhead
- Compile-time caching of reflection data

### 7.2 Runtime Performance
- Zero-overhead abstractions (cost-free when not used)
- Inlining of small functions
- Custom allocators for performance-critical code
- SIMD optimizations where applicable

### 7.3 Memory Usage
- Minimal overhead for smart pointers (1-2 words)
- Structural sharing for immutable collections
- Custom allocators to reduce fragmentation

## 8. Safety Features

### 8.1 Static Analysis
- Lifetime annotations for borrow checking
- Move semantics enforcement
- Const correctness

### 8.2 Dynamic Checks (Debug Builds)
- Bounds checking
- Null pointer checking
- Double-free detection
- Use-after-free detection (via allocator tracking)

### 8.3 Testing Support
- Property-based testing (QuickCheck-style)
- Fuzz testing integration
- Concurrency testing (Loom-style)
- Memory sanitizer support

## 9. Migration Strategy

### 9.1 From STL
- Adapter layer for gradual migration
- Compatibility headers for common STL types
- Conversion utilities between STL and SPP types

### 9.2 From Rust/OCaml
- Similar API design for familiarity
- Interop via C ABI for Rust code
- OCaml-like pattern matching syntax

### 9.3 From RPP
- Similar ownership model for easy transition
- Enhanced functional programming support
- Better interoperability with existing C++ code

## 10. Development Roadmap

### Phase 1: Core Foundation
- Ownership system (Box, Rc, Arc, Ref)
- Basic types (Option, Result, Variant)
- Allocator system
- Basic containers (Vec, String, Slice)

### Phase 2: Functional Programming
- Pattern matching
- Immutable collections
- Iterator adapters
- Function composition

### Phase 3: Concurrency
- Async/await support
- Channels and concurrent data structures
- Task scheduler

### Phase 4: Ecosystem
- Serialization formats
- Network I/O
- File system operations
- Reflection system

### Phase 5: Optimization
- Performance tuning
- Memory usage optimization
- Compile-time improvements

## 11. Comparison with Alternatives

### vs. C++ STL
- **SPP**: Memory safety, functional programming, explicit ownership
- **STL**: Mature, widely adopted, but lacks safety guarantees

### vs. Rust
- **SPP**: C++ interoperability, gradual adoption, no borrow checker complexity
- **Rust**: Strong safety guarantees, but steeper learning curve, slower compilation

### vs. RPP
- **SPP**: Functional programming focus, better OCaml integration, more explicit safety
- **RPP**: Rust-inspired, game development focus, similar performance goals

### vs. OCaml
- **SPP**: C++ performance, systems programming, direct hardware access
- **OCaml**: Strong functional programming, type inference, but GC pause times

## 12. Conclusion

SPP aims to bring Rust's memory safety and OCaml's functional elegance to C++ without sacrificing performance or control. By leveraging modern C++ features (concepts, constexpr, coroutines) and a carefully designed ownership system, SPP provides a safe, expressive, and high-performance alternative to traditional C++ libraries.

The library's modular design, inspired by spclib's single-header approach, allows for gradual adoption and easy integration with existing codebases. With its focus on compile-time safety, zero-cost abstractions, and functional programming, SPP represents a significant step forward for safe systems programming in C++.

---

*This document provides the architectural blueprint for SPP. Implementation details, API specifications, and concrete examples will be developed in subsequent phases by Codex ChatGPT5.4 as requested by the user.*