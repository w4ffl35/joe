## Workspace-wide rules — JOE OS (apply to every mode)

These load for every mode regardless of slug. Mode-specific rules
(`.roo/rules-<slug>/`) supplement this — they don't replace it.

---

## What we are working on

**JOE** is a blazing-fast, minimal operating system written in
**Curlee** — a custom, verification-first programming language built by
the same team. Curlee is the language; JOE is the OS that runs it.

- **Current phase:** Phase 1 — bare-metal "Hello World from JOE" x86-64
  kernel. Boots under QEMU (primary) and VirtualBox via GRUB ISO
  (secondary). Serial output on COM1 is the authoritative console.
- **Status:** pre-alpha. The kernel renders "Hello World from JOE" to
  the VGA text buffer (0xB8000) and emits the same text on serial, then
  halts deterministically.
- **License:** GPL 3.0 (this repo). Curlee itself is MIT.

### Key repos

| Repo | Path | Role |
|---|---|---|
| JOE (this project) | `~/Projects/joeos` | The OS: kernel, boot, scripts, Makefile |
| Curlee | `~/Projects/curlee` | The language + compiler toolchain (source of truth for syntax) |
| AIRunner | `~/Projects/airunner` | Unrelated project — only used as a structural reference for `.roo/` rules layout |

The **Curlee compiler** lives at `~/Projects/curlee/build/linux-debug/curlee`
(auto-detected by `scripts/find-curlee.sh`; override with `CURLEE` or
`CURLEE_ROOT`).

### Project layout

```
Makefile                 # build: kernel.elf, iso; run: qemu; verify gates
scripts/
  find-curlee.sh         # locate the curlee compiler (CURLEE/CURLEE_ROOT/PATH)
  build_iso.sh           # grub-mkrescue GRUB ISO (VirtualBox path)
  vbox-setup.sh          # automated VirtualBox VM creation + boot (VBoxManage)
kernel/
  kernel.curlee          # kmain: VGA text renderer + serial + halt
  pack.curlee            # pure cell/pixel helpers (VM-testable, verified)
  boot.S                 # multiboot2 64-bit entry (GRUB ISO path)
  putc_driver.c          # COM1 serial override of weak curlee_putc
  vga_setup.c            # VGA text-mode-3 programming (QEMU display path)
  fb.c                   # framebuffer glyph renderer (future path)
```

### Build, verify & run (the dev loop)

```sh
make kernel     # -> build/kernel.elf  (QEMU path, curlee build -> freestanding C -> cc/ld)
make iso        # -> build/joeos.iso   (VirtualBox/GRUB path)
make verify     # static gates: check, packer test, ELF entry, _start,
                # curlee_main, PVH note
make qemu-smoke # dynamic gate: boot kernel.elf, assert serial log contains
                # "Hello World from JOE"  <-- the acceptance gate
make qemu-serial # primary dev loop: kernel output prints LIVE to terminal
make check      # curlee check on kernel.curlee + pack.curlee (no proof, no build)
make pack-run   # curlee run pack.curlee (asserts pure cell math == 0)
```

### Design laws (do not violate)

1. **Single address space, Ring 0** — `Phys<T>` writes are direct
   volatile stores; no MMU, no privilege transitions.
2. **Deterministic verification** — every `phys<U32>(0xADDR)` address
   is a constant literal (Curlee rule); the packer is pure `Int -> Int`;
   the full check pipeline (lex → parse → resolve → type-check → verify)
   runs before any code is emitted. **No proof, no build.**
3. **Minimal footprint** — no libc, no malloc, no String/Vec (rejected
   by the freestanding target), no drivers beyond the VGA buffer + COM1
   serial hook.

---

## Curlee language reference (how to write Curlee)

> Curlee is a **verification-first** language: it refuses to run/build a
> program unless it can prove your declared contracts within a small,
> decidable scope (SMT solver Z3). The supported fragment is intentionally
> small; out-of-scope constructs are **hard errors**, never silently
> ignored. **Do not weaken or drop a contract to "get it compiling"** —
> that is a hard rule. Unsupported constructs get clear diagnostics.

### CLI

| Command | Meaning |
|---|---|
| `curlee check <file.curlee>` | lex → parse → resolve → type-check → verify (Z3). Fails if any obligation can't be proven or is out of scope. |
| `curlee run <file.curlee>` | Runs `check` first, then executes the verified program on a deterministic, fuel-bounded VM. |
| `curlee build --target freestanding-c -o out.c <file.curlee>` | Verify, then emit freestanding C (no proof, no build). |
| `curlee build --link -o kernel.elf <file.curlee>` | Emit C, compile `-ffreestanding -fno-builtin -nostdlib`, assemble `crt0.S`, link `linker.ld` → bootable multiboot2+PVH kernel ELF. |

