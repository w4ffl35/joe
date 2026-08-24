#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0
#
# vbox-setup.sh — create/configure/start a VirtualBox VM that boots the JOE
# kernel ISO (Phase 1: "Hello world from JOE").
#
# Usage:
#   bash scripts/vbox-setup.sh [--iso <path>] [--name <vm>] [--headless]
#
# Defaults:
#   ISO  = build/joeos.iso (build it first with `make iso`)
#   NAME = JOE
#   Headless? off (opens the GUI window) unless --headless
#
# Requires: VBoxManage (VirtualBox CLI). The VM is created with:
#   - 64-bit Linux/Other guest, 64 MiB RAM, 1 vCPU
#   - Boot order: DVD first (the ISO), then nothing else
#   - One COM1 serial port -> a host file (build/vbox-serial.log) so the
#     kernel's serial output ("Hello World from JOE!") is captured
#   - No disk, no network (minimal, matches the kernel's scope)
set -euo pipefail

ISO="build/joeos.iso"
NAME="JOE"
HEADLESS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        --headless) HEADLESS="1"; shift ;;
        -h|--help)
            echo "Usage: $0 [--iso <path>] [--name <vm>] [--headless]"
            exit 0 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

if ! command -v VBoxManage >/dev/null 2>&1; then
    echo "error: VBoxManage not found (is VirtualBox installed?)" >&2
    exit 1
fi

if [[ ! -f "$ISO" ]]; then
    echo "error: ISO not found: $ISO (run 'make iso' first)" >&2
    exit 1
fi

ISO_ABS="$(realpath "$ISO")"

log() { printf "%s\n" "$*"; }

if VBoxManage showvminfo "$NAME" >/dev/null 2>&1; then
    log "VM '$NAME' already exists; unregistering it first (keep disk: none)."
    VBoxManage unregistervm "$NAME" --delete 2>/dev/null || true
fi

log "Creating VM '$NAME' (64-bit, 64 MiB RAM, 1 vCPU)..."
VBoxManage createvm --name "$NAME" --ostype "Linux_64" --register

log "Configuring hardware..."
VBoxManage modifyvm "$NAME" \
    --memory 64 \
    --cpus 1 \
    --firmware bios \
    --graphicscontroller vmsvga \
    --vram 16 \
    --nic1 none \
    --audio none \
    --usb off

log "Configuring boot order (DVD first)..."
VBoxManage modifyvm "$NAME" --boot1 dvd --boot2 none --boot3 none --boot4 none

log "Attaching ISO: $ISO_ABS"
VBoxManage storagectl "$NAME" --name "IDE" --add ide --controller PIIX4
VBoxManage storageattach "$NAME" --storagectl "IDE" --port 0 --device 0 \
    --type dvddrive --medium "$ISO_ABS"

# COM1 serial -> host log file (kernel curlee_putc writes here).
SERIAL_LOG="$(realpath "$(dirname "$ISO")/vbox-serial.log")"
log "Enabling serial port COM1 -> $SERIAL_LOG"
VBoxManage modifyvm "$NAME" \
    --uart1 0x3F8 4 \
    --uartmode1 file "$SERIAL_LOG"

log "VM '$NAME' ready."
log "ISO:        $ISO_ABS"
log "Serial log: $SERIAL_LOG"

start_vm() {
    if [[ -n "$HEADLESS" ]]; then
        VBoxManage startvm "$NAME" --type headless
    else
        VBoxManage startvm "$NAME"
    fi
}

# VirtualBox needs the hardware virtualization extension (AMD-V/VMX), which
# the host's KVM kernel module also holds. If KVM is loaded, start fails with
# VERR_SVM_IN_USE / VERR_VMX_IN_USE. We then temporarily unload KVM, retry,
# and reload KVM afterward so the host's QEMU/KVM workflow is undisturbed.
kvm_was_loaded=""
handle_kvm_conflict() {
    if lsmod 2>/dev/null | grep -q '^kvm'; then
        kvm_was_loaded="1"
        log "KVM module holds the virtualization extension; unloading KVM temporarily..."
        sudo modprobe -r kvm_amd 2>/dev/null || sudo modprobe -r kvm_intel 2>/dev/null || true
        sudo modprobe -r kvm 2>/dev/null || true
    fi
}

restore_kvm() {
    if [[ -n "$kvm_was_loaded" ]]; then
        log "Reloading KVM..."
        sudo modprobe kvm_amd 2>/dev/null || sudo modprobe kvm_intel 2>/dev/null || true
    fi
}

if start_vm 2>&1 | tee /tmp/vbox-start.log; then
    restore_kvm
    log "Done. Serial output will appear in $SERIAL_LOG"
    exit 0
fi

if grep -qE 'VERR_(SVM|VMX)_IN_USE' /tmp/vbox-start.log; then
    handle_kvm_conflict
    log "Retrying start..."
    if start_vm 2>&1 | tee /tmp/vbox-start.log; then
        restore_kvm
        log "Done (after unloading KVM). Serial output will appear in $SERIAL_LOG"
        exit 0
    fi
    restore_kvm
fi

log "Failed to start VM. See log above." >&2
exit 1
