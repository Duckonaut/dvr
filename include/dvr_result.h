#pragma once

#include "dvr_log.h"
#include "dvr_types.h"
#include "dvr_utils.h"

#ifndef DVR_ABORT
#include <stdlib.h>
#define DVR_ABORT exit(1)
#endif

// [[nodiscard]] is a C23 feature, not supported by all compilers
// Hide it behind a macro and enable it only for compilers that support it
#if defined(__clang__) || defined(__GNUC__)
#define DVR_NODISCARD [[nodiscard]]
#else
#define DVR_NODISCARD
#endif

typedef struct {
    const char* message;
    const char* file;
    const char* function;
    u32 line;
} dvr_error_t;

#define DVR_RESULT_DEF(ok_type)                                                                \
    typedef struct DVR_NODISCARD dvr_result_##ok_type {                                        \
        union {                                                                                \
            ok_type ok;                                                                        \
            dvr_error_t error;                                                                 \
        };                                                                                     \
        bool is_ok;                                                                            \
    } dvr_result_##ok_type##_t

#define DVR_RESULT(ok_type) dvr_result_##ok_type##_t
#define DVR_OK(ok_type, value)                                                                 \
    (DVR_RESULT(ok_type)) {                                                                    \
        .ok = value, .is_ok = true,                                                            \
    }
#define DVR_ERROR(ok_type, m)                                                                  \
    (DVR_RESULT(ok_type)) {                                                                    \
        .error =                                                                               \
            (dvr_error_t){                                                                     \
                .message = m,                                                                  \
                .file = __FILE__,                                                              \
                .function = __func__,                                                          \
                .line = __LINE__,                                                              \
            },                                                                                 \
        .is_ok = false,                                                                        \
    }

/// Bubble the error through the call stack.
///
/// Only works if the ok type of the result is same as the ok type of the result
/// of the enclosing function.
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call)
#define DVR_BUBBLE(result)                                                                     \
    {                                                                                          \
        if (!result.is_ok) {                                                                  \
            return result;                                                                    \
        }                                                                                      \
    }

/// Bubble the error through the call stack.
///
/// Pass in the ok type of the enclosing function to convert the error correctly.
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call)
#define DVR_BUBBLE_INTO(scope_type, result)                                                    \
    {                                                                                          \
        if (!result.is_ok) {                                                                   \
            return DVR_ERROR(scope_type, result.error.message);                                \
        }                                                                                      \
    }

/// Extract the ok value of the result. The result needs to be checked for errors beforehand,
/// otherwise the error will be printed and program terminated with DVR_ABORT
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call).
#define DVR_UNWRAP(result)                                                                     \
    (result.is_ok ? result.ok                                                                  \
                  : (DVRLOG_ERROR(                                                             \
                         "%s %s:%d: %s",                                                       \
                         result.error.file,                                                    \
                         result.error.function,                                                \
                         result.error.line,                                                    \
                         result.error.message                                                  \
                     ),                                                                        \
                     DVR_ABORT,                                                                \
                     result.ok))

/// Extract the ok value of the result. If the result was an error, print error, and return the
/// or value.
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call).
#define DVR_UNWRAP_OR(result, or)                                                              \
    (result.is_ok ? result.ok                                                                  \
                  : (DVRLOG_ERROR(                                                             \
                         "(silenced) %s %s:%d: %s",                                            \
                         result.error.file,                                                    \
                         result.error.function,                                                \
                         result.error.line,                                                    \
                         result.error.message                                                  \
                     ),                                                                        \
                     or                                                                        \
                    ))

/// Print error if result is one.
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call).
#define DVR_SHOW_ERROR(result)                                                                 \
    if (!result.is_ok) {                                                                       \
        DVRLOG_ERROR(                                                                          \
            "%s %s:%d: %s",                                                                    \
            result.error.file,                                                                 \
            result.error.function,                                                             \
            result.error.line,                                                                 \
            result.error.message                                                               \
        );                                                                                     \
    }

/// Print error and exit if result is one.
///
/// NOT Safe to call with the parameter generating side effects (i.e. function call).
#define DVR_EXIT_ON_ERROR(result)                                                              \
    if (!result.is_ok) {                                                                       \
        DVRLOG_ERROR(                                                                          \
            "%s %s:%d: %s",                                                                    \
            result.error.file,                                                                 \
            result.error.function,                                                             \
            result.error.line,                                                                 \
            result.error.message                                                               \
        );                                                                                     \
        DVR_ABORT;                                                                             \
    }

/// Check if result is ok.
///
/// Safe to call with the parameter generating side effects (i.e. function call)
#define DVR_RESULT_IS_OK(result) (result.is_ok)
/// Check if result is an error.
///
/// Safe to call with the parameter generating side effects (i.e. function call)
#define DVR_RESULT_IS_ERROR(result) (!result.is_ok)

typedef struct dvr_none {
    u8 _nothing;
} dvr_none;

#define DVR_NONE                                                                               \
    (dvr_none) {}

DVR_RESULT_DEF(dvr_none);

DVR_RESULT_DEF(i32);
DVR_RESULT_DEF(u32);
DVR_RESULT_DEF(dvr_range);
