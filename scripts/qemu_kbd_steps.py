# SPDX-License-Identifier: GPL-3.0
"""qemu_kbd_steps.py — the steps of the keyboard gate (qemu_kbd_smoke.py).

A step is a batch of key events sent to QEMU and the serial lines apps/keys
must print in answer, in order, and no others:

  KEYS: <hhh>   the held keys changed; hhh is kbd_state() in hex:
                001 up, 002 down, 004 left, 008 right, 010 Z, 020 X,
                040 C, 080 Enter, 100 Escape
  TAP: <hhh>    keys were pressed and released within one poll

QEMU turns each qcode into the bytes a PC keyboard sends in scancode set 1:
`up` is 0xE0 0x48, the keypad's `kp_8` is a plain 0x48, `kp_enter` is
0xE0 0x1C and `volumedown` is 0xE0 0x2E, which a driver that ignored the
prefix would read as C. QEMU's keyboard queues at most 16 bytes, which is
what the burst steps fill.
"""

from __future__ import annotations

from dataclasses import dataclass

HALT = "Hello World from JOE!"
Event = tuple[str, bool]


@dataclass(frozen=True)
class Step:
    """Events sent as one batch (or, with hold_ms, one key pressed by
    `send-key` and released after that many ms), and the serial lines
    that must follow, in order, with no others."""
    name: str
    events: list[Event]
    expect: list[str]
    hold_ms: int | None = None


def keys_line(value: int) -> str:
    return f"KEYS: {value:03x}"


def tap_line(value: int) -> str:
    return f"TAP: {value:03x}"


def down(*keys: str) -> list[Event]:
    return [(key, True) for key in keys]


def up(*keys: str) -> list[Event]:
    return [(key, False) for key in keys]


def tap(*keys: str) -> list[Event]:
    return [event for key in keys for event in down(key) + up(key)]


def press_release(name: str, key: str, value: int) -> list[Step]:
    """`key` down sets `value` in the state and keeps it set; up clears."""
    return [Step(f"{name} down", down(key), [keys_line(value)]),
            Step(f"{name} up", up(key), [keys_line(0)])]


def hold_steps() -> list[Step]:
    """A held key, two keys in turn and in one batch, send-key's hold."""
    return press_release("Z held", "z", 0x010) + [
        Step("Z down", down("z"), [keys_line(0x010)]),
        Step("X down (two keys)", down("x"), [keys_line(0x030)]),
        Step("Z up", up("z"), [keys_line(0x020)]),
        Step("X up", up("x"), [keys_line(0)]),
        Step("Z and X in one batch", down("z", "x"), [keys_line(0x030)]),
        Step("Z and X released", up("z", "x"), [keys_line(0)]),
        Step("C by send-key, hold 300 ms", [("c", True)],
             [keys_line(0x040), keys_line(0)], hold_ms=300),
    ]


def arrow_steps() -> list[Step]:
    """The arrows as sent by a PC keyboard (0xE0 prefix), and the keypad
    digits, which send the same codes without it."""
    arrows = [("up", "kp_8", 0x001), ("down", "kp_2", 0x002),
              ("left", "kp_4", 0x004), ("right", "kp_6", 0x008)]
    steps: list[Step] = []
    for name, keypad, value in arrows:
        steps += press_release(f"{name} (0xE0 prefix)", name, value)
        steps += press_release(f"{keypad} (no prefix)", keypad, value)
    return steps


def other_key_steps() -> list[Step]:
    """Enter and Escape, keypad Enter (0xE0 0x1C), and Volume Down
    (0xE0 0x2E), which must not read as C (0x2E): the state stays 0."""
    return press_release("Enter", "ret", 0x080) \
        + press_release("keypad Enter", "kp_enter", 0x080) \
        + press_release("Escape", "esc", 0x100) + [
            Step("Volume Down down", down("volumedown"), []),
            Step("Volume Down up", up("volumedown"), []),
            Step("C after it", down("c"), [keys_line(0x040)]),
            Step("C released", up("c"), [keys_line(0)]),
        ]


def tap_steps() -> list[Step]:
    """Presses that end before the guest polls: the state never shows
    them, the tap latch does, and no key is left held. The first burst is
    16 bytes, all the controller can queue, and one poll reads them all."""
    return [
        Step("Z tapped within one poll", tap("z"), [tap_line(0x010)]),
        Step("arrows tapped, 16 bytes queued",
             tap("up", "down", "left", "right"), [tap_line(0x00f)]),
        Step("six keys tapped, 14 bytes queued",
             tap("up", "z", "x", "c", "ret", "esc"), [tap_line(0x1f1)]),
        Step("Z still works after them", down("z"), [keys_line(0x010)]),
        Step("Z released", up("z"), [keys_line(0)]),
    ]


def quit_steps() -> list[Step]:
    """Z, X and C together end the app; the kernel prints its halt line."""
    return [Step("Z, X and C together", down("z", "x", "c"),
                 [keys_line(0x070), HALT])]


def all_steps() -> list[Step]:
    return (hold_steps() + arrow_steps() + other_key_steps()
            + tap_steps() + quit_steps())
