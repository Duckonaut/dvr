#include "dvr_log.h"

#include <stdio.h>

FILE* g_dvr_log_file;

#ifndef __unix__
#include <windows.h>
#include <io.h>

HANDLE g_dvr_log_handle;

void dvr_log_init() {
    g_dvr_log_handle = GetStdHandle(STD_ERROR_HANDLE);

    if (g_dvr_log_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "GetStdHandle failed: %d\n", GetLastError());
        exit(1);
    }

#ifdef RELEASE
    g_dvr_log_file = fopen("dvr_log.txt", "w");
#endif
}

void dvr_log_close() {
    if (g_dvr_log_file) {
        fclose(g_dvr_log_file);
    }
}

#else

void dvr_log_init(void) {
#ifdef RELEASE
    g_dvr_log_file = fopen("dvr_log.txt", "w");
#endif
}

void dvr_log_close(void) {
    if (g_dvr_log_file) {
        fclose(g_dvr_log_file);
    }
}

#endif
