#pragma once

#include "dvr_log.h"
#include "dvr_types.h"
#include "dvr_utils.h"

typedef struct {
    const char* message;
    const char* file;
    const char* function;
    u32 line;
} dvr_error_t;

#define DVR_RESULT_DEF(ok_type)                                                                \
    typedef struct [[nodiscard]] dvr_result_##ok_type {                                        \
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

#define DVR_BUBBLE(result)                                                                     \
    {                                                                                          \
        typeof(result) result_ = result;                                                       \
        if (!result_.is_ok) {                                                                  \
            return result_;                                                                    \
        }                                                                                      \
    }

#define DVR_BUBBLE_INTO(scope_type, result)                                                    \
    {                                                                                          \
        typeof(result) result_ = result;                                                       \
        if (!result_.is_ok) {                                                                  \
            return DVR_ERROR(scope_type, result_.error.message);                               \
        }                                                                                      \
    }

#define DVR_TRY_DO(result, if_ok)                                                              \
    {                                                                                          \
        typeof(result) result_ = result;                                                       \
        if (result_.is_ok) {                                                                   \
            typeof(result_.ok) value = result_.ok;                                             \
            if_ok;                                                                             \
        } else {                                                                               \
            return result_;                                                                    \
        }                                                                                      \
    }

#define DVR_UNWRAP(result)                                                                     \
    (result.is_ok ? result.ok                                                                  \
                  : (DVRLOG_ERROR(                                                             \
                         "%s %s:%d: %s",                                                       \
                         result.error.file,                                                    \
                         result.error.function,                                                \
                         result.error.line,                                                    \
                         result.error.message                                                  \
                     ),                                                                        \
                     exit(1),                                                                  \
                     result.ok))

#define DVR_UNWRAP_OR(result, or)                                                              \
    (result.is_ok ? result.ok                                                                  \
                  : (DVRLOG_ERROR(                                                             \
                         "%s %s:%d: %s",                                                       \
                         result.error.file,                                                    \
                         result.error.function,                                                \
                         result.error.line,                                                    \
                         result.error.message                                                  \
                     ),                                                                        \
                     or                                                                        \
                    ))

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

#define DVR_EXIT_ON_ERROR(result)                                                              \
    if (!result.is_ok) {                                                                       \
        DVRLOG_ERROR(                                                                          \
            "%s %s:%d: %s",                                                                    \
            result.error.file,                                                                 \
            result.error.function,                                                             \
            result.error.line,                                                                 \
            result.error.message                                                               \
        );                                                                                     \
        exit(1);                                                                               \
    }

#define DVR_RESULT_IS_OK(result) (result.is_ok)
#define DVR_RESULT_IS_ERROR(result) (!result.is_ok)

DVR_RESULT_DEF(i32);
DVR_RESULT_DEF(u32);
DVR_RESULT_DEF(dvr_range);
