#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "common.h"

#include <stdarg.h>
// #include "rpl_vsnprintf.c"

s32 WriteUARTN(const void *buf, u32 len);

void _puts(char *s) {
    int length = strlen(s);
    WriteUARTN(s, length);
    return;
}

typedef void* (*WriteProc_t)(void*, const char*, size_t);

// Pokemon XD (NTSC-U)
int (*__pformatter)(WriteProc_t WriteProc, void* WriteProcArg, const char* format_str, va_list arg) = (void*)0x800dfb2c;
WriteProc_t __StringWrite = (void*)0x800dfa68;

typedef struct {
	char* CharStr;
	size_t MaxCharCount;
	size_t CharsWritten;
} __OutStrCtrl;

// from https://github.com/projectPiki/pikmin2/blob/0285984b81a1c837063ae1852d94607fdb21d64c/src/Dolphin/MSL_C/MSL_Common/printf.c#L1267-L1310
int vsnprintf(char* s, size_t n, const char* format, va_list arg) {
	int end;
	__OutStrCtrl osc;
	osc.CharStr      = s;
	osc.MaxCharCount = n;
	osc.CharsWritten = 0;

	end = __pformatter(__StringWrite, &osc, format, arg);

	if (s) {
		s[(end < n) ? end : n - 1] = '\0';
	}

	return end;
}

void gprintf(const char *fmt, ...) {
    va_list args;
    static char buf[256];

    va_start(args, fmt);
    int length = vsnprintf((char *)buf, sizeof(buf), (char *)fmt, args);
    WriteUARTN(buf, length);

    va_end(args);
}
