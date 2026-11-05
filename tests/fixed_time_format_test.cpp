#include "fixed_time_format.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <initializer_list>

int main() {
    // Cover the uint32 epoch range, leap days, year boundaries, and both
    // timezone extremes. Calendar conversion remains the existing libc call.
    for (uint64_t epoch = 0; epoch <= UINT32_MAX; epoch += 43201) {
        for (int offset : {-50400, 0, 50400}) {
            time_t seconds = static_cast<time_t>(epoch) + offset;
            // Windows' host CRT rejects pre-1970 timestamps.
            if (seconds < 0) continue;
            const struct tm* converted = gmtime(&seconds);
            assert(converted);
            struct tm value = *converted;
            for (bool iso : {false, true}) {
                char expected[32], actual[32];
                strftime(expected, sizeof(expected),
                         iso ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%d %H:%M:%S",
                         &value);
                formatFixedTime(value, actual, sizeof(actual), iso);
                assert(strcmp(expected, actual) == 0);
                for (size_t length = 0; length <= 32; ++length) {
                    memset(actual, '!', sizeof(actual));
                    formatFixedTime(value, actual, length, iso);
                    if (length == 0) assert(actual[0] == '!');
                    else if (length < (iso ? 21u : 20u)) assert(actual[0] == '\0');
                    else assert(strcmp(expected, actual) == 0);
                    if (length < sizeof(actual)) assert(actual[length] == '!');
                }
            }
        }
    }
    formatFixedTime(tm{}, nullptr, 32, false);
    puts("Fixed time formatting matches strftime; buffer checks passed.");
}
