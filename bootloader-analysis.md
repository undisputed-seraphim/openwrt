# RTL8198C Bootloader NAND-Boot Analysis

Updated: 2026-10-30 (resolved)
Status: solved. The bootloader accepts `mtd`-written images, OpenWrt boots
from NAND, and the JFFS2 overlay persists across powercycles.
Scope: Askey AP5100W, Winbond W29N01HV (128 MiB, 2 KiB pages, 128 KiB
blocks), boot code v1.6a dated 2018.02.01. The bootloader itself is never
written; all experiments touch only `linux`, `rootfs`, `linux2`, `rootfs2`
and optionally `data`. TFTP recovery remains available.

## Resolution

- **The bank-A rejection was the BBI/OOB swap.** The bootloader moves one
  byte per page between `data[2000]` (`DATA_BBI_OFF`) and the OOB tag at
  offset 53 (`BBI_SWAP_OFFSET`). Our driver read/wrote OOB offset 23 and
  only swapped on reads. With `OOB_BBI_OFF = 53` and the reverse swap in
  `write_page`, a fixed-driver read of the vendor `mtd2`/`mtd5` images
  produces the bootloader 16-bit checksum `0x0000` (previously `0x8c00`).
  A zero-checksum `cs6c` `rt-loader` image written through `mtd` is then
  accepted: the bootloader prints `Jump to image start=0x80c00000...`
  with no `ret=2`, no `no sys signature` scan and no bank-B copy.
- **LBC 128 is required but not sufficient.** It matches the bootloader's
  `NADCRR` setup and was committed as `803ba32736`; the BBI swap was the
  remaining difference.
- **Use `cs6c` for the final kernel, not `cr6c`.** The `cr6c` signature
  makes the bootloader check the rootfs. With the vendor rootfs the check
  passes; with the OpenWrt XZ squashfs the bootloader did not reach
  `Jump to image start` within 150 s (it appears to scan/validate the
  rootfs). The sysupgrade kernel is built as `cs6c`, so the bootloader
  does not check the rootfs and the kernel mounts it.
- **The vendor rootfs dump is now valid.** With the fixed read,
  `unsquashfs -s` accepts `mtd3` (squashfs 4.0, little endian, zlib) and
  the old `unsquashfs` failure is explained by the wrong OOB byte.
- **JFFS2 needed one target patch.** The controller has no
  spare-area-only DMA mode, so JFFS2's OOB cleanmarker write also
  programs the first data page. A later data node written to page 0 is a
  second program and its ECC cannot be written reliably. The target patch
  `901-jffs2-reserve-cleanmarker-page.patch` reserves page 0 of each
  eraseblock (accounted as wasted), extends the empty-block scan past
  that page, and relaxes the first-node check. Data nodes start at page 1
  and the overlay survives remounts and powercycles.
- **Bad blocks 300-302** (global NAND blocks, inside `rootfs_data`) were
  marked bad while isolating the JFFS2 failure. They are skipped by the
  kernel BBT and cost about 384 KiB of the 19.5 MiB overlay. They may not
  be genuinely defective; the marking was done with `MEMSETBADBLOCK`.
- **Bank B was restored.** A monitor `NANDBE 1280000 25E0000` erased more
  than intended because `NANDBE` takes a **start offset and a length**,
  not a start and end offset. `linux2` and `rootfs2` were restored from
  `vendor-linux2-fixed.bin` (6 MiB, `cr6c`, checksum `0x0000`) and
  `vendor-rootfs-fixed.bin` (24 MiB, valid squashfs), both read through
  the fixed driver and verified by SHA-256. Bank A is OpenWrt; bank B is
  the vendor fallback.