The VM never runs freestanding programs — `curlee run` rejects `Phys`
and `extern` bodies. `curlee build` is the freestanding execution path.

### Types

- `Int` — integer (maps to `int64_t` in freestanding C codegen)
- `Bool` — boolean (maps to `_Bool`)
- `Unit` — like void; functions return `return;`
- `U8` / `U16` / `U32` / `U64` — unsigned fixed-width (freestanding only;
  maps to `uintN_t`)
- `String`, `Vec` — hosted-only builtins (rejected in freestanding target)
- `Phys<T>` — raw physical memory pointer (freestanding only)
- `enum` — tagged unions with `variant_is` / `variant_unwrap`
- `struct` — records
- Capabilities — `cap <name>` parameter types (see below)

### Function declarations

```curlee
fn name(a: Int, b: Int) -> Int {
  return a + b;
}

// Void-ish function:
fn serial_hello() -> Unit {
  curlee_putc(72);   // H
  return;
}
```

`main` may return `Int` (exit code; VM) or `Unit` (freestanding kernel
entry; codegen exports it as `curlee_main`).

### Contracts (`requires` / `ensures`) and refinements (`where`)

```curlee
fn vga_cell(ch: Int, attr: Int) -> Int
  [ requires ch >= 0 && ch < 256; ensures true; ] {
  return ch + attr * 256;
}

// Alternative accepted bracket placement (seen in test fixtures):
fn take_nonzero(x: Int) -> Int [
  requires x != 0;
] {
  return x;
}
```

- Obligations checked: at call sites prove callee `requires` from caller
  facts; at returns prove `ensures`.
- Refinement on a binding: `let x: Int where x > 0 = 5;`
- Verifier is **path-sensitive**: it assumes the `if` condition as a fact
  inside the true branch, and assumes the `while` condition inside the
  loop body.
- Contract logic is Int/Bool only. Out-of-scope predicates are hard errors.

### Control flow

```curlee
if (cond) {
  ...
} else {
  ...
}

while (cond) {
  ...
}
```

- **NO `else if` chains** — use nested `if`/`else` blocks.
- No `for` loops, no `switch`. No ternary.

### Operators (supported fragment)

- Arithmetic: `+` `-` `*` `/` `%` (Euclidean modulo — verified directly
  2026-08-27: `7 % 3` runs, and `%` is provable inside `ensures`
  contracts via Z3's `Z3_OP_MOD`. It is NOT unsupported/freestanding-only
  — this doc previously said otherwise and was stale.)
- Comparison: `==` `!=` `<` `>` `<=` `>=`
- Logic: `&&` `||` `!`
- **NO shifts, NO bitwise ops (`& | ^ ~`)** — confirmed absent from
  curlee's lexer/parser as of this writing. Verify against
  `~/Projects/curlee/src/lexer/` or a throwaway `curlee run` probe if
  this matters for what you're doing, rather than trusting this doc.

### Literals

- Decimal integers are the norm: `753664`, `23`, `0`, `72`.
- Hex literals like `0xB8000` are **ONLY valid inside `phys<T>(...)`**
  — normal `Int` expressions must use decimal.
- Hex addresses may use `_` separators: `0xFD00_0000`.
- String literals: `"x"`.

### Physical memory (`Phys<T>`) — freestanding / kernel

```curlee
fn main(pm: cap phys.mem) -> Unit {
  unsafe {
    let fb: Phys<U32> = phys<U32>(0xFD00_0000); // framebuffer base
    fb.write(0xFF8800);                          // pixel write
    let v: U32 = fb.read();                      // register/framebuffer read
  }
  return;
}
```

Rules (enforced at type-check AND verifier — defense in depth):
- The `phys<T>(...)` address argument **MUST be a compile-time integer
  literal** (decimal or hex with `_` separators). Any computed expression
  → hard error: "physical address must be a constant literal".
- `read()`/`write()` are only allowed **inside `unsafe { }` blocks** AND
  the enclosing function must take a **`cap phys.mem`** parameter.
- Verifier semantics: `Phys<T>` is an opaque value; `read`/`write` are
  uninterpreted Z3 functions with **no axioms** → no out-of-bounds
  obligation is generated (documented, trusted access). Data read from
  MMIO is symbolic — contracts cannot see through it. The *address* is
  constrained, the *access* is trusted, the *data* is opaque.

### `extern fn` — external linkage

```curlee
extern fn curlee_putc(c: Int) -> Unit;
extern fn curlee_halt() -> Unit;

// Optional assumed contracts (diagnostic note: "extern boundary: contract assumed"):
extern fn boot_setup() -> Unit
  [ requires true; ensures true; ];
```

- No body. Optional `requires`/`ensures` are **assumed** (trusted boundary).
- Codegen emits `extern` declarations + direct calls; symbol resolved at
  link time. Non-extern fns get a `curlee_` prefix (`curlee_main`, etc.).

