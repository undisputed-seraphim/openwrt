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
cp target/linux/realtek/files-6.18/drivers/net/ethernet/rtl8198c/* \
   build_dir/target-mips_24kc_musl/linux-realtek_rtl8198c/linux-6.18.55/drivers/net/ethernet/rtl8198c/
make -j16
ln -sf bin/targets/realtek/rtl8198c/openwrt-realtek-rtl8198c-askey_ap5100w-initramfs-kernel.bin nfjrom
```

### Post-boot: connect serial and bring up the test network
```
serial-mcp_open /dev/ttyUSB0 38400
ifup lan                            # bring up eth0 at 192.168.2.2/24
cat /sys/kernel/debug/rtl8198c-eth/switch/regs  # live register dump
dmesg | grep "rtl8198c-eth"         # driver probe messages
ping 192.168.2.1                     # test connectivity
swtest.sh --full                    # self-test; PHY 3 "down" is expected
                                    # with a single cable (PHY 4 carries it)
```
**Always close:** `serial-mcp_close`

**initramfs packages:** `DEVICE_PACKAGES` in
`target/linux/realtek/image/rtl8198c.mk` only affects sysupgrade images.
The initramfs is built from the `CONFIG_PACKAGE_*` selection in
`.config`, so enable test tools there (e.g. `CONFIG_PACKAGE_tcpdump=y`,
`iperf3`, `ip-full`).

### Network layout
- `enx00e04c680290` — USB-Ethernet → board RJ45 jack (physical port 3 or 4)
- `enx04ab186c7fad` — USB-Ethernet → board RJ45 jack (physical port 3 or 4)
- `eno1` — Host internet access (main NIC)
- Port-to-port through switch fabric: **works** after the VLAN-decision
  fix (VLAN-based decision + PVIDs).  `ping -I 192.168.3.1 192.168.2.1`
  is still not a valid test — both addresses are local to the host, so
  the kernel loops the packet back (that is where the old 0.05 ms came
  from).  Use a second host or put one adapter in a network namespace.
- Board IP (uci-defaults): `192.168.2.2/24` on `eth4`, host: `192.168.2.1/24`
- Interfaces: `eth0`..`eth4` are physical ports 0..4, one netdev each,
  MAC = base + port.  Port 0 is the WAN port (VLAN 8), ports 1-4 are LAN
  (VLAN 9); the test cable is in port 4, hence `eth4` everywhere.
- Bootloader TFTP: `192.168.1.6` via `enx00e04c680290`

The uci-defaults script
(`target/linux/realtek/rtl8198c/base-files/etc/uci-defaults/90-test-link`)
sets:
- `network.lan.device='eth4'` with static IP `192.168.2.2/24`

**Bridge (br-lan) works.**  The old "promisc breaks the CPU path" note
was wrong: the blocker was the driver handing bridge-addressed frames to
the port whose MAC matched the destination, so the bridge learned senders
on the wrong port and answered out the wrong cable.  A port enslaved to a
bridge now takes frames by ingress port.  `br-lan` over eth3+eth4 with the
default MAC (the lowest port's) pings and learns each host on its real
port.  The port is also registered as a switchdev offload port, so the
bridge's MDB reaches the switch and the frames the switch flooded carry
`offload_fwd_mark` - the software bridge no longer sends a second copy
(measured: 100 broadcasts used to arrive twice on the other port, now
once).  Limits: known-unicast flows never reach the CPU, so the bridge's
MAC table only learns from floods; the switch enforces the bridge's STP
port states while a port is enslaved; port flags (learning, flood) stay in
software.  FFCR's TOCPU bits and `IFF_PROMISC` do nothing to delivery
(measured).  See the bridge section of RTL8198C.md.

**IPv4 routing can be offloaded, and is off by default.**  `echo 1 >
switch/route_offload` mirrors the kernel's FIB: gateway routes through the
next-hop table, connected prefixes as an ARP block (on-link prefixes /23
and longer, 512 entries in total), the router's own addresses as CPU
routes.  A route is only taken when every more specific route with a
different next hop is taken too, which is how the default route waits
until the connected prefixes are in hardware.  Routed frames then never
reach the CPU - they are not filtered, not tracked and not NAT'd - so a
NAT'd router has to leave the switch off.  Limit: one L3 interface per
VLAN, because the netif entry carries the router MAC and the lookup is by
VID.

**Link aggregation is not supported.**  There is no usable LAG block on
this core: the BSP's `LAGCR0` makes egress to the ports it names
disappear, and the 8198C register map does not list the block.  A Linux
bond still works in software (the kernel hashes into one member's port)
and the driver logs one line when a port is enslaved; debugfs `lag`
states the limitation read-only.

---

## PCIe Driver

`target/linux/realtek/files-6.18/drivers/pci/controller/pcie-realtek.c`
and `target/linux/realtek/files-6.18/drivers/phy/realtek/phy-rtk-pcie.c`

### Status
- **PCIe link training** on both ports.  RTL8813AE `[10ec:8813]` at
  `0000:00:01.0` and `0001:00:01.0`.  Host bridge `[10ec:8198]`.
- **Both ports fully operational** — rtw88_8814ae + 32-bit MMIO patch
  loads, firmware 33.6.0, phy0 and phy1 are created, and both phys scan
  networks (2.4 GHz on port 0, 5 GHz on port 1).
- **Both I/O BARs are assigned** — port 0 at I/O `0x1000`, port 1 at
  I/O `0x9000`; the previous `can't assign; no space` messages are gone.
  The memory BAR used by rtw88 remains at `0x19000000` / `0x1a000000`.
- **KSEG1 ioremap** is used for the PCIe memory windows; both ports work
  through the shared host/PHY drivers.

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

`target/linux/realtek/files-6.18/drivers/net/ethernet/rtl8198c/`

Ten objects, one per hardware block (`core`, `phy`, `l2`, `l3`, `l4`,
`acl`, `mcast`, `qos`, `dma`, `netdev`), with `rtl8198c_regs.h` for the
register map and `rtl8198c.h` for the types and the interfaces between
them.  The file headers name the BSP counterpart of each block;
`RTL8198C.md` has the table.

### Status — WORKING

| Capability | Status |
|---|---|
| Board → host ping | Working — 0.5ms RTT |
| Host → board ping | Working |
| Cross-host through switch fabric | Working — port-to-port unicast verified, ~0.6ms RTT |
| Interrupts (virq 18, GIC hwirq 32) | Working — RX and TX completions |
| Link carrier | Working — one link-change IRQ for all ports, NAPI rescans `PSRP0-4` and drives `netif_carrier_on/off`; verified by powering port 4's PHY down and up (`NO-CARRIER`/`LOWER_UP`, ping resumes) |
| TX completions | Working — tx_tail catches up to tx_head |
| TX scatter-gather | Working — `m_next` chains of per-slot spare mbufs, last `MBUF_EOR`+4 CRC, frag cache-flushed; `NETIF_F_SG` on; sendfile 617-630 Mbit/s vs 466 linearised (+33%); `frag_list`/overflow linearised and counted |
| RX frame delivery | Working — RISC fills descriptors |
| RX zero-copy | Working — one skb per mbuf, RISC writes into `skb->data`, `napi_alloc_skb()` refill; host→board ~762 Mbit/s at 9000, 1.08-1.14 Gbit/s with two streams |
| RX buffer sizing | Working — buffers follow the largest port MTU (2 KB at 1500, 10 KB jumbo); MTU changes rebuild the RX ring without touching TX; host→board 543-545 Mbit/s at 1500 vs 337-380 with 10 KB buffers |
| NAPI poll | Working |
| MAC from device tree | Working |
| TCP throughput at 1G | Working — ~430 Mbps host→board, ~370 Mbps board→host (iperf3, CPU-bound) |
| Per-port netifs | Working — `eth0`..`eth4` = ports 0..4, own MACs, RX attributed by destination MAC then source port (bridge ports take the ingress port) |
| Hardware STP port states | Working — the first bridge port turns on `MSCR.EN_STP` + `RMACR.MADDR00`; `br_set_state()` reaches the driver as a switchdev port attribute and lands in `PCRP` bits 5:4.  Verified: `bridge link set dev eth4 state 0` drops data while BPDUs still arrive, and the kernel STP state machine steps listening -> learning -> forwarding |
| Bridge offload (switchdev) | Working — ports register with `switchdev_bridge_port_offload` (PPID = switch id + filtering database, `tx_fwd_offload`); flooded frames are marked `offload_fwd_mark`, so the software bridge sends no second copy (100 broadcasts: 2 copies -> 1, unknown unicast 5 -> 5 not 10); `bridge mdb` groups become L2 entries with the port mask + CPU copy |
| IPv4 routing offload | Working (off by default) — the FIB is mirrored and the routes the hardware can carry are programmed: 8-prefix route table with the CPU default at index 7, ARP blocks for on-link prefixes of /23 and longer, next-hop pairs for gateways, the router's own addresses as CPU /32s.  `echo 1 > switch/route_offload` enables it; routed frames then bypass netfilter and NAT.  Verified in both directions on hardware (0 frames on the CPU ports), withdrawal when a route or neighbour goes away, `l3`/`taci` debugfs |
| NAPT / flow offload | Working (off by default) — IPv4 TCP/UDP source NAT driven by nftables flowtables.  `ndo_setup_tc(TC_SETUP_FT)` turns each offered flow's match tuple and mangle actions into the two session rows (out at `hash(int,remote)`, in verification row at `hash(remote,ext)`), keeps the nft cookie per direction for teardown, refreshes the software flow from the row's validity (no per-row counters) and flushes on unbind.  `echo 1 > switch/flow_offload` needs `route_offload=1`.  Measured 942 Mbit/s TCP and 500 Mbit/s UDP through masquerade with 0 data frames on the CPU ports; a 40 s flow stays offloaded; fragmented UDP passes; ICMP is punted and NAT'd by the kernel |
| Multi-ring TX/RX and queues | Working — two TX rings (`skb->priority >= 4` uses ring 1) and six RX rings, one per switch queue; `CPUQIDMCR0` maps priority->CPU queue, `CPUQDM` queue->ring, `QNUMCR` gives the CPU port six queues; debugfs `dscp` and `rxmap`; `ethtool -S` shows `tx_ring0/1_packets`, `rx_ring0..5_packets`, `rx_queue0..7_packets` |
| Rate limiting | Working (egress) — per port via `tx-0/tx_maxrate`, per queue via debugfs `rate` (APR/L1/PPR); TX priority uses `ph_txPriority` + `UPTCMCR1`, not `ph_queueId`.  Ingress `IBCR` is inert on this core (16 kbps on every port still passed 941 Mbit/s through the fabric) |
| Jumbo frames | Working — MTU up to 9000 (`ndo_change_mtu`, PCRP 9K, PHY page 0 reg 23 bit 8, 10 KB RX buffers, netif-table MTU); ~762 Mbit/s host→board at 9000 with zero-copy RX |
| RX checksum offload | Working — per-frame `ph_flags` trusted for unfragmented IPv4 TCP/UDP (both bits); other protocols/IPv6/fragments stay software-verified, bad frames fall back and are dropped |
| Bridge (`br-lan`) | Working — per-port attribution; flooded frames are marked `offload_fwd_mark`, so the software bridge forwards them once |
| Link aggregation | Not supported — no usable LAG block; Linux bonding runs in software and logs a warning |
| Hardware ACL | Working (ingress) — MAC/IPv4 rules via debugfs `acl`; TCP/UDP rule types unusable (L4 stage not run), egress engine disabled by the vendor |
| Multicast steering | Working — debugfs `mcast` pins groups to ports via L2 entries; the IP (S,G) table is L3-only and does not steer on the bridge |
| WAN/LAN VLANs | Working — port 0 in VLAN 8 (fid 0), ports 1-4 in VLAN 9 (fid 1), CPU in both; per-VLAN MAC tables |
| Tagged VLANs | Working — VLAN devices program membership (max 4 VLANs, one fid each); the CPU is tagged in requested VLANs so the kernel sees the tag. 802.1Q-in-802.1Q (QinQ) works; 802.1ad cannot be switched (the ASIC ignores 0x88a8) |
| Protocol-based VLAN | Working — debugfs `pbvlan`; built-in IPX rule matches 802.2 LLC, not EtherType 0x8137 |
| MAC changes | Working — `ip link set eth4 address ...` moves the L2 + netif entries |
| FDB management | Working — `bridge fdb add/del/show/flush dev eth4`; debugfs `fdb` pins a MAC to chosen ports |
| Storm control | Working — broadcast/multicast meters via debugfs `storm`; no unknown-unicast meter in this core |
| Port mirroring | Working — debugfs `mirror`; destination 6 (CPU) allows single-cable capture |
| PHY power / green Ethernet | Working — debugfs `phy_power` (5-bit mask), `green` (on/off) |
| Debug detectors (hung_task, softlockup, hardlockup) | Operational — exposed a real softlockup in `emulate_load_store_insn`, since resolved via `skb_reserve` alignment fix |
| RX burst recovery | Working — a 2000-frame burst fills the ring but the port recovers; runout no longer storms interrupts |

### Current status — gigabit working, hardware offload partially blocked

Gigabit link auto-negotiation at 1000 Mbps is now working via:
- `phy_refine()` — NCTL DSP calibration (`phy_98c_para[]` table from BSP)
- Explicit `mdio_write(id, 9, v | BIT(9))` — 1000BASE-T FD advertisement (PCRP NwayAbility bits alone are insufficient)
- `MACCR` SYSCLK set to 100MHz (lx_clk) + FCDSC enlarged to 48 pages
- Auto-down-speed detects 2-pair cables and correctly falls back to 100M

Throughput with a working 4-pair cable at 1G, zero-copy RX and
MTU-sized buffers: host-to-board TCP ~750 Mbit/s at MTU 9000 and
up to ~1.0 Gbit/s receiver with two streams or in clean single-stream
runs; 543-545 Mbit/s sender at MTU 1500; board-to-host ~370 Mbps.  Below
that the MIPS 1074Kc is the limit, not the wire.  A 2-pair
cable silently caps the link at 100M (the PHY's auto-down-speed drops the
1G advertisement), which is easy to mistake for a driver problem — check
`swdebug.sh phy 4 10`: bit 13 set means the link really is at 1G.

Hardware offload (checksum, scatter-gather, NAPT) is partially implemented:
- TX checksum offload works for IPv4 TCP/UDP: `CSCR` is programmed in
  `switch_l2_init()` and `eth_start_xmit()` sets `CSUM_TCPUDP`.  The ASIC
  leaves the IPv4 header checksum at zero, so the driver computes that in
  software (`ipv4_hdr_csum()`); IPv6 and anything else falls back to
  `skb_checksum_help()`
- NAPT works as the backend of nftables flowtable offload: 942 Mbit/s TCP
  and 500 Mbit/s UDP through masquerade with no data on the CPU ports.
  It needs `route_offload=1` and is source-NAT only; see the flow offload
  test above for the setup and limits
- `MACCR` bits 3:0 (CF_RXIPG) must not be 0: a bare `LONG_TXE` write
  makes the MAC drop back-to-back frames (TCP collapses to ~4% while
  pings pass).  The driver sets `MACCR_RXIPG` = 6
- Not implemented: `ndo_set_rx_mode` (bridge/promisc/allmulti) — a per-port
  design question; `FFCR` does not gate CPU delivery (measured)
- Register and bit details are commented where they are used in the driver;
  `RTL8198C.md` has the chip-level map and the programming-order rules

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
- **Two TX rings** — ring 0 for normal traffic, ring 1 for
  `skb->priority >= 4`.  Both are 256 entries and `DMA_CR1` is programmed
  with that one length; `CPUTPDCR1` = TX base + 1 KiB, `TXRINGCR` = 0x3.
  The pkthdr's `ph_queueId` follows the ring and `QNUMCR` gives every
  port two egress queues.  `CPUTPDCRn` read back as the hardware's
  current descriptor pointer, never as the written base.
- **Six RX rings** — one per switch queue, 256 entries each, sharing one
  mbuf ring of 6 * 256 entries (mbuf index = `ring * RX_RING_SIZE + slot`).
  `CPUQDM0/2/4` map queue n to ring n (32-bit writes only: the next slot
  is `DMA_CR0`), `CPUQIDMCR0` maps priority class to CPU queue, and the
  CPU port gets six queues in `QNUMCR`.  NAPI services every ring with a
  per-ring share, highest ring first.  Classification tables are zero by
  default; debugfs `dscp`/`rxmap` configure them.
- **Egress rate limiting** — per-port APR lives in `WFQRCRP0 + port * 0xC`,
  per-queue APR/L1/PPR in `PnQmRGCR`.  The egress queue comes from the TX
  pkthdr's `ph_txPriority` via `UPTCMCR1`; `ph_queueId` is RX-only.  Ingress
  `IBCR` writes land but have no effect (measured).
- **Jumbo frames** — 10 KB RX clusters, one per frame (no SG).  The cluster
  pool is six per-ring 2.6 MB blocks, not one 15.7 MB block: the latter is
  an order-12 allocation that fails on this board.  PCRP `AcptMaxLen` = 9K
  and the GPHY page 0 reg 23 bit 8 workaround are both required.
- **Netif table entry** — uses the non-8198C (8881A-compatible) format
  because the bootloader compiles without `CONFIG_RTL8198C` defined.
- **RX buffers** — one skb per mbuf; `rx_align_skb()` reserves 0..2 bytes
  so the IP header behind the 14-byte Ethernet header is 4-byte aligned
  (MIPS traps unaligned access into the emulator, which spinlocks under
  load).  The RISC writes at `skb->data`, so the buffer handed to the
  hardware is 2 mod 4, exactly the vendor's `RX_OFFSET = 2`.
- **RX buffer identity** — the RISC takes its buffer from the shared
  *linear* mbuf ring (one entry per frame, independent of the six pkthdr
  rings) and rewrites `ph_mbuf` to the entry it used, so received skbs
  are keyed by mbuf index, not descriptor slot.  After 256 frames the
  two are one ring apart.
- **CPUICR starts at open** — `alloc_rings()` programs the ring base
  registers but leaves the CPU interface disabled; otherwise the RISC
  fills descriptors before the first `eth_open()` and the ring cannot be
  resynchronised, because the pointers only reset while it is stopped.
- **RX buffer size follows the MTU** — 2 KB covers a 1500-MTU frame and
  lets `napi_alloc_skb()` use the page-fragment cache; jumbo needs the
  10 KB buffer.  The ring is shared, so the size is the maximum over all
  ports.  An MTU change stops only RX (`CPUICR` bit 30), reallocates,
  replumbs `CPURPDCRn`/`CPURMDCR0` and resets `rx_head`; TX keeps its
  ring pointers, so it keeps running across the resize.
- **TXFD doorbell is required** — dropping the per-packet `CPUICR |= TXFD`
  stops TX completely even with `TX_RING0/1_TAIL_AWARE` set in `DMA_CR4`:
  descriptors queue (`tx_head` advances) but `tx_tail` never moves and
  nothing reaches the wire.
- **TX scatter-gather** — one mbuf per page fragment chained through
  `m_next`; the last carries `MBUF_EOR` and four extra bytes for the CRC
  the switch appends.  Each TX descriptor slot has its own
  `MAX_SKB_FRAGS` chain, so in-flight packets never share fragment
  mbufs.  `frag_list` skbs and over-long chains are linearised
  (`tx_linearized`); `ethtool -K eth4 sg off` gives a clean A/B.
  Fragment pages must be cache-flushed individually.
- **VLAN tag delivery** — the CPU is untagged only in the native VLANs
  (8 and 9) so ordinary port traffic reaches the netifs without a tag.
  A VLAN created by a VLAN device keeps its tag, otherwise the 802.1Q
  layer cannot classify the frame and a stacked (QinQ) frame arrives
  with only its inner tag.  The ASIC parses an 0x8100 outer tag only:
  measured, an 0x88a8 frame is classified as untagged into the PVID, so
  802.1ad is rejected by `ndo_vlan_rx_add_vid`.
- **FFCR** — both `EN_UNMCAST_TOCPU` and `EN_UNUNICAST_TOCPU` are cleared.
  Measured behaviour: unknown unicast does *not* get flooded to other
  physical ports (port-to-port unicast fails); broadcasts do flood via
  the broadcast FDB entry, and the CPU still receives unknown destinations
  because it is a VLAN 8 member.  See RTL8198C.md bring-up notes.
- All `#if 0` / `#if 1` guards removed.  The driver is split into one
  file per hardware block; the old monolithic `rtl8198c_eth.c` is gone.

### Register write differences from bootloader

The driver now writes several registers the bootloader does not:
`DMA_CR0` (HsbAddrMark), `DMA_CR1`, `DMA_CR4`, `TXRINGCR`, `CPUICR1`,
`SWTCR0` (VLAN-based + EnUkVIDtoCPU), `MACCTRL1` (CMAC_CLK_SEL),
`CPUICR1` BIT(6) (CF_TX_GATHER), `CSCR` (checksum offload).
These are all confirmed correct and stable.

### KSEG1 vs Physical Addresses

| Value fed to... | Format | Reason |
|-----------------|--------|--------|
| **SWTAA** (TACI) | **KSEG1** (0xBBxxxxxx) | RISC TACI engine needs KSEG1 |
| CPURPDCR0 / CPUTPDCR0 (ring bases) | Either | Bootloader uses KSEG1 |
| debugfs `reg` accessor | Either | Physical (0x18xxxxxx) or KSEG1; only the SoC IO window 0x18000000-0x1BFFFFFF is accepted |
| C code register access | KSEG1 (volatile ptrs) | Standard MIPS uncached IO |
| pkthdr/mbuf pool pointers in ring entries | KSEG1 | RISC reads them directly |

### Networking debugging

**Live register dump:**
```
cat /sys/kernel/debug/rtl8198c-eth/switch/regs   # PSRP0-4 = per-port link
grep PSRP /sys/kernel/debug/rtl8198c-eth/switch/regs   # bit 4 = link up
cat /sys/class/net/eth4/carrier                  # 1 = reported, 0 = none
```

**Single register access (no /dev/mem; `devmem` is not built):**
```
echo 0x1B804000 > /sys/kernel/debug/rtl8198c-eth/switch/reg
cat /sys/kernel/debug/rtl8198c-eth/switch/reg   # "0xBB804000 0xXXXXXXXX"
echo 0x1B804428=0x403 > .../switch/reg          # write
swdebug.sh rd 0x1B804000                      # wrapper
swdebug.sh wr 0x1B804428 0x403
```

**swdebug.sh commands (on board):**
```
swdebug.sh swregs          # All switch core + CPUIF registers
swdebug.sh phy <id> <reg>  # Read PHY register
swdebug.sh phy_all         # Dump all 5 PHYs
swdebug.sh vlan_read [vid] # Read VLAN entry (decodes member/untag/fid)
swdebug.sh vlan_write <vid> <word0>  # Write VLAN entry (member/untag/fid)
swdebug.sh l2_read <row> <col>  # Read L2 entry (direct memory)
swdebug.sh l2_write_force <row> <col> <w0> <w1>  # Write L2 (CMD_FORCE)
swdebug.sh pcrp             # PCRP0-6 dump
swdebug.sh link             # eth0 link stats
swdebug.sh irq              # Interrupt count
swdebug.sh rd <addr>        # one 32-bit register (hex or physical)
swdebug.sh wr <addr> <val>  # write one 32-bit register
ethtool -S eth0             # driver counters + per-port link / 1G status
ethtool -S eth0 | grep -E "tx_ring|rx_ring|rx_queue"  # per-ring / per-queue counters
echo 12500000 > /sys/class/net/eth0/queues/tx-0/tx_maxrate  # 100 Mbit/s port egress
ip link set eth4 mtu 9000          # jumbo (both ends; 1500-9000)
ethtool -s eth0 ...         # force speed/duplex or advertise; ethtool -r to renegotiate
bridge fdb show dev eth4    # switch FDB (static + learned)
bridge mdb show dev br0     # groups marked "offload" are in the L2 table
bridge mdb add|del dev br0 port eth4 grp 239.1.2.3   # drives the mcast table
bridge fdb add|del <mac> dev eth4   # static entry on the port named
bridge fdb flush dev eth4   # drop learned entries, keep static ones
ip link set eth4 address <mac>      # moves the L2 + netif entries
echo 1 > .../switch/route_offload   # IPv4 route offload (bypasses netfilter/NAT)
cat .../switch/l3           # route/ARP/next-hop tables, FIB mirror, slot per route
echo "read 2 0" > .../switch/taci; cat .../switch/taci   # raw TACI entry (route 0)
echo "read 1 257" > .../switch/taci; cat .../switch/taci # ARP block entry (gateway)
```

**Routing offload test** (two cables, one host: put the second adapter in a
network namespace, otherwise the kernel loops the packet back locally):
```
ip netns add peer
ip link set enxf8e43b6ff0b9 netns peer
ip -n peer addr add 192.168.3.1/24 dev enxf8e43b6ff0b9
ip -n peer link set enxf8e43b6ff0b9 up
ip -n peer route add default via 192.168.3.2
ip route add 10.9.0.0/24 via 192.168.2.2
# board: fw4 stop; ip link set eth3 up; ip addr add 192.168.3.2/24 dev eth3
#        ip route add 10.9.0.0/24 via 192.168.3.1 dev eth3
#        echo 1 > /sys/kernel/debug/rtl8198c-eth/switch/route_offload
ping -c 3 10.9.0.1          # ~0.5 ms; tcpdump on eth3/eth4 shows no ICMP
```
When both addresses live in the same VLAN, the peer has to ARP for the
address owner's MAC (the netif MAC is the first address owner, `eth4` in
the default setup): `ip -n peer neigh replace 192.168.3.2 lladdr
ca:fe:ba:be:00:04 dev enxf8e43b6ff0b9`.

**Flow offload test (nftables masquerade)** - both knobs on, then a
flowtable with `flags offload`.  The nft chain name `fwd` is reserved;
use `forward`.
```
fw4 stop
ip link set eth3 up; ip addr add 192.168.3.2/24 dev eth3
ip route add 10.9.0.0/24 via 192.168.3.1 dev eth3
echo 1 > /sys/kernel/debug/rtl8198c-eth/switch/route_offload
echo 1 > /sys/kernel/debug/rtl8198c-eth/switch/flow_offload
cat > /tmp/t.nft <<'EOF'
table inet t {
 flowtable ft {
  hook ingress priority filter
  devices = { eth3, eth4 }
  flags offload
 }
 chain forward {
  type filter hook forward priority filter; policy accept
  ip protocol { tcp, udp } flow add @ft
 }
 chain post {
  type nat hook postrouting priority srcnat; policy accept
  oifname eth3 masquerade
 }
}
EOF
nft -f /tmp/t.nft
# host: ip route add 10.9.0.0/24 via 192.168.2.2
# peer netns: iperf3 -s -B 10.9.0.1 -1 -D
# host: iperf3 -c 10.9.0.1 -t 5        # ~940 Mbit/s, 0 data frames on eth3/4
cat /sys/kernel/debug/rtl8198c-eth/switch/l4   # flows with both cookies
nft delete table inet t                # unbind: flushes every row
```
The firewall sees only the first packet of an offloaded flow: after that
it is not filtered, tracked or counted, and `conntrack` counters stop
advancing.  Source NAT only - a router that needs port forwards must
leave `route_offload` off, because their replies are routed before the
kernel can offer them - and ICMP is punted and NAT'd in software.
`flow_offload` off flushes the rows, so toggle it with `route_offload`,
not under traffic.

**Hardware STP port states** (bridge them, then drive the states):
```
ip link add br0 type bridge ; ip link set eth4 master br0
swdebug.sh rd 0x1B804410    # MSCR: bit5 (EN_STP) set while bridged
swdebug.sh rd 0x1B804408    # RMACR: bit0 traps BPDUs to the CPU
swdebug.sh rd 0x1B804114    # PCRP4 bits5:4: 0 disable, 1 block, 2 learn, 3 forward
bridge link set dev eth4 state 0    # data stops, BPDUs still arrive (trap)
bridge link set dev eth4 state 3
ip link set br0 type bridge stp_state 1 forward_delay 2
                                    # kernel STP then steps listen/learn/forward
```
A manual state only sticks while the bridge's own STP is off - with
`stp_state 1` the bridge owns the states.  Testing an IP on `br0` needs
`fw4 stop`: the new interface is not in a firewall zone, so the input
policy rejects it with ICMP port-unreachable.

**Debugfs knobs** (`/sys/kernel/debug/rtl8198c-eth/switch/` - one set for the
whole switch, not per netdev):
```
fdb          # cat: whole L2 table with fid, port mask and age
             # echo "add <mac> [portmask] [fid]" / "del <mac> [fid]" / flush
l2_aging     # echo on|off|fast        global L2 aging
storm        # echo "broadcast on|off" / "multicast on|off" / "rate <0-100>"
mirror       # echo "<dest-port> <rx-mask> <tx-mask>"; 6 = CPU port
phy_power    # echo <5-bit mask>       power down PHYs
green        # echo on|off             Green Ethernet
pbvlan       # echo "user1 ethertype 0x88b5" / "ipx port 3 vid 20" / "... port 3 off"
lag          # read-only: why hardware link aggregation is unavailable
dscp         # echo "<dscp> <prio>" sets DSCP->priority (DSCPCR0-6)
rxmap        # echo "<queue> <ring>" maps a switch queue to an RX ring;
             # "reset" restores queue n -> ring n; read shows counters
rate         # echo "port <n> egress <kbps>"        per-port egress limit
             # echo "queue <n> <q> <kbps> [burst_kb] [ppr]"  per-queue limit
             # echo "reset" clears every limit; read shows the table
acl          # echo "add mac|ip ... action permit|drop|tocpu" / "del <n>" / "default drop" / flush
mcast        # echo "add 239.1.2.3 ports 0x08 [fid N] [cpu]" / "del <group>" / flush
route_offload# echo 0|1  let the ASIC route (bypasses netfilter and NAT)
flow_offload # echo 0|1  let nftables flowtables program the NAPT engine;
             # needs route_offload=1; off flushes every session and ext-IP
l4           # cat: engine state, TEATCR, external-IP and session mirror
             # echo "add tcp <int ip> <port> <rem ip> <port> <ext ip> <port>"
             # echo "del tcp <int ip> <port> <rem ip> <port>" / flush
             # echo "engine on|off"  raw arm/disarm (flow_offload is the knob)
```

Note: the switch's per-port MIB counters (SWCORE + 0x1000) are not decoded
on this SoC, so switch-level drops show up nowhere — see RTL8198C.md.
Storm-control percentages therefore come from the BSP's 30360-byte
constant rather than from a measured drop rate, and only the 0% / 100%
endpoints have been verified (a 300-frame broadcast flood reaches the
CPU with the meter off or at 100%, and not at all at 0%).

**Host debug scripts:**
```
~/test_traffic.sh           # Automated host-side packet testing
ping -I 192.168.2.1 192.168.2.2       # Host → board via enx00e04c680290
ping -I 192.168.3.1 192.168.2.1       # NOT a switch test: loops back locally
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