- **Final image**: kernel `cs6c`, 3,268,818 bytes, checksum `0x0000`;
  rootfs 5,093,376 bytes (XZ squashfs); `wpad-basic-mbedtls` included.
  A real `sysupgrade -n` with the sysupgrade tar wrote both partitions,
  rebooted through the watchdog and booted with the JFFS2 overlay; a test
  file survived the reboot. The `rootfs_data` split is computed
  dynamically after the squashfs, so the larger rootfs is safe.
- **Watchdog/reboot fixed.** The restart handler used a divider of 1,
  which does not tick on this SoC, so `reboot -f` ended in
  `Reboot failed -- System halted`. A divider of 500 with the BSP's
  `WDTCNR=0` overflow selection resets in about 2.6 s. `reboot -f` now
  prints `Reboot Result from Watchdog Timeout!` and boots OpenWrt from
  NAND with the overlay intact.

## TL;DR (historical)

- The factory boot log (`boot.log`) shows the normal path:
  `Checking bank2 first...` -> `Bank2 is corrct!` -> `Jump to image
  start=0x80c00000...` -> vendor Linux 3.10.24. There is **no** bank copy
  when bank A is valid.
- Before the BBI fix, every image written through our `mtd` path (the
  vendor kernel, a `cs6c` loader, and a zero-checksum `cr6c` loader)
  failed the bank-A check and produced the same `ret=2` + `no sys
  signature` scan. The bootloader then checked bank B, copied 30 MiB over
  bank A and booted the vendor image. This was the BBI/OOB mismatch, not
  the image header.
- The `mtd0` dump contains only the first-stage/monitor and the BBT/V2R
  tables. The main bootloader (the code that prints `Checking bank2
  first...`, `Bank2 is corrct!`, `Copy bank2...`) is not in the dump; it is
  loaded separately. The monitor is reachable at `<RealTek>`.

## Confirmed observations

> This section is the pre-fix research log. It documents how the failure was
> isolated; see **Resolution** above for the final outcome and the code
> that was committed.

### 1. Factory boot path (`boot.log`)

`boot.log` is a 355-line factory boot capture. The bootloader output is:

```
Nand booting..
...
---Realtek RTL8198C boot code at 2018.02.01-21:49-0500 v1.6a (800MHz)
Checking bank2 first...
Bank2 is corrct!
Jump to image start=0x80c00000...
Uncompressing Linux... done, booting the kernel.
Linux version 3.10.24 ...
```

The vendor command line (line 54):

```
console=ttyS0,38400 root=/dev/mtdblock3
mtdparts=rtk_nand:5m@0x0(boot),3m@0x500000(setting),6m@0x800000(linux),
24m@0xe00000(rootfs),6m@0x2600000(linux2),24m@0x2C00000(rootfs2),
38m@0x4400000(data),2m@0x6A00000(nvdata) init=/bin/init
```

The MTD partition table in the vendor kernel (lines 123-130) matches the
DTS. The factory bank A is valid and boots directly; the bootloader checks
bank B first, but does not copy it when bank A is already valid.

### 2. Our `mtd`-written images fail bank A

The same bootloader, with an image written through our kernel `mtd` path,
prints the bank-A failure path:

```
Bank2 is corrct!
ret=2  ------> line 315!
no sys signature at 00820000!
no sys signature at 00840000!
...
no sys signature at 00E00000!
Checking bank2...
Flash read from 02600000 to A1000000 with 01E00000 bytes ?
Flash Program from A1000000 to 00800000 with 01E00000 bytes ?
Copy bank2 to bank1 successfully
```

Then it boots the vendor Linux 3.10.24. This was reproduced with:

- the vendor kernel `mtd2-linux.bin` (`cr6c`, 6 MiB) written with
  `mtd write ... linux`;
- a reconstructed `cs6c` kernel image;
- a zero-checksum `cr6c` `rt-loader` image
  (`realtek-cvimg linux-ro ... 0x80c00000 0x800000`, 3,268,400 bytes).

