# RTL8198C Bootloader NAND-Boot Analysis

## Summary

The Askey AP5100W's bootloader supports NAND-booting (confirmed by serial
observation of a factory kernel booting from the `linux` partition), but
our OpenWrt kernel image — despite having a correctly formatted Realtek
image header, valid checksum, and being successfully recognized by the
bootloader — fails to boot.  This document captures the reverse-engineering
findings and explains why this blocks a self-contained, flashable OpenWrt
firmware.

## Bootloader overview

- **Type**: Proprietary Realtek RTL8198C boot ROM
- **Source reference**: `bootcode_rtl8198c_20141121.tar.gz` (may not match
  the actual binary on the device)
- **Flash layout**: 128 MiB Winbond W29N01HV parallel NAND, 8 `fixed-partitions`
  defined in DTS:

| Name      | NAND offset | Size   | Purpose              |
|-----------|-------------|--------|----------------------|
| `boot`    | 0x00000000  | 5 MiB  | Bootloader (RO)      |
| `setting` | 0x00500000  | 3 MiB  | Factory config       |
| `linux`   | 0x00800000  | 6 MiB  | Kernel slot A        |
| `rootfs`  | 0x00E00000  | 24 MiB | Rootfs slot A        |
| `linux2`  | 0x02600000  | 6 MiB  | Kernel slot B        |
| `rootfs2` | 0x02C00000  | 24 MiB | Rootfs slot B        |
| `data`    | 0x04400000  | 38 MiB | Persistent data      |
| `nvdata`  | 0x06A00000  | 2 MiB  | NVRAM                |

- **A/B dual-boot**: Slots A (`linux` + `rootfs`) and B (`linux2` +
  `rootfs2`) provide failover.  The bootloader scans both banks, copies a
  valid image from one bank to the other if one is corrupted, and
  prefers the `cr6c` (kernel+rootfs) image type over `cs6c` (kernel-only).

## Image format

The bootloader expects a **Realtek IMG_HEADER** at the start of each NAND
block boundary (every 128 KiB) within the kernel partition:

```
Offset  Size  Field       Description
------  ----  -----       -----------
 0       4    signature   "cs6c" (kernel only) or "cr6c" (kernel + rootfs)
 4       4    startAddr   RAM address to copy the image to and jump to
 8       4    burnAddr    NAND offset where image should reside (8 MiB)
12       4    len         Payload length in bytes (includes 2-byte checksum)
```

All multi-byte fields are **big-endian** (MIPS native).

After the 16-byte header comes the **payload**: the kernel image data
followed by a 2-byte 16-bit word checksum that makes the sum of all
16-bit words in the payload equal to zero.

## What the bootloader does at power-on

1. **NAND init** — prints `Nand booting..` and scans the NAND chip
2. **Bank scan** — calls `check_image_header()` for bank A (offset 0),
   which scans every 128 KiB block looking for a valid `cs6c` or `cr6c`
   signature.  A successful find sets `ret = 1` (cs6c) or `ret = 2` (cr6c).
3. **Checksum** — if `NEED_CHKSUM` is enabled, computes and validates the
   payload checksum.  **If checksum is OK, copies the payload to
   `startAddr` via `memcpy`.**  If `NEED_CHKSUM` is disabled, the copy
   does NOT happen.
4. **Dual-bank** — if bank A fails (`ret == 0`), checks bank B.  Copies
   bank B to bank A (or bank A to bank B) to keep the backup slot healthy.
5. **Boot** — calls `goToLocalStartMode()` which jumps to `startAddr`.

## Current status

Our OpenWrt kernel image is built with this pipeline:

```
kernel-bin → append-dtb → rt-compress → rt-loader-rtl8198c → rtk-nand-header
```

The `rtk-nand-header` step (Python script:
`target/linux/realtek/image/rtk-nand-header.py`) prepends a proper
Realtek header with signature `cr6c`, `startAddr = 0x80C00000`,
`burnAddr = 0x00800000`, and a valid zero-checksum.

When written to the `linux` partition (`mtd write … linux`), the
bootloader **successfully recognizes** the image:

```
Nand booting..
Scanning NAND Bank2 is corrct!
ret=2  ------> line 315!       <-- our kernel found, signature accepted
no sys signature at 00820000!   <-- scanning remaining blocks
...
```

However, after scanning both banks, the bootloader **hangs** — it never
prints `Jump to image` and the kernel never executes.

## Root cause hypothesis

