# AGENTS.md — Realtek RTL8198C Development Guide

## Flash / test cycle

### Full build + flash + test (one-shot)
```
~/flash.sh --build --test
```
This builds (`make -j16`), powercycles the Tapo P110, floods ESC to
intercept the bootloader, TFTP-uploads the initramfs, waits 75s for
kernel boot, then reconfigures `enx00e04c680290` to `192.168.2.1/24`
for board testing.

### Flash without build
```
~/flash.sh              # no args = just flash + wait for boot
~/flash.sh --test       # flash + reconfigure test network after boot
~/flash.sh --no-wait    # flash only, don't wait for boot
```

### Manual build (without full flash cycle)
```
cp target/linux/realtek/files-6.18/drivers/net/ethernet/rtl8198c_eth.c \
   build_dir/target-mips_24kc_musl/linux-realtek_rtl8198c/linux-6.18.28/drivers/net/ethernet/
make -j16
ln -sf bin/targets/realtek/rtl8198c/openwrt-realtek-rtl8198c-askey_ap5100w-initramfs-kernel.bin nfjrom
```

### Post-boot: connect serial and bring up the test network
```
serial-mcp_open /dev/ttyUSB0 38400
ifup lan                            # bring up eth0 at 192.168.2.2/24
cat /sys/kernel/debug/rtl8198c-eth/eth0/regs  # live register dump
dmesg | grep "rtl8198c-eth"         # driver probe messages
ping 192.168.2.1                     # test connectivity
```
**Always close:** `serial-mcp_close`

### Network layout
- `enx00e04c680290` — USB-Ethernet → board RJ45 jack (physical port 3 or 4)
- `enx04ab186c7fad` — USB-Ethernet → board RJ45 jack (physical port 3 or 4)
- `eno1` — Host internet access (main NIC)
- Port-to-port through switch fabric: `ping -I 192.168.3.1 192.168.2.1`
- Board IP (uci-defaults): `192.168.2.2/24`, host: `192.168.2.1/24`
- Bootloader TFTP: `192.168.1.6` via `enx00e04c680290`

The uci-defaults script (`files/etc/uci-defaults/90-test-link`) sets:
- `network.lan.device='eth0'` with static IP `192.168.2.2/24`

**Bridge (br-lan) is not yet supported.**  Enslaving eth0 to a Linux
bridge puts the interface in promiscuous mode, which breaks the switch's
CPU-port forwarding path — the FDB entries and dynamic learning don't
correctly forward bridged frames to the CPU.  This needs investigation
of how the switch handles promiscuous-mode traffic (possibly requiring
additional FDB entries or switch configuration changes).

---

## PCIe Driver

`target/linux/realtek/files-6.18/arch/mips/pci/pci-rtl8198c.c`

### Status
- **PCIe link training** on both ports.  RTL8813AE `[10ec:8813]` at
  `0000:00:01.0` and `0000:02:01.0`.  Host bridge `[10ec:8198]`.
- **Port 1 Wi-Fi fully operational** — rtw88_8814ae + 32-bit MMIO patch
  loads, firmware 33.6.0, phy1 created, wlan1 scans networks.
- **Port 0 firmware power-on hang** — rtw88 probe reaches "init done",
  then hangs at `rtw_pci_dma_reset`.  Port 0 RTK_PCI_CTRL reads all-zero.
- **Port 0 requires KSEG1 bypass** for BAR2 mapping; standard ioremap
  fails even with correct bridge windows.  Port 1 works either way.

### Patching mac80211 (rtw88) with quilt
```
make package/kernel/mac80211/{clean,prepare} V=s QUILT=1
cd build_dir/.../mac80211-regular/backports-...
quilt new 100-my-fix.patch
quilt add drivers/net/wireless/realtek/rtw88/pci.c
quilt refresh
cp patches/100-my-fix.patch ../../../../../package/kernel/mac80211/patches/rtl/
make -j16
```

---

## Switch / Ethernet Driver

`target/linux/realtek/files-6.18/drivers/net/ethernet/rtl8198c_eth.c`

### Status — WORKING

