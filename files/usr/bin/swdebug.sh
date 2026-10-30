#!/bin/sh
# swdebug.sh — RTL8198C switch core / GPHY / descriptor debug tool
# Uses busybox devmem (physical addresses, NOT KSEG1)
set -e

# ── physical address helpers ──
# KSEG1 = physical + 0xA0000000.  devmem uses physical addresses.
# Base registers (physical addresses from KSEG1 - 0xA0000000)
CPU_IF_BASE=0x18010000
SWCORE_BASE=0x1B800000
SWTBL_BASE=0x1B000000

# System registers
SIRR=0x1B804204
GIMR=0x18003000
SYS_CLK_MAG=0x18000010
REVR=0x18000000

# CPU Interface registers (offsets from CPU_IF_BASE)
CPUICR=0x18010000
CPURPDCR0=0x18010004
CPUTPDCR0=0x18010020
CPUIIMR=0x18010028
CPUIISR=0x1801002C
DMA_CR0=0x1801003C
DMA_CR1=0x18010040
DMA_CR4=0x180100A0
TXRINGCR=0x18010078

# Switch core L2 registers
MSCR=0x1B804410
SWTCR0=0x1B804418
FFCR=0x1B804428
VCR0=0x1B804A00
MACCR=0x1B804000
CSCR=0x1B804048
ALECR=0x1B80440C
QNUMCR=0x1B804754
GDSR0=0x1B806100
# PCRP ports 0-6
PCRP0=0x1B804104
PCRP6=0x1B80411C
# MDIO
MDCIOCR=0x1B804004
MDCIOSR=0x1B804008
# TACI (Table Access Control Interface)
TACR=0x1B804D00
TASR=0x1B804D04
TAA=0x1B804D08
TCR0=0x1B804D20
# VLAN table base (type 5 in bootloader layout, = 0xBB050000 phys 0x1B050000)
VLAN_BASE=0x1B050000
# L2 table base (type 0) — TAA requires KSEG1 (0xBB000000), NOT physical!
# Physical base used for direct memory reads.
L2_BASE=0x1B000000

rd()  { devmem "$1" 32; }
wr()  { devmem "$1" 32 "$2" >/dev/null; }
phys() { echo $(($1 - 0xA0000000)); }  # KSEG1 → physical, kept for ref

# ── MDIO helpers ──
mdio_read() { # phy reg
	wr $MDCIOCR $(($1 << 24 | $2 << 16))
	while [ $(rd $MDCIOSR) -ge 2147483648 ]; do :; done
	rd $MDCIOSR
}
mdio_write() { # phy reg val
	wr $MDCIOCR $((0x80000000 | $1 << 24 | $2 << 16 | $3))
	while [ $(rd $MDCIOSR) -ge 2147483648 ]; do :; done
}

# ── TACI helpers ──
taci_stop_tlu() {
	wr $SWTCR0 $(($(rd $SWTCR0) | 0x40000))      # EN_STOP_TLU = BIT(18)
	while [ $(($(rd $SWTCR0) & 0x80000)) -eq 0 ]; do :; done  # wait STOP_TLU_READY
}
taci_start_tlu() {
	wr $SWTCR0 $(($(rd $SWTCR0) & ~0x40000))
}
taci_write() { # table_type eidx "w0 w1 w2 w3 w4 w5 w6 w7" [use_force]
	local type=$1 eidx=$2 force=${4:-0}
	shift 2
	taci_stop_tlu
	while [ $(($(rd $TACR) & 1)) -ne 0 ]; do :; done  # wait TACI idle
	local i=0
	for w in $1; do
		wr $(($TCR0 + $i * 4)) $w
		i=$((i + 1))
	done
	local base=$(($SWTBL_BASE + ($type << 16) + 0xA0000000))  # TAA must be KSEG1!
	local addr=$(($base + $eidx * 32))
	wr $TAA $addr
	if [ "$force" = "1" ]; then
		wr $TACR 9     # ACTION_START(1) | CMD_FORCE(8)
	else
		wr $TACR 3     # ACTION_START(1) | CMD_ADD(2)
	fi
	while [ $(($(rd $TACR) & 1)) -ne 0 ]; do :; done
	taci_start_tlu
}

