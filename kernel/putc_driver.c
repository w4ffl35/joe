// SPDX-License-Identifier: GPL-3.0
//
// putc_driver.c — override of the runtime's weak `curlee_putc` hook with a
// COM1 (serial port 0x3F8) UART implementation.
//
// Curlee's freestanding runtime (runtime/rt.c) declares `curlee_putc` weak and
// no-op by default so linking always succeeds. When this object is linked in,
// it overrides that symbol, so `curlee_putc(c)` (called from the kernel's
// serial_hello) emits each character over COM1. QEMU captures this with
// `-serial file:...` / `-serial stdio`, giving an observable, deterministic
// boot check: "Hello world from JOE" appears in the serial log.
//
// This is freestanding (no libc): uses only `outb`/`inb` and inlined asm.

// COM1 registers (I/O ports).
#define COM1_DATA  0x3F8
#define COM1_LCR   0x3FB
#define COM1_LSR   0x3FD

// Serial line status register bit 5 (THR empty).
#define LSR_THR_EMPTY 0x20

static inline void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" ::"a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void curlee_putc(char c)
{
    // Wait for the transmit-holding register to be empty (THR empty bit set).
    while ((inb(COM1_LSR) & LSR_THR_EMPTY) == 0)
    {
        __asm__ volatile("pause");
    }
    outb(COM1_DATA, (unsigned char)c);
}