The `mtd` read-back of the used portion matched the input every time
(`cmp -n <len> ...` returned 0). The bootloader still rejected bank A.
Both `cs6c` and `cr6c` fail, so the `cs6c`/`cr6c` preference is not the
only issue. The `ret=2` debug line suggests bank A's signature is found
before a later check makes the image fail (`ret` becomes 0 and the scan
continues). The bank-B copy is the bootloader's fallback, not a preference
for a normal boot. For the `cr6c` loader the copy itself was not captured;
the serial read stopped after the `no sys signature at 00E00000` scan.

### 3. The bank-B fallback overwrites bank A

The fallback copies the entire bank (30 MiB = `linux2` + `rootfs2`,
`0x02600000`..`0x04400000`) to `0x00800000`..`0x02600000`. It overwrites
both `linux` and `rootfs` in bank A. The bootloader, `boot`, `setting`,
`nvdata` and the BBT/V2R area are not touched. The vendor bank B remains
until it is overwritten.

### 4. The `mtd` write/read path is self-consistent

A NAND write/read test on the `data` partition (mtd7) wrote the vendor
kernel image and read it back through the same kernel driver. The used
1,908,756 bytes matched exactly (`CMP_USED=0`). A single-byte difference
appeared only in the unused 0xFF tail, where the driver skips pages that
are all 0xFF; that is not relevant to the image. The bootloader prints
`Congratulation!! No BBs in this Nand`; the V2R table at `0x100000` maps
blocks 64-111 (`linux`) and 112-303 (`rootfs`) identically, with the only
remap at block 500 in `rootfs2`. So the physical offsets used by the Linux
MTD and the bootloader's virtual mapping agree for the images we write.

This proves the driver write/read is internally consistent, but **not**
that the bootloader can decode the OOB/tag. The bank-A rejection is the
evidence that it cannot.

### 5. `mtd0` is the first-stage/monitor, not the main bootloader

The 5 MiB `boot` partition dump (`~/firmware/ap5100w/mtd0-boot.bin`,
`a4a7837e72e4b264258c9605734a0cbd`) contains:

- `0x000000`-`0x00D000`: non-0xFF code and strings;
- `0x100000`, `0x120000`, `0x140000` and `0x160000`, `0x180000`,
  `0x1A0000`: BBT/V2R table copies;
- the rest is 0xFF.

The only boot-related strings in the dump are the first-stage strings:

```
Nand booting..
Nand DMA pages:
Nand DMA ECC NRER error
Nand DMA ECC NECN error
NandD-R-Done->Jump to DRAM
```

The main bootloader strings (`COMMAND MODE HELP`, `Checking bank2
first...`, `Bank2 is corrct!`, `Copy bank2 to bank1 successfully`,
`Jump to image start=`) are **not** in the dump. The monitor's `?` help
strings are not in the dump either. The `mtd0` dump is therefore the
first-stage/NAND loader/monitor, not the complete main bootloader. A
kernel-side `mtd0` read uses the controller's ECC/tag path; the first-stage
reads the main bootloader with `TAG_DIS` (see section 6), so the dump may
be missing the code that was written without ECC tags.

### 6. First-stage disassembly (radare2)

The dump was opened as raw MIPS big-endian, mapped at `0xBFC00000`:

```
r2 -a mips -b 32 -e cfg.bigendian=true -m 0xBFC00000 mtd0-boot.bin
```

The reset vector is:

```
0xbfc00000  j 0xbfc0000c
0xbfc0000c  jal 0xbfc00c60      ; print "Nand booting.."
0xbfc00010  nop
...
```

`0xbfc00c60` writes the string at `0xBFC00DE4` ("Nand booting..") to the
UART data register at `0xB8002000`. The rest of the first-stage performs
DRAM initialization and then a NAND DMA read:

