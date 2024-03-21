#pragma once

#include "dvr_types.h"

static inline i32 dvr_clamp(i32 v, i32 min, i32 max) {
    if (v < min) {
        return min;
    } else if (v > max) {
        return max;
    } else {
        return v;
    }
}

static inline u32 dvr_clampu(u32 v, u32 min, u32 max) {
    if (v < min) {
        return min;
    } else if (v > max) {
        return max;
    } else {
        return v;
    }
}

static inline f32 dvr_clampf(f32 v, f32 min, f32 max) {
    if (v < min) {
        return min;
    } else if (v > max) {
        return max;
    } else {
        return v;
    }
}

typedef struct dvr_range {
    void* base;
    usize size;
} dvr_range;

#define DVR_RANGE(value)                                                                       \
    (dvr_range) {                                                                              \
        .base = (void*)&value, .size = sizeof(value)                                           \
    }

#define DVR_RANGE_NULL                                                                         \
    (dvr_range) {                                                                              \
        .base = NULL, .size = 0                                                                \
    }

typedef struct dvr_result_dvr_range dvr_result_dvr_range_t;

dvr_result_dvr_range_t dvr_read_file(const char* path);
dvr_result_dvr_range_t dvr_read_file_range(const char* path, usize offset, usize size);
void dvr_free_file(dvr_range range);