### Imports / modules

```curlee
import std.io as io;
import mymod.math as m;   // relative module (dir/module pairs)

fn main(input: cap io.stdin) -> String {
  return io.read_line(input);
}
```

- Module-qualified calls work both via alias and full path:
  `m.add1(41) + mymod.math.add1(-1)`.
- Stdlib lives at `~/Projects/curlee/stdlib/v1/std/`:
  `io` (stdin/stdout/tty), `fs` (read/write), `math`, `rng`, `tty`, `vec`,
  `python` (stubbed).

### Capabilities (no ambient authority)

Capabilities are passed as explicit parameter types and flow through
call chains — there is no ambient authority:

| Capability | Purpose |
|---|---|
| `cap io.stdout` | print / host output |
| `cap io.stdin` | read line |
| `cap io.tty` | `__tty_clear`, `__tty_write_at`, `__tty_flush` |
| `cap fs.read` / `cap fs.write` | file access |
| `cap rng.seeded` | seeded random |
| `cap python.ffi` | python interop (stubbed) |
| `cap phys.mem` | `Phys<T>` read/write (kernel) |

The `--cap` CLI flag grants capabilities at run time. Functions must
thread capabilities through parameters (linear-style flow).

### Enums

```curlee
enum Maybe {
  Some(Int);
  None;
}

fn main() -> Int {
  let m: Maybe = Maybe::Some(1);
  if (variant_is(m, Maybe::Some)) {
    return variant_unwrap(m, Maybe::Some) + 41;
  }
  return 0;
}
```

### Freestanding target constraints (kernel code)

- **No `print`, no `String`, no `Vec`, no `malloc`** — the freestanding
  target rejects hosted builtins with a clear diagnostic.
- `Int` → `int64_t`, `Bool` → `_Bool`, `U8/16/32/64` → `uintN_t`.
- `main` → `int64_t curlee_main(void)`.
- Phys load/store → `*(volatile uintN_t*)(ADDR)` with the constant embedded.
- No libc at all: kernel drivers (serial, VGA) are provided as C files
  (`putc_driver.c`, `vga_setup.c`, `fb.c`) linked alongside the codegen.

### Comments

- `//` line comments only (no `/* */`).
- Files start with an SPDX header: `// SPDX-License-Identifier: GPL-3.0`.
- Kernel files use rich header comment blocks explaining boot path,
  display strategy, and design laws before the code.

---

## Documentation references (source of truth)

Curlee's docs live in the repo and its GitHub wiki. When unsure about
syntax or scope, read these before guessing:

- **Curlee README:** `~/Projects/curlee/README.md` — overview, build/run,
  freestanding kernel walkthrough, MVP scope.
- **Curlee docs index:** `~/Projects/curlee/docs/README.md`
- **Freestanding kernel plan:** `~/Projects/curlee/plans/freestanding-kernel-support.md`
  — the design doc for `Phys<T>`, `extern fn`, `curlee build --link`,
  `phys.mem` capability, crt0/linker.
- **Curlee issue plans:** `~/Projects/curlee/plans/issues/*.md`
- **Curlee examples:** `~/Projects/curlee/examples/*.curlee` — runnable
  syntax samples (control flow, enums, capabilities, budget gate).
- **Curlee stdlib:** `~/Projects/curlee/stdlib/v1/std/*.curlee`
- **Curlee test fixtures:** `~/Projects/curlee/tests/fixtures/*.curlee` —
  tiny, authoritative examples of contracts, Phys usage, extern,
  imports, structs, refinements. `tests/codegen/*.curlee` shows the
  freestanding codegen surface.
- **Curlee agent rules:** `~/Projects/curlee/.github/copilot-instructions.md`
  — verification-first soundness, formatting policy, workflow.
- **Wiki (remote, source of truth for the supported fragment):**
  - Stability & supported fragment: https://github.com/w4ffl35/curlee/wiki/Stability-and-Supported-Fragment
  - Syntax reference: https://github.com/w4ffl35/curlee/wiki/Language-Syntax
  - Modules & imports: https://github.com/w4ffl35/curlee/wiki/Modules-and-Imports
  - Running programs (fuel, capabilities, interop): https://github.com/w4ffl35/curlee/wiki/Running-Programs

**When in doubt, mirror existing code.** The kernel sources
(`kernel/kernel.curlee`, `kernel/pack.curlee`) and Curlee's own examples/
fixtures are the best living reference for what the compiler accepts.

---

## Host system boundaries

- This project's files live under `~/Projects/joeos`. Keep edits inside
  the workspace. Do not modify `~/Projects/curlee` unless explicitly
  asked (it is the language's repo, used read-only for reference).
- `build/` is generated output — `make clean` removes it.
- The compiler is auto-detected by `scripts/find-curlee.sh`; never hardcode
  a path in the Makefile beyond what's already there.