```
0xbfc00a68  lui  t7, 0xb801
0xbfc00a6c  ori  t7, t7, 0xa004   ; NACR
0xbfc00a70  sw   t6, (t7)         ; NACR = 0xC0044553
...
0xbfc00b04  lui  t4, 0xb801
0xbfc00b08  ori  t4, t4, 0xa018   ; NADFSAR = flash address
0xbfc00b0c  sw   t1, (t4)
0xbfc00b10  lui  t4, 0xb801
0xbfc00b14  ori  t4, t4, 0xa020   ; NADRSAR = RAM address
0xbfc00b18  ...
0xbfc00b28  lui  t4, 0xb801
0xbfc00b2c  ori  t4, t4, 0xa010   ; NADCRR
0xbfc00b30  li   t5, 0x5b         ; TAG_DIS | DESC0 | DMARE | LBC_128
0xbfc00b34  sw   t5, (t4)
0xbfc00b38  ...
0xbfc00b40  lw   t0, (s0)
0xbfc00b44  lui  at, 0x8000       ; poll NACR READY
...
0xbfc00b58  addi t1, t1, 0x210    ; 528-byte sector stride
0xbfc00b5c  addi t3, t3, 0x200    ; 512-byte data
...
0xbfc00b90  lui  k0, 0xa010
0xbfc00ba0  lui  at, 0xa000
0xbfc00ba4  or   k0, k0, at
0xbfc00ba8  jr   k0               ; jump to 0xA0100000
```

The read starts at flash `0x2000`, uses a 528-byte flash stride and a
512-byte RAM stride, and jumps to `0xA0100000`. The first-stage also uses
two constants that appear to be the main bootloader's RAM bounds:
`k0 = 0xBFC01000`, `k1 = 0xBFC1CF5C` (size `0x1BF5C` = 114,524 bytes).
The exact relationship between `0xA0100000` and `0xBFC01000` is unresolved
(see H4).

### 7. The bootloader monitor is reachable

`powercycle_esc.sh` (Tapo P110, `192.168.1.221`) floods ESC during boot and
leaves the bootloader at:

```
<RealTek>
```

The `?` command lists the real monitor command set (excerpt):

```
HELP (?)
DB <Address> <Len>
DW <Address> <Len>
EB <Address> <Value1> <Value2>...
EW <Address> <Value1> <Value2>...
CMP: CMP <dst><src><length>
IPCONFIG:<TargetAddress>
AUTOBURN: 0/1
LOADADDR: <Load Address>
J: Jump to <TargetAddress>
NANDID: Read NAND Flash ID
NANDSCRUB: <offset><size>
NANDBE: <offset><size>
NANDPIOR:<flash_Paddress><image_addr><image_size>
NANDPIOW:<flash_Paddress><image_addr><image_size>
NANDR:<flash_Paddress><image_addr><image_size>
NANDW:<flash_Paddress><image_addr><image_size>
NANDBBD:<offset><length>
NANDMARKB:<offset>
NANDT:<cmd>
NANDBBD ...
MDIOR / MDIOW / PHYR / PHYW / PHYPR / PHYPW
XMOD <addr> [jump]
...
ETH : startup Ethernet
CPUClk:
C1Wake : Core 1 wake Up
```

Notable observations:

- `DW` requires the address/length without `0x`; the length appears to be
  in **words**, not bytes. `DW BFC00000 8` prints two lines (8 words).
- `DW BFC00000 8` shows the first-stage code
  (`0BF00003`, `00000000`, `00000000`, `0FF00318`, ...), confirming the
  monitor is running from the first-stage at `0xBFC00000`.
- `DW A0100000 10`, `DW 80100000 8`, `DW A011B71C 8`, `DW A011D000 8`,
  `DW A0110000 8`, `DW A0120000 8` and `DW 80110000 8` return all zeros or
  uninitialized DRAM (`AAAA...`). `DW 84000000 8` is also random. The
  bank-check main bootloader is not at those addresses.
