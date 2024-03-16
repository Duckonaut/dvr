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
        .base = &value, .size = sizeof(value)                                                  \
    }

