#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern FILE* g_dvr_log_file;

#ifndef __unix__
#include <Windows.h>
extern HANDLE g_log_handle;
#endif

void dvr_log_init(void);
void dvr_log_close(void);

// if shipping, strip all logging
#ifndef SHIPPING

// if in release, log to pre-opened file descriptor
#ifdef RELEASE
#define DVRLOG_FILE g_dvr_log_file
#else
#define DVRLOG_FILE stderr
#endif

#define __DVRLOG_ERROR_STR "ERROR"
#define __DVRLOG_WARNING_STR "WARN "
#define __DVRLOG_INFO_STR "INFO "
#define __DVRLOG_DEBUG_STR "DEBUG"

#ifdef RELEASE
#define DVRLOG_ERROR(...)                                                                      \
    fprintf(DVRLOG_FILE, __DVRLOG_ERROR_STR ": " __VA_ARGS__), fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_WARNING(...)                                                                    \
    fprintf(DVRLOG_FILE, __DVRLOG_WARNING_STR ": " __VA_ARGS__), fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_INFO(...)                                                                       \
    fprintf(DVRLOG_FILE, __DVRLOG_INFO_STR ": " __VA_ARGS__), fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_DEBUG(...)
#else
#ifdef __unix__
#define __DVRLOG_NC "\033[0m"
#define __DVRLOG_ERROR "\033[31m"
#define __DVRLOG_WARNING "\033[33m"
#define __DVRLOG_INFO "\033[32m"
#define __DVRLOG_DEBUG "\033[34m"

#define DVRLOG_ERROR(...)                                                                      \
    fprintf(DVRLOG_FILE, __DVRLOG_ERROR __DVRLOG_ERROR_STR __DVRLOG_NC ": " __VA_ARGS__),      \
        fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_WARNING(...)                                                                    \
    fprintf(DVRLOG_FILE, __DVRLOG_WARNING __DVRLOG_WARNING_STR __DVRLOG_NC ": " __VA_ARGS__),  \
        fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_INFO(...)                                                                       \
    fprintf(DVRLOG_FILE, __DVRLOG_INFO __DVRLOG_INFO_STR __DVRLOG_NC ": " __VA_ARGS__),        \
        fprintf(DVRLOG_FILE, "\n")
#define DVRLOG_DEBUG(...)                                                                      \
    fprintf(DVRLOG_FILE, __DVRLOG_DEBUG __DVRLOG_DEBUG_STR __DVRLOG_NC ": " __VA_ARGS__),      \
        fprintf(DVRLOG_FILE, "\n")

#else // __unix__
#define __DVRLOG_ERROR 0x0C
#define __DVRLOG_WARNING 0x0E
#define __DVRLOG_INFO 0x0A
#define __DVRLOG_DEBUG 0x09

#define DVRLOG_ERROR(...)                                                                      \
                                                                                               \
    SetConsoleTextAttribute(g_log_handle, __DVRLOG_ERROR),                                     \
        fprintf(DVRLOG_FILE, __DVRLOG_ERROR_STR ": "),                                         \
        SetConsoleTextAttribute(g_log_handle, 0x07), fprintf(DVRLOG_FILE, __VA_ARGS__),        \
        fprintf(DVRLOG_FILE, "\n")

#define DVRLOG_WARNING(...)                                                                    \
                                                                                               \
    SetConsoleTextAttribute(g_log_handle, __DVRLOG_WARNING),                                   \
        fprintf(DVRLOG_FILE, __DVRLOG_WARNING_STR ": "),                                       \
        SetConsoleTextAttribute(g_log_handle, 0x07), fprintf(DVRLOG_FILE, __VA_ARGS__),        \
        fprintf(DVRLOG_FILE, "\n")

#define DVRLOG_INFO(...)                                                                       \
    SetConsoleTextAttribute(g_log_handle, __DVRLOG_INFO),                                      \
        fprintf(DVRLOG_FILE, __DVRLOG_INFO_STR ": "),                                          \
        SetConsoleTextAttribute(g_log_handle, 0x07), fprintf(DVRLOG_FILE, __VA_ARGS__),        \
        fprintf(DVRLOG_FILE, "\n") #define DVRLOG_DEBUG(...)                                   \
            SetConsoleTextAttribute(g_log_handle, __DVRLOG_DEBUG),                             \
        fprintf(DVRLOG_FILE, __DVRLOG_DEBUG_STR ": "),                                         \
        SetConsoleTextAttribute(g_log_handle, 0x07), fprintf(DVRLOG_FILE, __VA_ARGS__),        \
        fprintf(DVRLOG_FILE, "\n")
#endif // __unix__
#endif // RELEASE

#else // SHIPPING

#define DVRLOG_ERROR(...)
#define DVRLOG_WARNING(...)
#define DVRLOG_INFO(...)
#define DVRLOG_DEBUG(...)

#endif

#ifdef __cplusplus
}
#endif
