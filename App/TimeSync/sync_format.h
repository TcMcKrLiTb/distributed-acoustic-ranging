#ifndef APP_SYNC_FORMAT_H__
#define APP_SYNC_FORMAT_H__
#include <stdint.h>

/* Includes space for the sign, 19 decimal digits and NUL. */
#define SYNC_I64_TEXT_SIZE 21U

void SyncFormat_I64(char text[SYNC_I64_TEXT_SIZE], int64_t value);

#endif