- `DW BFC01000 8` **does** work:
  ```
  BFC01000: 3C08A011 2508B71C 00000000 21082000
  BFC01010: 00000000 0100E821 00000000 0C04000C
  ```
  The previous `Unknown command !` was a transient/parser issue. The
  `DW` length is **decimal words**, not hex bytes: `DW BFC01000 800`
  prints exactly 800 words (0xC80 bytes), while `0x800` is parsed as
  decimal 800. The address is hex.
- The range `0xBFC01000`-`0xBFC1CF5C` (`0x1BF5C` = 114,524 bytes) was
  dumped in 800-word chunks with no gaps and saved on the host as
  `~/firmware/ap5100w/main-bootloader.bin` (114,524 bytes). Its first
  bytes match the `DW` above. `strings` finds repeated
  `NandD-R-Done->Jump to DRAM`, but **not** `Checking bank2 first...`,
  `Bank2 is corrct!`, `Copy bank2 to bank1 successfully` or the monitor
  help strings. This 0xBFC01000 image is therefore a second-stage/NAND
  loader (or a relocated first-stage), not the bank-check main bootloader.
  The bank-check code/strings are more likely in the NAND
  `0xD000`-`0x3BEB8` region that the kernel's tag-enabled mtd read
  returns as 0xFF; the monitor's `NANDR`/`NANDPIOR` can read that region
  with the bootloader's own ECC.

### 8. BBI/OOB swap mismatch (confirmed)

The bootloader source in `bootcode_rtl8198c` has:

```c
#define SWAP_2K_DATA 1
#define BBI_SWAP_OFFSET ((16 * 4) - 10 - 1)   /* = 53 */
```

and in `rtk_read_ecc_page_a` / `rtk_write_ecc_page_a`:

```c
image_addr[DATA_BBI_OFF] = oob_addr[BBI_SWAP_OFFSET];
oob_addr[BBI_SWAP_OFFSET] = temp_val;
```

`DATA_BBI_OFF` is `(512 * 4) - 48 = 2000`. The bootloader relocates one
byte per page between the page data at offset 2000 and the OOB tag at
offset 53.

The driver at the time had:

```c
#define DATA_BBI_OFF   ((512 * 4) - 48)   /* 2000 */
#define OOB_BBI_OFF    23
```

and the swap existed only in `rtl8198c_nand_read_page`/`read_oob`:

```c
if (orig_block >= BOOT_BLOCK) {
    tmp = buf[DATA_BBI_OFF];
    buf[DATA_BBI_OFF] = oob[OOB_BBI_OFF];
    oob[OOB_BBI_OFF] = tmp;
}
```

There was no reverse swap in `rtl8198c_nand_write_page`. This had three
consequences, all confirmed after the fix:

1. The `mtd` read of the factory image swapped the wrong OOB byte
   (`23` instead of `53`), so the dumped data byte at 2000 was not the
   payload. The vendor `mtd2-linux.bin` dump's 16-bit bootloader checksum
   was `0x8c00`, not zero. With offset 53 it is `0x0000`.
2. The `mtd` write of an image (payload at 2000) wrote that byte to flash
   data[2000] and left OOB[53] as 0xFF. The bootloader's read swapped the
   payload into OOB[53] and put 0xFF into data[2000], so its checksum
   failed. This was the bank-A rejection mechanism.
3. The factory rootfs read/dump with the wrong OOB byte explained the
   old `unsquashfs` failure on `mtd3`. The correct offset did fix the
   rootfs dump; `unsquashfs -s` accepts it.

The driver now uses `OOB_BBI_OFF = 53` and performs the reverse swap when
writing a page.

### 9. LBC / DMA tag configuration

The bootloader's ECC read/write uses:

```c
#define TAG_DIS  (1 << 6)
#define DESC0    (1 << 4)
#define DMARE    (1 << 3)
#define DMAWE    (1 << 2)
#define LBC_128  3
```

