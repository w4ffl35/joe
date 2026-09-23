// SPDX-License-Identifier: GPL-3.0
//
// host_panic.c — curlee_panic for the host-side codegen probes
// (run-net-stack-codegen.sh, run-json-codegen.sh, run-irq-snn-codegen.sh,
// run-mb2-codegen.sh).
//
// `curlee build` wraps every array access it cannot prove in bounds with
// curlee_bounds_guard(...), which calls
// `extern _Noreturn void curlee_panic(const char* msg);`. The real kernel
// gets this from runtime/rt.c (linked by `make kernel`'s --link path); the
// probes above compile the generated C with a plain `cc` and no kernel
// runtime, so the symbol is otherwise undefined.
//
// This is deliberately NOT runtime/rt.c linked in as-is: that file also
// defines memset/memcpy/memcmp (which a hosted link already gets from
// libc) and an x86 `cli; hlt` halt loop meant for a freestanding kernel —
// executed in host user space, `hlt` is a privileged instruction and
// faults instead of exiting cleanly. A guard firing in a probe must fail
// loudly and deterministically, so this prints the message and exits
// non-zero instead of looping or crashing.

#include <stdio.h>
#include <stdlib.h>

_Noreturn void curlee_panic(const char* msg)
{
    fprintf(stderr, "curlee_panic: %s\n", msg);
    exit(1);
}
