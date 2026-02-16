#include <stdio.h>
#include <stdint.h>

typedef struct AsciiMsg {
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;
    uint8_t b4;
    uint8_t b5;
    uint8_t b6;
    uint8_t b7;
    uint8_t b8;
    uint8_t b9;
    uint8_t b10;
    uint8_t b11;
    uint8_t b12;
} AsciiMsg;

AsciiMsg g_msg;

int c_print_g_msg(void) {
    const char* text = (const char*)&g_msg;
    return printf("%s", text);
}
