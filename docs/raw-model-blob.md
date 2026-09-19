# Raw model blob disk contract

JOE's first native-inference storage path deliberately needs no filesystem.
An opaque model or runtime blob is placed at a caller-selected logical block
address (LBA) in a pre-sized raw disk image. The kernel and build invocation
must agree on that LBA and the blob's byte length.

Create a raw image, place a blob at LBA 2048, and verify it independently:

```bash
truncate -s 64M build/model-disk.raw
scripts/place-raw-blob.sh build/model-disk.raw path/to/model.bin 2048
scripts/place-raw-blob.sh --verify-only build/model-disk.raw path/to/model.bin 2048
```

The default logical sector size is 512 bytes. A different positive sector
size can be passed as the fourth positional argument. Placement fails before
writing if the blob would exceed the existing image; the script never grows
the image and never creates filesystem metadata.

Attach the result to QEMU as a raw VirtIO block device:

```bash
qemu-system-x86_64 \
  -drive if=none,file=build/model-disk.raw,format=raw,id=modeldisk \
  -device virtio-blk-pci,drive=modeldisk
```

The read-only kernel path is implemented in the small `virtio_blk_*` Curlee
modules. It supports one synchronous request of up to eight sectors, validates
the request against device capacity, and exposes bytes only after the VirtIO
status byte reports success. `make qemu-blk-smoke` proves a two-sector read
across the sector boundary on a real emulated legacy VirtIO device.

A versioned blob header, content digest, and runtime-specific validation still
belong above this raw transport before native inference consumes model bytes.
