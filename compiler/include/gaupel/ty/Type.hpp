// compiler/include/gaupel/ty/Type.hpp
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>


namespace gaupel::ty {

    using TypeId = uint32_t;
    inline constexpr TypeId kInvalidType = 0xFFFF'FFFFu;

    enum class Builtin : uint8_t {
        kNull,

        kBool,
        kChar,

        kI8,  kI16,  kI32,  kI64,
        kU8,  kU16,  kU32,  kU64,
        kISize, kUSize,

        kF32, kF64,
    };

    enum class Kind : uint8_t {
        kError,
        kBuiltin,
        kOptional, // T?
        kArray,    // T[]
        kNamedUser, // user-defined type name (v0: unresolved/class later)

        kBorrow,     // &T / &mut T
        kEscape,     // &&T 

        kFn,        // (T1, T2, ...) -> R
    };

    struct Type {
        Kind kind = Kind::kError;

        // kBuiltin
        Builtin builtin = Builtin::kNull;

        // kOptional / kArray
        TypeId elem = kInvalidType;

        // kNamedUser
        std::string_view name{};

        // kBorrow
        bool borrow_is_mut = false;
        
        // kFn
        TypeId ret = kInvalidType;
        uint32_t param_begin = 0;
        uint32_t param_count = 0;
    };

} // namespace gaupel::ty