The first-stage's `NADCRR` is `0x5B` =
`TAG_DIS | DESC0 | DMARE | LBC_128`. The main `rtk_read_ecc_page` also
uses `TAG_DIS` for the kernel image; `rtk_read_ecc_page_a` uses tag access
enabled for the ECC/OOB reads.

Our driver originally used:

```c
#define NADCRR_DMA_READ (NADCRR_DESC0 | NADCRR_DMARE | 2)  /* LBC 64 */
```

and the write used `DESC0 | DMAWE | 2`. The driver was changed to LBC 128
(`NADCRR_LBC_128 3`) to match the bootloader and committed as
`803ba32736 realtek: use the bootloader's 128-word NAND DMA burst`.

The LBC fix alone did **not** make the bootloader accept the `mtd`-written
vendor image or the `cr6c` loader: both still produced the bank-A scan and
the bank-B copy. LBC is likely necessary but not sufficient. The tag
enable/disable and the BBI swap remain.

### 10. Vendor image checksum

Using the bootloader's 16-bit word sum over the `pHeader->len` payload:

```
sum = 0
for i in range(0, len, 2):
    sum += (payload[i+1] | (payload[i] << 8)) & 0xffff
```

- Vendor `mtd2-linux.bin` / `mtd5-linux2.bin` with the old read (OOB 23):
  `0x8c00`.
- Vendor `mtd2`/`mtd5` with the fixed read (OOB 53): `0x0000`.
- Our `cs6c` and `cr6c` `rt-loader` images: `0x0000`.

The fixed read reconstructs the factory image with a zero sum, which
confirmed the BBI hypothesis. The bootloader enforces this checksum for
bank A; the old dump was wrong, not the factory image.

### 11. Board and tree state (final)

- Branch: `rtl8198c_boot`, based on `rtl8198c_master`.
- Local commits:
  - `36eb42b5a5 realtek: pass the burn address to realtek-cvimg`
  - `02da8bb198 realtek: build the AP5100W flash kernel through rt-loader`
  - `433c193e96 realtek: check AP5100W sysupgrade image sizes`
  - `803ba32736 realtek: use the bootloader's 128-word NAND DMA burst`
  - `b7c04b07f3 realtek: fix the NAND BBI swap and OOB handling`
  - `4113f3866b realtek: reserve the JFFS2 cleanmarker page on NAND`
  - `28fef3e5aa realtek: reset the AP5100W through the watchdog`
  - a single docs commit at the tip (this file)
- Flash layout now:
  - `linux` (bank A): OpenWrt `cs6c` kernel, 3,268,818 bytes, checksum
    `0x0000`;
  - `rootfs` (bank A): OpenWrt XZ squashfs, 5,093,376 bytes;
  - `rootfs_data`: JFFS2 overlay, formatted by the patched kernel;
  - `linux2`/`rootfs2`: vendor fallback restored from
    `vendor-linux2-fixed.bin` and `vendor-rootfs-fixed.bin` (SHA-256
    verified).
- `boot`, `setting` and `nvdata` untouched.
- The board boots OpenWrt from NAND with `wpad-basic-mbedtls`, and
  `/root/persist-final` survives powercycles.
- No push; no squash.

## Hypotheses — outcome

### H1 (confirmed): BBI/OOB swap mismatch

The bootloader relocates one byte per page between `data[2000]` and the OOB
tag at offset 53 on both reads and writes. The driver used OOB offset 23
and only swapped on read. With offset 53 and the reverse write swap, the
fixed-driver read of the vendor `mtd2`/`mtd5` images sums to `0x0000`
(previously `0x8c00`), and a zero-checksum `cs6c` image written through
`mtd` is accepted by the bootloader. The vendor rootfs dump also becomes
valid (`unsquashfs -s` succeeds). This was the root cause of the bank-A
rejection.

### H2 (confirmed, necessary but not sufficient alone): DMA tag/LBC mismatch

The bootloader uses `LBC_128`; the driver used `LBC_64`. The LBC 128
change (`803ba32736`) matches the bootloader but did not by itself make
bank A pass. The remaining difference was the BBI swap (H1).

