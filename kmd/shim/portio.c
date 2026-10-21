// SPDX-License-Identifier: MIT
// MSVC's port I/O intrinsics, out of line. clang defines them only inline in its own intrin.h, which the WDK headers
// do not include, so the virtio legacy register accessors compile to external references.
unsigned char __inbyte(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %w1, %b0" : "=a"(v) : "Nd"(port));
    return v;
}

unsigned short __inword(unsigned short port)
{
    unsigned short v;
    __asm__ volatile("inw %w1, %w0" : "=a"(v) : "Nd"(port));
    return v;
}

unsigned long __indword(unsigned short port)
{
    unsigned long v;
    __asm__ volatile("inl %w1, %k0" : "=a"(v) : "Nd"(port));
    return v;
}

void __outbyte(unsigned short port, unsigned char v) { __asm__ volatile("outb %b0, %w1" : : "a"(v), "Nd"(port)); }
void __outword(unsigned short port, unsigned short v) { __asm__ volatile("outw %w0, %w1" : : "a"(v), "Nd"(port)); }
void __outdword(unsigned short port, unsigned long v) { __asm__ volatile("outl %k0, %w1" : : "a"(v), "Nd"(port)); }
