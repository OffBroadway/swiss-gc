#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "common.h"

// #include <stdarg.h>
// #include "rpl_vsnprintf.c"

s32 WriteUARTN(const void *buf, u32 len);

void _puts(char *s) {
    int length = strlen(s);
    WriteUARTN(s, length);
    return;
}

// void gprintf(const char *fmt, ...) {
//     va_list args;
//     static char buf[256];

//     va_start(args, fmt);
//     int length = vsnprintf((char *)buf, sizeof(buf), (char *)fmt, args);
//     WriteUARTN(buf, length);

//     va_end(args);
// }
