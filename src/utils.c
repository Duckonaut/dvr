#include "dvr_utils.h"

#include "dvr_result.h"

#include <stdio.h>
#include <stdlib.h>

DVR_RESULT(dvr_range) dvr_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return DVR_ERROR(dvr_range, "failed to open file");
    }

    fseek(file, 0, SEEK_END);
    usize file_size = (usize)ftell(file);
    rewind(file);

    char* buffer = malloc(file_size);
    if (buffer == NULL) {
        fclose(file);
        return DVR_ERROR(dvr_range, "failed to allocate buffer");
    }

    if (fread(buffer, 1, file_size, file) != file_size) {
        fclose(file);
        free(buffer);
        return DVR_ERROR(dvr_range, "failed to read file");
    }

    fclose(file);

    dvr_range range = {
        .base = buffer,
        .size = file_size,
    };

    return DVR_OK(dvr_range, range);
}

DVR_RESULT(dvr_range) dvr_read_file_range(const char* path, usize offset, usize size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return DVR_ERROR(dvr_range, "failed to open file");
    }

    if (fseek(file, (u32)offset, SEEK_SET) != 0) {
        fclose(file);
        return DVR_ERROR(dvr_range, "failed to seek file");
    }

    char* buffer = malloc(size);
    if (buffer == NULL) {
        fclose(file);
        return DVR_ERROR(dvr_range, "failed to allocate buffer");
    }

    if (fread(buffer, 1, size, file) != size) {
        fclose(file);
        free(buffer);
        return DVR_ERROR(dvr_range, "failed to read file");
    }

    fclose(file);

    dvr_range range = {
        .base = buffer,
        .size = size,
    };

    return DVR_OK(dvr_range, range);
}

void dvr_free_file(dvr_range range) {
    free(range.base);
}
