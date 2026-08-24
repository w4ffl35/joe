## Code-mode rules — Curlee formatting styles & quirks

Mode-specific supplement to `.roo/rules/rules.md` (workspace-wide). This
file is about **how to write clean, idiomatic Curlee** — the formatting
conventions, style quirks, and compiler quirks observed across the Curlee
repo, the JOE kernel sources, and Curlee's own examples/fixtures. The
language reference (types, contracts, capabilities, Phys, extern, modules)
lives in `.roo/rules/rules.md` — this file assumes that knowledge and adds
the style layer.

---

## Ground truth: mirror existing code

The Curlee repo is the **living reference** for what the compiler accepts
and how the project formats it. When in doubt, copy the shape of:

- `~/Projects/curlee/examples/*.curlee` — runnable syntax samples
- `~/Projects/curlee/tests/fixtures/*.curlee` — authoritative tiny examples
- `~/Projects/curlee/stdlib/v1/std/*.curlee` — idiomatic module code
- `kernel/kernel.curlee` and `kernel/pack.curlee` in this repo — the JOE
  house style for freestanding kernel code

---

## Indentation & layout

- **2-space indentation** for blocks. No tabs. No 4-space style.
  ```curlee
  fn main() -> Int {
    if (true) {
      return 1;
    } else {
      return 0;
    }
  }
  ```
- **Opening brace `{` goes on the same line** as the signature, the `if`,
  the `else`, and the `while` (K&R / 1TBS style). In the fixtures the
  contract-carrying functions put the body brace on the next line, but the
  kernel and examples keep it inline — see "Contracts" below for the exact
  two accepted shapes.
- `else` stays on its own line (it is never `} else {` in this codebase).
- One statement per line. `return;` is used for `Unit` functions.

## Function signatures

- `fn name(param: Type, ...) -> RetType {` with a space after `fn`, and
  spaces after commas in parameter lists.
- No space before the `(` of the parameter list: `fn foo(a: Int)` not
  `fn foo (a: Int)`.
- `->` has spaces on both sides: `-> Int`.
- Parameter and return type annotations use `: Type` (colon + space).
- No explicit `where` on a `fn` — refinements go on `let` bindings only.

## Contracts (`requires` / `ensures`) — two accepted shapes

The codebase accepts **both** bracket placements. Match whichever the
file you're editing already uses; for new code, prefer shape #1 (used by
`kernel/kernel.curlee` and the README):

```curlee
// Shape 1: contract bracket closes on the same line as the body brace.
fn vga_cell(ch: Int, attr: Int) -> Int
  [ requires ch >= 0 && ch < 256; ensures true; ] {
  return ch + attr * 256;
}

// Shape 2: contract bracket closes, body brace on its own line.
fn take_nonzero(x: Int) -> Int [
  requires x != 0;
] {
  return x;
}
```

- Contract clauses are indented 2 spaces inside the `[ ]`.
- Each clause ends with `;` (including the last one before `]`).
- Order convention: `requires` clauses first, then `ensures`.
- Multiple `requires` clauses go on separate lines rather than being
  joined with `&&` when the author wants each obligation visible:
  ```curlee
  fn add(a: Int, b: Int) -> Int
    [ requires a > 0;
      requires b > 0;
      ensures result > a && result > b; ] {
    return a + b;
  }
  ```
- A `requires` of `true` is written when the contract is only meaningful
  through its `ensures`: `[ requires true; ensures result > 0; ]`.
- Do **not** omit or weaken a contract to make verification pass — that is
  a hard rule from the language reference. If the verifier can't prove an
  obligation, restructure the code (add a refinement, add a path fact)
  rather than deleting the clause.

## Naming conventions

- `fn` names: **lower_snake_case** (`vga_cell`, `serial_hello`,
  `take_nonzero`, `projected_balance`).
- Parameter names: **lower_snake_case**, short and descriptive (`ch`,
  `attr`, `col`, `i`, `x`, `src`, `dst`).
- `let` bindings: **lower_snake_case** (`remaining`, `m`, `reg`).
- Types: **PascalCase** (`Int`, `Bool`, `Unit`, `U16`, `U32`, `Phys<T>`,
  `Maybe`, `Point`, `Token`).
- Enum variants: **PascalCase** (`Maybe::Some`, `Maybe::None`).
- Extern functions and builtins that must keep their exact symbol name:
  `curlee_putc`, `curlee_halt`, `fb_init`, `fb_ready`, `fb_draw_char`,
  `vga_text_setup` — these are 1:1 identifiers at link time.
