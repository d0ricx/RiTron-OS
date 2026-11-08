#include <stddef.h>
#include <stdint.h>

// Minimal libc-compatible primitives required by generated code and the Rust
// static library; the freestanding kernel does not link a hosted C runtime.
extern "C" void *memcpy(void *destination, const void *source,
                        size_t length) {
    uint8_t *output = static_cast<uint8_t *>(destination);
    const uint8_t *input = static_cast<const uint8_t *>(source);
    for (size_t index = 0; index < length; ++index) {
        output[index] = input[index];
    }
    return destination;
}

extern "C" void *memset(void *destination, int value, size_t length) {
    uint8_t *output = static_cast<uint8_t *>(destination);
    for (size_t index = 0; index < length; ++index) {
        output[index] = (uint8_t)value;
    }
    return destination;
}

extern "C" int memcmp(const void *left, const void *right, size_t length) {
    const uint8_t *left_bytes = static_cast<const uint8_t *>(left);
    const uint8_t *right_bytes = static_cast<const uint8_t *>(right);
    for (size_t index = 0; index < length; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return (int)left_bytes[index] - (int)right_bytes[index];
        }
    }
    return 0;
}

// The kernel aborts on Rust panics and has no unwinder; the GNU core archive
// still carries unwind metadata that references this personality symbol.
extern "C" void rust_eh_personality(void) {}
