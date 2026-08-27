# Contributing to JOE

Thanks for contributing! JOE is a minimal x86-64 kernel written entirely in
[Curlee](https://github.com/w4ffl35/curlee), a verification-first language:
**if Curlee can't prove a contract, the kernel doesn't build.** Keep that
principle front and center in every change.

## Project values

- **No proof, no build.** JOE's own Makefile enforces this: `make verify`
  runs the full check → build → boot-smoke pipeline before anything is
  considered done.
- **Zero C in the kernel.** As of the C-to-Curlee migration, `kernel/`
  contains no `.c` files — every driver, data structure, and piece of logic
  is Curlee. New kernel code should stay that way; see
  `docs/c-boundary-policy.md` before adding anything in C.
- **Deterministic verification, not runtime guessing.** Physical memory
  addresses are compile-time literals, contracts are proven by Z3 before
  code is emitted, and `unsafe { }` blocks are the only place raw memory
  access happens.
- **Small, focused changes**, each verified against a real boot (QEMU
  primary, VirtualBox/GRUB secondary), not just a static check.

## Development setup

You need a built [Curlee](https://github.com/w4ffl35/curlee) compiler.
Clone it as a sibling directory and build it:

```bash
git clone https://github.com/w4ffl35/curlee.git ../curlee
cd ../curlee
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ libz3-dev pkg-config
cmake --preset linux-debug
cmake --build --preset linux-debug
```

`scripts/find-curlee.sh` auto-detects the compiler at
`../curlee/build/linux-debug/curlee` (or via `$CURLEE_ROOT` / `$CURLEE` /
`$PATH` — see that script for the full resolution order).

Build and verify JOE:

```bash
make kernel       # -> build/kernel.elf (QEMU path)
make check        # curlee check across the whole kernel (no proof, no build)
make verify        # full static gate suite (check, pack/canvas/json/net-stack
                    # tests, mb2 codegen, C-boundary, kernel build)
make qemu-smoke     # boots kernel.elf under QEMU, asserts the serial banner
make qemu-serial    # primary dev loop — kernel output streams live
```

qemu-system-x86_64 is required for the boot gates. VirtualBox
(`make iso`, `scripts/vbox-setup.sh`) is the secondary boot path and is
optional for most changes.

## Code style

- `//` line comments only (Curlee has no `/* */`).
- Every `.curlee` file starts with an SPDX header:
  `// SPDX-License-Identifier: GPL-3.0`.
- Kernel files carry a header comment block explaining boot path, design
  constraints, and any non-obvious verification tradeoffs before the code.
- No `else if` — nested `if`/`else` only (Curlee has no such construct).
  No `for` loops, no `switch`, no ternary.
- Match the syntax and structure already used in `kernel/*.curlee` and
  Curlee's own wiki (`Language-Syntax`, `Stability-and-Supported-Fragment`)
  over guessing — the compiler is the final authority on what's accepted.

## Tests

- Every module with pure (non-`Phys`) logic should be VM-testable —
  `curlee run` it directly rather than only exercising it through a full
  kernel boot. See `kernel/*_test.curlee` for the existing pattern.
- A change to kernel behavior should be verified against a real boot, not
  just `curlee check` — `make qemu-smoke` / `make qemu-serial` are the
  authoritative gates, not a proxy for them.
- If you add a new verify gate, wire it into `make verify` so CI catches
  regressions automatically.

## Commit and PR process

- Work on a branch off `master`; open a PR against `master`.
- Describe what you verified (which `make` targets you ran, and their
  output) in the PR body — "looks right" is not a substitute for a real
  `make verify` / `make qemu-smoke` pass.
- CI must pass: the full verify + boot-smoke pipeline.

## License

By contributing, you agree that your contributions are licensed under the
GPL-3.0 License (see `LICENSE`). Curlee itself (the language and compiler)
is MIT-licensed and lives in its own separate repository.

## Repository administration

- If this repository is transferred to a different owner/organization,
  update the Curlee repository links in `README.md`, the `SECURITY.md`
  reporting URL, and any hardcoded paths in `scripts/find-curlee.sh`'s
  documentation comment.
