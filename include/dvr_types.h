#pragma once
#include <cglm/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef float f32;
typedef double f64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;
#if defined(__linux__)
typedef __ssize_t isize;
#elif defined(_WIN32)
typedef ptrdiff_t isize;
#endif
