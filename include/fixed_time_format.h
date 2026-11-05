#pragma once

#include <stdio.h>
#include <time.h>

// The protocol uses only these numeric, locale-independent date formats.
static inline void formatFixedTime(const struct tm& value, char* out,
                                   size_t outLen, bool iso) {
    if (!out || outLen == 0) return;
    // strftime does not promise useful output when the buffer is too small.
    // Make that case deterministic and always leave a terminated string.
    if (outLen < (iso ? 21u : 20u)) {
        out[0] = '\0';
        return;
    }
    snprintf(out, outLen, "%04d-%02d-%02d%c%02d:%02d:%02d%s",
             value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
             iso ? 'T' : ' ', value.tm_hour, value.tm_min, value.tm_sec,
             iso ? "Z" : "");
}