- Non-extern Curlee functions are mangled to `curlee_<name>` at codegen —
  you never write that prefix yourself.
- No Hungarian notation, no `m_`/`p_` prefixes, no trailing underscores.

## Comments

- **`//` line comments only.** `/* */` block comments are not valid Curlee.
- **Every `.curlee` file starts with an SPDX header:**
  `// SPDX-License-Identifier: GPL-3.0`
- Kernel files carry a rich header block after the SPDX line explaining the
  boot path, display strategy, and which design laws are honored (see
  `kernel/kernel.curlee` lines 1–23 for the house style).
- Section comments use a `//` line followed by a blank line, like this:
  ```curlee
  // Pack (char, attribute) into the 2-byte VGA cell value:
  //   low byte  = ASCII character
  //   high byte = attribute (foreground | background << 4)
  fn vga_cell(...)
  ```
- ASCII-code magic numbers in drivers are annotated inline with the char:
  `curlee_putc(72);   // H`
- Comments explain *why* (boot strategy, design laws, verifier facts), not
  *what* the code obviously does.

## Control flow quirks

- **No `else if` chains.** Use nested `if`/`else` blocks, each level
  indented 2 more spaces. `kernel/pack.curlee`'s `hello_char_at` is the
  canonical deep-nesting example — 23 levels of nested `if (i == N)`
  inside `else` blocks.
- **No `for`, no `switch`, no ternary** — only `if`/`else` and `while`.
- `if (cond)` and `while (cond)` always parenthesize the condition, with a
  space between the keyword and `(`.
- Inside the true branch of an `if` you may `return` early; the trailing
  `return 0;` (or `return;`) after the `if` block is the common fallthrough
  pattern.

## Operators & literals quirks

- Space around binary operators: `ch + attr * 256`, `x > 0`, `a && b`.
  No spaces inside parentheses.
- **No shifts, no `%`, no bitwise ops (`& | ^ ~`)** — the compiler rejects
  them. Work with `*`, `/`, `+`, `-` and comparisons instead.
- **Hex literals (`0xB8000`, `0xFD00_0000`) are ONLY valid inside
  `phys<T>(...)`.** Everywhere else, use decimal (`753664`, `15`, `72`).
- Hex addresses inside `phys<...>` may use `_` separators for readability:
  `phys<U32>(0xFD00_0000)`, `phys<U16>(0xB8000)`.
- The address passed to `phys<T>(...)` must be a **constant literal** —
  never a computed expression, never a variable. This is enforced by the
  verifier as a hard error.
- Use `_` in long decimal constants sparingly; the kernel prefers plain
  decimals (`753664`) and comments the hex equivalent.

## `let` bindings & refinements

- Bindings are `let name: Type = expr;` — the type annotation is written
  even when it could be inferred (the examples and kernel always annotate).
- Refinement: `let x: Int where x > 0 = 5;` — note `where` goes between
  the type and the `=`.
- Refinements are how you give the verifier facts (e.g., proving a
  `requires` at a call site): `let v: U32 where v > 0 = reg.read();`

## `Phys<T>` style

- Declare at the top of the `unsafe` block, one per address, each with a
  comment naming what it points at:
  ```curlee
  unsafe {
    // Row 0, columns 0..22 (2 bytes per cell, base 0xB8000).
    let c0: Phys<U16> = phys<U16>(0xB8000);
    c0.write(vga_cell(72, 15));   // H
    ...
  }
  ```
- Group related pointer declarations together, then the reads/writes.
- `read()`/`write()` calls are `ptr.read()` / `ptr.write(value)` — method
  syntax with a dot, no space.
- Never put `read()`/`write()` outside `unsafe { }`, and never in a
  function that lacks `cap phys.mem`.

## `unsafe` blocks

- `unsafe {` on its own line, body indented 2 spaces, `}` on its own line.
- Only `Phys<T>` access and gated host calls (`python_ffi.call`) go inside
  `unsafe`. Pure logic stays outside.
- Keep `unsafe` blocks as small as the code allows — the kernel's `main`
  puts the whole VGA render in one block, but prefers minimal raw access
  where a pure helper exists (e.g., `vga_cell` is a pure fn called from
  inside the block).

## `extern fn` style