### H3 (resolved): Bank-A check logic

The bootloader's `ret=2 ------> line 315!` line is its 16-bit checksum
failure path: the signature is found (`cr6c` returns 2), the checksum over
the logical image fails, `ret` becomes 0 and the bootloader scans the
remaining image offsets before falling back to bank B. The checksum is
computed through the bootloader's BBI-swapped read, which is why the
driver's write representation mattered.

### H4 (open, not blocking): the main bootloader is not in the dumps

The bank-check code/strings are not in `mtd0` or in the
`0xBFC01000`-`0xBFC1CF5C` dump (`~/firmware/ap5100w/main-bootloader.bin`);
that range is a second-stage/NAND loader. The monitor is at
`0xBFC00000`; the first-stage reads NAND `0x2000`-`0x3BEB8` into
`0xA0100000` with `TAG_DIS` and jumps there. The bank-check main
bootloader is likely in the NAND `0xD000`-`0x3BEB8` region, but it is not
needed for the fix. If it is ever wanted, use the monitor's
`NANDR`/`NANDPIOR` to read that region and analyze it with radare2.

### H5 (rejected): historical `NEED_CHKSUM`/missing `memcpy`

The factory boot and the bank-B copy both work, so the copy path exists.
The `mtd` write/BBI mismatch fully explains the rejection; no
`NEED_CHKSUM`/`memcpy` bug was found.

### H6 (resolved): rootfs `cr6c` check

The `cr6c` signature makes the bootloader check the rootfs. The vendor
rootfs passes; the OpenWrt XZ squashfs did not reach `Jump to image
start` within 150 s under `cr6c` (the bootloader appears to scan/validate
the rootfs). The final image uses the sysupgrade kernel's `cs6c`
signature, which does not trigger the rootfs check, and the kernel mounts
the squashfs itself.

## Remaining work

1. **Bad blocks 300-302.** They were marked bad while isolating the JFFS2
   double-program failure. They are skipped and cost about 384 KiB of the
   19.5 MiB overlay. If they are not genuinely weak, they can be reclaimed
   later by clearing OOB[0] (the current driver only implements marking
   bad, not unmarking).
2. **Bank B.** Restored to the vendor `linux2`/`rootfs2` from the
   fixed-driver dumps and verified by SHA-256. Keep it as the fallback
   unless both banks are intentionally programmed with OpenWrt.
3. **JFFS2 unmount warning.** `sysupgrade` logs `VFS: Busy inodes after
   unmount of jffs2` and a stack trace while stopping the overlay. The
   upgrade completes and the overlay mounts again on the next boot; worth
   cleaning up separately.
4. **Optional bootloader archaeology.** The bank-check main bootloader is
   still not dumped; only needed if the checksum/rootfs logic must be
   documented at the instruction level.

## Appendix

### A. Monitor capture

The monitor was entered with `~/powercycle_esc.sh` (Tapo P110 at
`192.168.1.221`). The direct script needs the USB NIC
(`enx00e04c680290`) down so the Tapo is reached through the host's main
network; `flash.sh` does this automatically. The script timestamps:
`flash.sh` 2026-10-26 09:23, `powercycle_esc.sh` 2026-10-26 10:59. If the
Tapo IP changes, update both scripts.

The `?` output is the source of the command list in section 7. The actual
help does not list `TFTP`; it lists `DB`/`DW`, `NANDR`/`NANDW`,
`NANDPIOR`/`NANDPIOW`, `NANDBE`, `NANDSCRUB`, `NANDBBD`, `NANDMARKB`,
`NANDT`, `MDIO*`, `PHY*`, `XMOD`, `ETH`, `CPUClk`, etc.

### B. First-stage NAND read details

