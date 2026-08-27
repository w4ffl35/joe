# Security Policy

JOE is a bare-metal, Ring-0, single-address-space kernel — nearly everything
in it runs with full hardware privilege by design. We still take security
reports seriously, especially anything that:

- Defeats Curlee's verification gate for kernel code (a construct that
  should have been rejected at `curlee check` time but wasn't).
- Breaks the `unsafe { }` / `cap phys.mem` boundary around physical memory
  access (e.g., a `Phys<T>` read/write reachable without the capability or
  outside an `unsafe` block).
- Corrupts boot-time state (multiboot2/PVH handoff, GRUB path) in a way an
  attacker controlling boot input could exploit.
- Affects the freestanding C codegen path in a way that introduces memory
  unsafety not present in the Curlee source.

## Supported versions

JOE is pre-alpha. Security fixes land on `master` only — there are no
tagged releases yet, so assume only the latest commit on `master` is
supported.

## Reporting a vulnerability

**Do not open a public issue for a security vulnerability.**

Use the repository's private vulnerability reporting path:

- GitHub: **Security → Report a vulnerability** on this repository
  (https://github.com/w4ffl35/joeos/security/advisories)

Please include:

1. The affected component (boot path, VGA/serial driver, network stack,
   framebuffer, the Curlee-to-C codegen boundary, or the Curlee source
   itself — the latter should also be reported to
   [Curlee's security policy](https://github.com/w4ffl35/curlee/security/policy)).
2. A minimal reproduction (a `.curlee` source snippet, or exact QEMU/VirtualBox
   boot steps) if possible.
3. Impact: what an attacker controlling the described input could do.
4. Environment: QEMU version, VirtualBox version if relevant, host OS,
   Curlee compiler commit/version.

You will receive a response within 5 business days. We ask that you do not
publicly disclose the issue until a fix is released and announced.

## Scope

In scope:

- Memory-safety issues in kernel code reachable without an explicit
  `unsafe` block and `cap phys.mem`.
- Boot-time integrity issues (multiboot2, PVH, GRUB ISO path).
- Curlee-to-freestanding-C codegen bugs that introduce unsafety not present
  in the verified Curlee source.
- Driver-level issues (VirtIO-net, framebuffer, serial) that could be
  triggered by untrusted input over the corresponding interface.

Out of scope (by design, documented):

- Anything requiring physical or hypervisor-level access to the machine
  JOE boots on — JOE assumes Ring-0/single-address-space trust by design
  (see `docs/c-boundary-policy.md` and the kernel `.roo/rules/rules.md`
  design laws).
- Missing hardening that would only matter with an MMU/privilege
  separation model JOE does not currently implement (tracked as future
  roadmap work, not a vulnerability in the current design).
- Issues purely in the Curlee compiler/verifier itself — report those to
  [Curlee's own security policy](https://github.com/w4ffl35/curlee/security/policy)
  instead.
