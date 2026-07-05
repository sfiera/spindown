#include <gba_types.h>

typedef struct level {
    u8         w, h;
    const char title[30];
    const char data[14 * 14];
} level_t;

extern level_t level_set[50];