| Capability | Status |
|---|---|
| Board → host ping | Working — 0.5ms RTT |
| Host → board ping | Working |
| Cross-host through switch fabric | Working — ~0.05ms |
| Interrupts (IRQ 17 via GIC shared 25) | Working — fires for RX/TX events |
| TX completions | Working — tx_tail catches up to tx_head |
| RX frame delivery | Working — RISC fills descriptors |
| NAPI poll | Working |
| MAC from device tree | Working |
| iperf3 TCP throughput | Working — ~121 Mbps across 4 streams (MIPS CPU-bound at gigabit) |
| Debug detectors (hung_task, softlockup, hardlockup) | Operational — exposed a real softlockup in `emulate_load_store_insn`, since resolved via `skb_reserve` alignment fix |

### Current status — gigabit working, hardware offload partially blocked

Gigabit link auto-negotiation at 1000 Mbps is now working via:
- `phy_refine()` — NCTL DSP calibration (`phy_98c_para[]` table from BSP)
- Explicit `mdio_write(id, 9, v | BIT(9))` — 1000BASE-T FD advertisement (PCRP NwayAbility bits alone are insufficient)
- `MACCR` SYSCLK set to 100MHz (lx_clk) + FCDSC enlarged to 48 pages
- Auto-down-speed detects 2-pair cables and correctly falls back to 100M

Throughput is ~121 Mbps (up from ~95 Mbps at 100M). The MIPS 24Kc CPU
at ~500 MHz is the bottleneck for CPU-terminated traffic; the switch core
forwards between physical ports at wire speed via L2/L3 hardware offload.

Hardware offload (checksum, scatter-gather) is partially implemented:
- Descriptor checksum fields (`CSUM_IP`, `CSUM_TCPUDP`, `ph_ipIpv4/1st/v6`,
  `ph_ipIpv6HdrLen`) added; `CHECKSUM_PARTIAL` path in `eth_start_xmit`
- Blocked: CSCR (`0xBB804048`) — any write kills the board
- Blocked: `NETIF_F_SG` — TX path changes cause TCP hang
- Blocked: `ndo_set_rx_mode` (bridge/promisc) — BSP stub, from-scratch
- See inline `BLOCKED:` comments in the driver for register/bit details

### Key design decisions

- **Legacy pkthdr/mbuf descriptor format** — matches the bootloader's
  `swNic_poll.c` AND the BSP kernel's `swNic_init()` for 8198C.
  `CONFIG_RTL_SWITCH_NEW_DESCRIPTOR` is NOT defined in the 8198C config;
  `rtl_types.h` maps `RTL_swNic_init` to `swNic_init` (legacy).
  Flat 24-byte format retained under `#else` as a compile-time
  alternative (not used by the BSP for this chip).
- **OWN bit convention** — `DESC_RISC_OWNED=0x00`, `DESC_SWCORE_OWNED=0x01`.
  Descriptors are handed to the RISC (CPU owns=1=empty), the RISC fills
  them and clears OWN (0=done), the CPU processes and sets OWN back to 1.