- Flash start: `0x2000`.
- Flash stride: `0x210` (528 bytes) per sector, 4 sectors per page, with
  a `0x7C0` gap to the next page boundary (`0x1000` per page).
- RAM destination: `0xA0100000`, 512 bytes per sector.
- `NADCRR = 0x5B` (`TAG_DIS | DESC0 | DMARE | LBC_128`).
- `NACR = 0xC0044553` (first-stage), vs the driver's
  `0xC00FFFFF`/`NACR_ECC_EN`.
- `k0 = 0xBFC01000`, `k1 = 0xBFC1CF5C` (0x1BF5C bytes). This range was
  dumped through the monitor (`DW`, decimal word lengths) and saved as
  `~/firmware/ap5100w/main-bootloader.bin`. It contains
  `NandD-R-Done->Jump to DRAM` but not the bank-check strings; it is a
  second-stage/NAND loader (or relocated first-stage), not the bank-check
  code.
- Jump: `jr 0xA0100000`. `DW A0100000` is zero in the monitor; the
  bank-check code is likely in the NAND `0xD000`-`0x3BEB8` region that the
  kernel's tag-enabled mtd read returns as 0xFF.
- Monitor `DW` quirk: address is hex, length is **decimal words**
  (`DW BFC01000 800` = 800 words = 0xC80 bytes).

### C. BBI/LBC constants

```
bootloader: BBI_SWAP_OFFSET = (16 * 4) - 10 - 1 = 53
            DATA_BBI_OFF   = (512 * 4) - 48    = 2000
driver:     OOB_BBI_OFF    = 53   (fixed)
            DATA_BBI_OFF   = 2000
bootloader: LBC_128 = 3, TAG_DIS = 1 << 6
driver:     NADCRR_LBC_128 = 3 (commit 803ba32736)
```

The bootloader's `rtk_read_ecc_page_a`/`rtk_write_ecc_page_a` swap
`image_addr[2000]` and `oob_addr[53]`. The driver now performs the same
swap on both read and write.

### D. Bootloader sum

```
sum = 0
for i in range(0, header.len, 2):
    sum += (payload[i+1] | (payload[i] << 8)) & 0xffff
```

- Vendor `mtd2`/`mtd5` with the old read (OOB 23): `0x8c00`.
- Vendor `mtd2`/`mtd5` with the fixed read (OOB 53): `0x0000`.
- Our `cs6c`/`cr6c` loader: `0x0000`.
- Factory image with the fixed read: `0x0000` (confirmed).

### E. Current tree

```
git log --oneline -7
28fef3e5aa realtek: reset the AP5100W through the watchdog
4113f3866b realtek: reserve the JFFS2 cleanmarker page on NAND
b7c04b07f3 realtek: fix the NAND BBI swap and OOB handling
803ba32736 realtek: use the bootloader's 128-word NAND DMA burst
433c193e96 realtek: check AP5100W sysupgrade image sizes
02da8bb198 realtek: build the AP5100W flash kernel through rt-loader
36eb42b5a5 realtek: pass the burn address to realtek-cvimg
```

The docs commit at the tip contains this file; the earlier doc-only
commits were squashed into it. No push. The JFFS2 change is a target
kernel patch (`901-jffs2-reserve-cleanmarker-page.patch`), not a driver
`#ifdef`.

### F. Safety

- The bootloader, `boot`, `setting` and `nvdata` are never written.
- `linux2`/`rootfs2` are the restored vendor fallback. They were
  accidentally erased by the monitor's `NANDBE` (which takes a start and
  a length, not a start and end) and restored from the fixed-driver
  dumps, SHA-256 verified.
- TFTP recovery is available via `flash.sh` and `powercycle_esc.sh`.
- `boot.log` remains an untracked research artifact; this analysis file is
  committed with the driver and JFFS2 changes.
- `reboot -f` now resets through the watchdog in about 2.6 seconds. The
  Tapo powercycle is only needed if the board is already halted or the
  bootloader needs ESC.
