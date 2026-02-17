// frontend/include/parus/ty/Type.hpp
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>


namespace parus::ty {

    using TypeId = uint32_t;
    inline constexpr TypeId kInvalidType = 0xFFFF'FFFFu;

    enum class Builtin : uint8_t {
        kNull,

        kUnit,
        kNever,

        kBool,
        kChar,
        kText,   // builtin string slice: {ptr u8, usize}

        // signed integers
        kI8,  kI16,  kI32,  kI64, kI128,
        
        // unsigned integers
        kU8,  kU16,  kU32,  kU64, kU128,
        
        kISize, kUSize,

        kF32, kF64, kF128,

        // -------------------------------------------------
        // INTERNAL ONLY (user cannot spell these type names)
        // -------------------------------------------------
        kInferInteger, // "{integer}" placeholder (Rust-like unsuffixed integer)
    };

    enum class Kind : uint8_t {
        kError,
        kBuiltin,
        kOptional,  // T?
        kArray,     // T[]
        kNamedUser, // user-defined type name (NOW: path slice)

        kBorrow,    // &T / &mut T
        kEscape,    // &&T
        kPtr,       // ptr T / ptr mut T

        kFn,        // def(T1, T2, ...) -> R
    };

    struct Type {
        Kind kind = Kind::kError;

        // kBuiltin
        Builtin builtin = Builtin::kNull;

        // kOptional / kArray / kBorrow / kEscape
        TypeId elem = kInvalidType;

        // kArray
        // - false: unsized array/slice element type (T[])
        // - true : fixed-size array (T[N])
        bool array_has_size = false;
        uint32_t array_size = 0;

        // kNamedUser: path slice (no string flatten!)
        uint32_t path_begin = 0;
        uint32_t path_count = 0;

        // kBorrow
        bool borrow_is_mut = false;
        // kPtr
        bool ptr_is_mut = false;

        // kFn
        TypeId ret = kInvalidType;
        uint32_t param_begin = 0;
        uint32_t param_count = 0;
    };

} // namespace parus::ty