# ── command dispatch ──
cmd_phy() {
	local phy=${1:-8} reg=${2:-1}
	echo "PHY $phy reg $reg: $(mdio_read $phy $reg)"
}
cmd_phy_all() {
	local phy reg
	for phy in 8 1 2 3 4; do
		echo "=== PHY $phy ==="
		for reg in 0 1 4 5 9 10 17; do
			printf "  reg %2d: 0x%08X\n" $reg $(mdio_read $phy $reg)
		done
	done
}
cmd_swregs() {
	echo "CPUICR:      $(rd $CPUICR)"
	echo "CPURPDCR0:   $(rd $CPURPDCR0)"
	echo "CPUTPDCR0:   $(rd $CPUTPDCR0)"
	echo "DMA_CR0:     $(rd $DMA_CR0)"
	echo "DMA_CR1:     $(rd $DMA_CR1)"
	echo "DMA_CR4:     $(rd $DMA_CR4)"
	echo "TXRINGCR:    $(rd $TXRINGCR)"
	echo "GDSR0:       $(rd $GDSR0)"
	echo "SIRR:        $(rd $SIRR)"
	echo "GIMR:        $(rd $GIMR)"
	echo "MSCR:        $(rd $MSCR)"
	echo "SWTCR0:      $(rd $SWTCR0)"
	echo "FFCR:        $(rd $FFCR)"
	echo "VCR0:        $(rd $VCR0)"
	echo "MACCR:       $(rd $MACCR)"
	echo "CSCR:        $(rd $CSCR)"
	echo "ALECR:       $(rd $ALECR)"
	echo "QNUMCR:      $(rd $QNUMCR)"
	local p
	for p in 0 1 2 3 4 6; do
		printf "PCRP%d:       %s\n" $p $(rd $(($PCRP0 + $p * 4)))
	done
}
cmd_pcrp() {
	local p
	for p in 0 1 2 3 4 6; do
		printf "PCRP%d:       %s\n" $p $(rd $(($PCRP0 + $p * 4)))
	done
}
cmd_vlan_read() {
	local vid=${1:-1}
	local addr=$(($VLAN_BASE + $vid * 32))
	local i
	echo "VLAN VID=$vid at 0x$(printf '%X' $addr):"
	for i in 0 1 2; do
		printf "  word %d: 0x%08X\n" $i $(rd $(($addr + $i * 4)))
	done
}
cmd_vlan_set() {
	# Create VLAN: swdebug.sh vlan_set VID
	local vid=${1:-1}
	echo "Creating VLAN VID=$vid (all ports 0-5 + CPU, untagged)..."
	taci_write 5 $vid "0xFE7F 0 0 0 0 0 0 0" 1
	cmd_vlan_read $vid
}
cmd_l2_read() {
	local row=${1:-0} col=${2:-0}
	local eidx=$((($row << 2) | $col))
	local addr=$(($L2_BASE + $eidx * 32))
	local i
	echo "L2 row=$row col=$col (eidx=$eidx, addr=0x$(printf '%X' $addr)):"
	for i in 0 1; do
		printf "  word %d: 0x%08X\n" $i $(rd $(($addr + $i * 4)))
	done
}
cmd_l2_write_force() {
	# Force-write L2 entry: swdebug.sh l2_write_force row col w0 w1 ...
	local row=${1:-0} col=${2:-0}
	shift 2
	local eidx=$((($row << 2) | $col))
	local words="$*"
	while [ ${#words} -lt 64 ]; do words="$words 0"; done
	echo "L2 force write row=$row col=$col (eidx=$eidx)..."
	taci_write 0 $eidx "$words" 1
	cmd_l2_read $row $col
}
cmd_link() {
	ifconfig eth0 2>/dev/null | grep -E "Link|RX|TX"
}
cmd_irq() {
	grep rtl8198c-eth /proc/interrupts
}
cmd_arping() {
	arping -I eth0 -c 3 "$@"
}
cmd_help() {
	cat <<'EOF'
swdebug.sh — RTL8198C switch core debug tool
Commands:
  phy <id> <reg>     Read PHY register (default phy=8 reg=1)
  phy_all            Dump all PHY registers for all 5 PHYs
  swregs             Dump all switch core / CPUIF registers
  pcrp               Dump per-port control registers (PCRP0-6)
  vlan_read [vid]    Read VLAN table entry (default vid=1)
  vlan_set [vid]     Create VLAN entry with all ports+CPU
  l2_read [row] [col]  Read L2 table entry (default row=0 col=0)
  l2_write_force <row> <col> <w0..w7>  Force-write L2 entry
  link               Show eth0 link stats
  irq                Show interrupt count
  arping <ip>        Send ARP request from eth0
EOF
}

case "${1:-help}" in
	phy)       cmd_phy "$2" "$3" ;;
	phy_all)   cmd_phy_all ;;
	swregs)    cmd_swregs ;;
	pcrp)      cmd_pcrp ;;
	vlan_read) cmd_vlan_read "$2" ;;
	vlan_set)  cmd_vlan_set "$2" ;;
	l2_read)   cmd_l2_read "$2" "$3" ;;
	l2_write_force) shift; cmd_l2_write_force "$@" ;;
	link)      cmd_link ;;
	irq)       cmd_irq ;;
	arping)    shift; cmd_arping "$@" ;;
	help|--help|-h) cmd_help ;;
	*)         cmd_help ;;
esac