In the bootloader source code (see `utility.c`), the critical `memcpy`
that moves kernel data from NAND to RAM at `startAddr` is guarded by
`#if defined(NEED_CHKSUM)`:

```c
// check_system_image, utility.c
ret = 1;  // "cs6c" found
#if defined(NEED_CHKSUM)
    if (ret) {
        for (...) sum += ...;   // checksum loop
        if (sum) ret = 0;       // checksum fail → reject
        else {                  // checksum OK:
            #ifdef CONFIG_NAND_FLASH_BOOTING
            memcpy((void *)pHeader->startAddr,
                   ptr_data + sizeof(IMG_HEADER_T),
                   pHeader->len);
            #endif
        }
    }
#endif
```

And the boot function (`goToLocalStartMode`):

```c
// goToLocalStartMode
// For NAND boot, the NOR flashread is skipped (#ifndef CONFIG_NAND_FLASH_BOOTING)
prom_printf("Jump to image start=0x%x...\n", pheader->startAddr);
jump = (void *)(pheader->startAddr);
flush_cache();
jump();      // crash if data was never copied
```

**If `NEED_CHKSUM` is not defined in the binary**, the `memcpy` never
executes.  The bootloader jumps to `startAddr` (0x80C00000) where only
uninitialized RAM exists → instant hang with no error message.

The factory kernel boots fine because the factory bootloader binary was
compiled with `NEED_CHKSUM` enabled.  Our extracted binary (`bootloader/
boot.bin`, 50 KiB of code in a 5 MiB partition) may have `NEED_CHKSUM`
disabled, or the checksum passes but another check fails silently.

## Dual-bank A/B complications

The bootloader's dual-bank mechanism adds complexity:

- When bank A is valid but bank B is corrupted (e.g., after manual
  `mtd erase linux2`), the bootloader tries to **restore** bank B from
  bank A.  This restore copies the entire 30 MiB bank (kernel + rootfs)
  but **silently fails** — bank B ends up with all-zeros or unchanged.
- The restore loop repeats indefinitely until the bootloader gives up.
- If bank B has a valid factory image (`ret = 2` with `cr6c`) while bank
  A has our OpenWrt image, the bootloader **prefers bank B** and copies
  it over bank A, overwriting our kernel.

This makes iterative testing difficult: every test cycle requires
TFTP-booting initramfs, erasing bank B, and re-flashing bank A.

## What works

| Component                                   | Status      |
|---------------------------------------------|-------------|
| NAND flash driver (read / write / erase)    | Working     |
| ONFI chip auto-detection (W29N01HV)         | Working     |
| Image header format (BE `cr6c` + checksum)  | Verified    |
| Bootloader finds and accepts our kernel     | Confirmed   |
| TFTP-booted initramfs (development)         | Working     |

## What's blocked

| Goal                              | Reason                              |
|-----------------------------------|-------------------------------------|
| Self-contained NAND-bootable      | Bootloader hangs after image check  |
| `root=` from NAND (squashfs)      | Kernel never starts from NAND       |
| Persistent overlay on `data`      | Requires NAND-booted squashfs root  |
| Sysupgrade (flash from running)   | No running system on NAND root      |
| Factory image for production      | No NAND boot path functional        |

## Path forward

1. **Deep binary analysis** — set up Ghidra with the correct MIPS base
   address (`0xBFC00000`) and decompile the `check_image` and
   `doBooting`/`goToLocalStartMode` functions to determine exactly
   where execution goes and whether `NEED_CHKSUM` is active.

2. **Alternative: bypass bootloader** — use a chain-loading approach:
   flash a tiny secondary bootloader or a known-good decompressor stub
   at `startAddr` that GZIP-decompresses the kernel and jumps to it.

3. **Alternative: rebuild the bootloader** — the source reference
   (`bootcode_rtl8198c_20141121.tar.gz`) could be compiled with
   `NEED_CHKSUM` and `CONFIG_NAND_FLASH_BOOTING` enabled, then flashed
   to the `boot` partition.  High risk but definitive.

## References

- Bootloader source: `bootcode_rtl8198c_20141121.tar.gz`
- Extracted binary: `bootloader/boot.bin` (5,242,880 bytes, first
  ~50 KiB are actual code)
- Image header script: `target/linux/realtek/image/rtk-nand-header.py`
- Device recipe: `target/linux/realtek/image/rtl8198c.mk`
- NAND driver: `target/linux/realtek/files-6.18/drivers/mtd/nand/raw/
  rtl8198c_nand.c`
- DTS changes: `target/linux/realtek/dts/rtl8198c.dtsi` (bootargs)
