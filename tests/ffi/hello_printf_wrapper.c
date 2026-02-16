#include <stdint.h>
#include <stdio.h>

typedef struct ParusText {
    const uint8_t* data;
    uint64_t len;
} ParusText;

int c_print_text(ParusText msg) {
    return printf("%.*s", (int)msg.len, (const char*)msg.data);
}
