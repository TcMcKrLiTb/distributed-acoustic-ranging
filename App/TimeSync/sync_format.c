#include "sync_format.h"

/* newlib-nano's printf need not support %lld. Keep the UART format independent
   of that library option, including INT64_MIN (whose negation overflows). */
void SyncFormat_I64(char text[SYNC_I64_TEXT_SIZE], const int64_t value) {
    char reverse[20];
    unsigned count = 0, out = 0;
    uint64_t magnitude = value < 0 ? 0ULL - (uint64_t)value : (uint64_t)value;
    do {
        reverse[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0)
        text[out++] = '-';
    while (count)
        text[out++] = reverse[--count];
    text[out] = '\0';
}
