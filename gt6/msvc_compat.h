#pragma once

// The recompiler's portable runtime spells these operations using Clang/GCC
// built-ins. Map them to the corresponding MSVC intrinsics for native Windows
// builds without changing lifted output.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define __builtin_bswap16 _byteswap_ushort
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
#define __builtin_return_address(level) _ReturnAddress()
#endif
