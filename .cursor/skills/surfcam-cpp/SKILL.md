---
name: surfcam-cpp
description: >-
  Applies modern C++ (C++17/20) conventions, STL usage, testing, security, and
  documentation practices for SurfCamWeatherPi. Use when writing, editing, or
  reviewing C++ in this repository or when the user asks about C++ style here.
---

# SurfCamWeatherPi C++

Apply senior-level C++ practices consistent with the codebase.

## Expertise baseline

Favor modern C++ (C++17/20), the STL, and idiomatic system-level programming.

## Code style and structure

- Write concise, idiomatic C++ code with accurate examples.
- Follow modern C++ conventions and best practices.
- Use object-oriented, procedural, or functional patterns as appropriate.
- Leverage the STL and standard algorithms for collection operations.
- Use descriptive variable and method names (e.g. `isUserSignedIn`, `calculateTotal`).
- Structure files into headers (`.hpp`) and implementation files (`.cpp`) with clear separation of concerns.

## Naming conventions

- PascalCase for class names.
- camelCase for variables and methods.
- SCREAMING_SNAKE_CASE for constants and macros.
- Prefix member variables with an underscore or `m_` (e.g. `_userId`, `m_userId`).
- Use namespaces to organize code logically.

## C++ feature usage

- Prefer modern features (`auto`, range-based loops, smart pointers).
- Use `std::unique_ptr` and `std::shared_ptr` for memory management.
- Prefer `std::optional`, `std::variant`, and `std::any` where they clarify intent.
- Use `constexpr` and `const` for compile-time and immutability where appropriate.
- Use `std::string_view` for read-only string parameters to avoid unnecessary copies.

## Syntax and formatting

- Follow one consistent style (e.g. Google C++ Style Guide or existing project norms).
- Place braces on the same line for control structures and methods unless the project already does otherwise.
- Keep comments clear and consistent with surrounding code.

## Error handling and validation

- Use exceptions where appropriate (e.g. `std::runtime_error`, `std::invalid_argument`).
- Use RAII for resource management.
- Validate inputs at function boundaries.
- Log errors with the project’s logging approach (e.g. spdlog, Boost.Log) when adding new paths.

## Performance

- Avoid unnecessary heap allocations; prefer stack or existing buffers where safe.
- Use `std::move` where it preserves semantics and avoids copies.
- Prefer `<algorithm>` for non-trivial loops when readability and correctness improve.
- Profile hot paths when optimization is required.

## Key conventions

- Prefer smart pointers over raw pointers for ownership.
- Avoid globals; use singletons sparingly.
- Use `enum class` for enumerations.
- Separate interface from implementation in classes.
- Use templates and metaprogramming only when they pay for themselves.

## Testing

- Add or extend unit tests with the project’s framework (e.g. GoogleTest, Catch2).
- Mock dependencies with Google Mock or equivalent when the codebase already does.
- Cover integration points for system components when behavior changes.

## Security

- Avoid buffer overflows, use-after-free, and dangling pointers.
- Prefer `std::array` or `std::vector` over raw arrays.
- Avoid C-style casts; use `static_cast`, `dynamic_cast`, or `reinterpret_cast` when necessary.
- Enforce const-correctness on APIs and members.

## Documentation

- Comment non-obvious classes, methods, and logic.
- Use Doxygen-style comments if the project uses Doxygen.
- Document assumptions, constraints, and expected behavior where it affects callers.

Align with ISO C++ guidance and patterns already present in this repository.