- Declared at the top of the file, before any Curlee function, each
  followed by `;`:
  ```curlee
  extern fn curlee_putc(c: Int) -> Unit;
  extern fn curlee_halt() -> Unit;
  ```
- Group them by provider with a `//` comment (e.g., "Provided by
  kernel/fb.c:", "Provided by kernel/vga_setup.c:").
- Only add assumed contracts (`[ requires true; ensures true; ];`) when the
  boundary genuinely needs them — the compiler emits a "extern boundary:
  contract assumed" note either way.

## Imports

- `import` statements go at the top of the file, after the SPDX header and
  header comments, before any `fn`.
- Alias form is preferred: `import std.io as io;`
- Module-qualified calls use the alias: `io.read_line(input)`.
- Blank line after the import block before the first declaration.

## Structs & enums

- `struct` fields: `name : Type;` with spaces around the colon, field name
  in lower_snake_case, one field per line, closing brace on its own line.
  This differs from `fn` param syntax (`a: Int` — no space before `:`).
- Struct construction: `Point{x : 1, y : 2}` — named fields with ` : `.
- Field access: `p.x + p.y` (dot, no space).
- Enums: variants PascalCase, each ending with `;`; payloads in parens:
  ```curlee
  enum Maybe {
    Some(Int);
    None;
  }
  ```
- Enum construction: `Maybe::Some(1)` (double-colon, no spaces).
- Inspection: `variant_is(m, Maybe::Some)` and `variant_unwrap(m, Maybe::Some)`.

## Capability style

- `cap` parameters are placed like any other parameter: `fn main(pm: cap phys.mem) -> Unit`
- Capabilities are threaded explicitly through call chains — pass the cap
  as an argument at every hop; never stash it in a global.
- The parameter name for the physical-memory cap in the kernel is `pm`.
- Run-time grants use `--cap phys.mem` etc.; that's a CLI concern, not
  something expressed in source.

## Unit functions

- `Unit` return type means the body ends with a bare `return;`:
  ```curlee
  fn serial_hello() -> Unit {
    curlee_putc(72);
    return;
  }
  ```
- Even when the last statement is a call, still write the explicit
  `return;` — the codebase never relies on implicit fallthrough.

## Code layout in files (house order)

1. SPDX header (`// SPDX-License-Identifier: GPL-3.0`)
2. Header comment block (purpose, boot path, design laws) — kernel files
3. `extern fn` declarations (grouped by provider, with `//` comments)
4. `import` statements
5. Pure helper functions (contracts where useful)
6. Driver/render functions
7. `main` last

## Verification-friendly habits

- Write pure `Int -> Int` / `Int -> Bool` helpers with explicit contracts
  so the verifier can prove call-site obligations (`vga_cell`, `valid_col`).
- When a helper needs a precondition, declare it with `requires` and give
  the caller the fact via a refinement or an `if` guard — the verifier is
  path-sensitive and picks up branch conditions as facts.
- Never read MMIO and try to reason about the value in a contract — Phys
  reads are opaque/symbolic to the verifier. Keep contracts on the *pure*
  data flow only.
- Total functions (every input maps to an output) verify more easily; make
  helpers total with a sensible fallback (`hello_char_at` returns `0` for
  out-of-range indices).

## What NOT to do (rejected constructs)

- `else if`, `for`, `switch`, ternary `?:`, `&&=` style compound ops
- `%`, shifts, `& | ^ ~` bitwise ops
- `/* */` block comments, doc-comments, `#` comments
- Hex literals outside `phys<...>()`
- `String`/`Vec`/`print`/`malloc` in freestanding kernel code
- `phys<T>(...)` with a computed/non-literal address
- `read()`/`write()` outside `unsafe` or without `cap phys.mem`
- Dropping or weakening a `requires`/`ensures` clause to "make it compile"

---

## Verification workflow (code mode)

```sh
make check      # curlee check kernel.curlee + pack.curlee — must pass before build
make pack-run   # curlee run pack.curlee — VM executes the verified packer
make kernel     # codegen -> freestanding C -> cc/ld -> build/kernel.elf
make verify     # all static gates (check, packer, ELF entry, _start, curlee_main, PVH)
make qemu-smoke # the acceptance gate: boot + serial log assert
```

- Run `make check` before touching the build pipeline — the compiler's
  diagnostics are the fastest feedback on syntax/scope mistakes.
- If a new `.curlee` file is added to the kernel build, wire it into the
  Makefile and add it to the `check` target.