- **BUSBURST_32WORDS** — matches the bootloader (not the BSP's 128-word).
- **TXRINGCR** — only ring0 enabled; rings 1-3 disabled to keep the
  hardware state machine healthy (BSP comment).
- **Netif table entry** — uses the non-8198C (8881A-compatible) format
  because the bootloader compiles without `CONFIG_RTL8198C` defined.
- **Cluster buffer allocation** — cluster buffers are 4-byte aligned
  (no `+2` offset); IP header alignment is corrected dynamically in
  `eth_poll` via `skb_reserve` to avoid MIPS unaligned-access softlockups.
- **FFCR** — both `EN_UNMCAST_TOCPU` and `EN_UNUNICAST_TOCPU` are cleared
  so unknown frames flood between physical ports (cross-host forwarding).
  The broadcast FDB entry ensures broadcasts still reach the CPU.
- All `#if 0` / `#if 1` guards removed.  File is ~1550 lines.

### Register write differences from bootloader

The driver now writes several registers the bootloader does not:
`DMA_CR0` (HsbAddrMark), `DMA_CR1`, `DMA_CR4`, `TXRINGCR`, `CPUICR1`,
`SWTCR0` (PORT_BASED + EnUkVIDtoCPU), `MACCTRL1` (CMAC_CLK_SEL),
`CPUICR1` BIT(6) (CF_TX_GATHER), `CSCR` (blocked).
These are all confirmed correct and stable except `CSCR` — see inline
`BLOCKED:` comments.

### KSEG1 vs Physical Addresses

| Value fed to... | Format | Reason |
|-----------------|--------|--------|
| **SWTAA** (TACI) | **KSEG1** (0xBBxxxxxx) | RISC TACI engine needs KSEG1 |
| CPURPDCR0 / CPUTPDCR0 (ring bases) | Either | Bootloader uses KSEG1 |
| `devmem` reads/writes | Physical | Busybox expects physical |
| C code register access | KSEG1 (volatile ptrs) | Standard MIPS uncached IO |
| pkthdr/mbuf pool pointers in ring entries | KSEG1 | RISC reads them directly |

### Networking debugging

**Live register dump:**
```
cat /sys/kernel/debug/rtl8198c-eth/eth0/regs
```

**swdebug.sh commands (on board):**
```
swdebug.sh swregs          # All switch core + CPUIF registers
swdebug.sh phy <id> <reg>  # Read PHY register
swdebug.sh phy_all         # Dump all 5 PHYs
swdebug.sh vlan_read [vid] # Read VLAN entry
swdebug.sh l2_read <row> <col>  # Read L2 entry (direct memory)
swdebug.sh l2_write_force <row> <col> <w0> <w1>  # Write L2 (CMD_FORCE)
swdebug.sh pcrp             # PCRP0-6 dump
swdebug.sh link             # eth0 link stats
swdebug.sh irq              # Interrupt count
```

**Host debug scripts:**
```
~/test_traffic.sh           # Automated host-side packet testing
ping -I 192.168.2.1 192.168.2.2       # Host → board via enx00e04c680290
ping -I 192.168.3.1 192.168.2.1       # Cross-host via switch fabric
sudo tcpdump -i enx00e04c680290 -n    # Capture traffic on host
```

**Sysfs / proc:**
```
cat /proc/interrupts | grep eth        # IRQ counter
ifconfig eth0                          # TX/RX packet counts
ethtool eth0                           # Link status
```

---

## XHCI Driver

`target/linux/realtek/files-6.18/drivers/usb/host/xhci-realtek.c`

### Status

Stub — driver loads and USB hub initializes, but no active development
has been done on this driver.  A spinlock corruption was observed under
`SLUB_DEBUG` (documented in `BUGS.md`), likely a pre-existing MIPS kernel
bug, not a driver issue.

---

## BSP Reference Directories

The authoritative BSP source for this chip is:
- **Drivers:** `~/rtl819xx/RTL8197_3411D_4/rtl819x/linux-3.10/drivers/net/rtl819x/`
- **Board config:** `~/rtl819xx/RTL8197_3411D_4/rtl819x/boards/rtl8198C_8954E/`
- **Bootloader source:** `~/rtl819xx/rtl8198c-rtk/original/bootcode.tar.gz`
  (`bootcode_rtl8198c_20141121.tar.gz` inside)

Key BSP files used during driver development:
- `rtl865xc_swNic.c` — legacy pkthdr/mbuf swNic_init (CSUM/TCPUDP flags, SG mbuf chaining)
- `rtl819x_swNic.c` — flat 24-byte New_swNic_init (non-legacy, not used by 8198C)
- `rtl_nic.c` — main BSP NIC driver
- `rtl865x_asicCom.c` — `rtl865x_start()`, `rtl865x_down()`, `FullAndSemiReset()`
- `rtl865x_asicL2.c` — `rtl865x_initAsicL2()` (massive init function), `Setting_RTL8198C_GPHY()`, `phy_refine()` NCTL calibration
- `rtl865xc_asicregs.h` — complete register definitions
- `asicTabs.h` — table type enumeration and sizes
- `common/mbuf.h` — pkthdr/mbuf struct definitions including 8198C checksum/LSO fields
- `include/net/rtl/rtl_types.h` — RTL_swNic_init macro mapping for legacy vs new descriptor
- `boards/rtl8198C_8954E/config.linux-3.10.RTL8198C_8814_8194_MP` — kernel config for MP build

## Other Documents

- `RTL8198C.md` — chip-level reference (registers, descriptor format, TACI, memory map)
- `BUGS.md` — known kernel bugs exposed by debug configs (not Ethernet driver)
- `realtek-switch-overview.txt` — RTL838x switch architecture reference (conceptual overlap)
- `crash-traces.txt` — collected Oops/panic traces from development session
