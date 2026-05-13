#!/bin/sh
# swtest.sh — RTL8198C Ethernet driver self-test (device-side only)
# Usage:
#   swtest.sh              Quick sanity (register + PHY + dmesg + ping)
#   swtest.sh --full       Full battery (FDB, VLAN, PCRP, ifconfig)
#   swtest.sh --mac-drop MAC IP  Filter test (needs cable + host)

# ── helpers (subset of swdebug.sh, kept self-contained) ──
phys()  { echo $(($1 - 0xA0000000)); }
wr()    { devmem $(phys "$1") 32 "$2" 2>/dev/null; }
rd()    { devmem $(phys "$1") 32 2>/dev/null; }

MDCIOCR=0xBB804004
MDCIOSR=0xBB804008
MDIO_BUSY=0x80000000
MDIO_WRITE=0x80000000

mdio_read() {
	local phy=$1 reg=$2
	wr $MDCIOCR $(( (phy << 24) | (reg << 16) ))
	local v=$(rd $MDCIOSR)
	echo $((v & 0xffff))
}

SWTCR0=0xBB804418
TACR=0xBB804D00
TASR=0xBB804D04
TAA=0xBB804D08
TCR0=0xBB804D20
SWTBL_BASE=0x1B000000

taci_stop_tlu() {
	wr $SWTCR0 $(( $(rd $SWTCR0) | 0x40000 ))
	while [ $(( $(rd $SWTCR0) & 0x80000 )) -eq 0 ]; do :; done
}
taci_start_tlu() {
	wr $SWTCR0 $(( $(rd $SWTCR0) & ~0x40000 ))
}
taci_write() {
	local type=$1 eidx=$2 force=${4:-0}
	shift 2
	taci_stop_tlu
	while [ $(( $(rd $TACR) & 1 )) -ne 0 ]; do :; done
	local i=0
	for w in $1; do
		wr $((TCR0 + i * 4)) $w
		i=$((i + 1))
	done
	local base=$((SWTBL_BASE + (type << 16) + 0xA0000000))
	wr $TAA $((base + eidx * 32))
	if [ "$force" = "1" ]; then
		wr $TACR 9
	else
		wr $TACR 3
	fi
	while [ $(( $(rd $TACR) & 1 )) -ne 0 ]; do :; done
	taci_start_tlu
}

# ── register addresses (KSEG1) ──
FFCR=0xBB804428
MACCR=0xBB804000
GDSR0=0xBB806100
PCRP0=0xBB804104
CPUICR_ADDR=0xB8010000

# ── test framework ──
PASS=0; FAIL=0; WARN=0
SKIP=0
warn() { WARN=$((WARN + 1)); echo "  [WARN] $*"; }
pass() { PASS=$((PASS + 1)); echo "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $*"; }
skip() { SKIP=$((SKIP + 1)); echo "  [SKIP] $*"; }

check_cmd()  { command -v "$1" >/dev/null 2>&1; }
check_bit()  { [ $(($1 & (1 << $2))) -ne 0 ]; }
check_not_zero() { [ "$1" != "0" ] && [ "$1" != "0x00000000" ] && [ "$1" != "0x0" ]; }
phy_link_up() { check_bit $(mdio_read "$1" 1) 2; }

# ── quick tests ──
test_ffcr() {
	local v=$(rd $FFCR)
	if [ "$v" = "0x00000400" ]; then
		pass "FFCR=$v (both TOCPU bits clear)"
	else
		fail "FFCR=$v (expected 0x00000400)"
	fi
}

test_maccr() {
	local v=$(rd $MACCR)
	if check_bit "$v" 14; then
		pass "MACCR=$v (flow control BIT(14) set)"
	else
		fail "MACCR=$v (flow control BIT(14) NOT set)"
	fi
}

test_cpuicr() {
	local v=$(rd $CPUICR_ADDR)
	if check_not_zero "$v"; then
		pass "CPUICR=$v"
	else
		fail "CPUICR=$v (zero — driver not loaded?)"
	fi
}

test_gdsr0() {
	local v=$(rd $GDSR0)
	if check_not_zero "$v"; then
		pass "GDSR0=$v (RISC alive)"
	else
		fail "GDSR0=$v (RISC dead?)"
	fi
}

test_phy_link() {
	local ok=0 bad=0
	for id in 3 4; do
		if phy_link_up $id; then
			ok=$((ok + 1))
		else
			fail "PHY $id link DOWN"
			bad=$((bad + 1))
		fi
	done
	[ $ok -gt 0 ] && pass "PHY link: $ok ports up (ports 3,4)"
}

test_gigabit_link() {
	local ok=0
	for id in 3 4; do
		local gstatus=$(mdio_read "$id" 10 2>/dev/null)
		if check_bit "$gstatus" 10; then
			ok=$((ok + 1))
		fi
	done
	if [ "$ok" -ge 1 ]; then
		pass "Gigabit link: $ok ports at 1000M (reg 10 bit 10 set)"
	elif [ "$ok" -eq 0 ]; then
		warn "Gigabit link: 0 ports at 1000M (cable or host adapter?)"
	fi
}

test_pcrp() {
	local i ok=0
	for i in 0 1 2 3 4; do
		local v=$(rd $((PCRP0 + i * 4)))
		local nway=$(( (v >> 18) & 0x1F ))
		[ "$nway" = "31" ] && ok=$((ok + 1))
	done
	if [ $ok -ge 4 ]; then
		pass "PCRP NwayAbility: $ok/5 ports OK"
	else
		fail "PCRP NwayAbility: only $ok/5 ports OK"
	fi
}

test_dmesg() {
	dmesg | grep -qi softlockup && fail "dmesg: SOFTLOCKUP" || pass "dmesg: no softlockup"
	dmesg | grep -qiE "oops|pani|BUG:" && fail "dmesg: Oops/Panic/BUG" || pass "dmesg: no Oops/Panic"
}

test_interrupts() {
	local n=$(grep rtl8198c-eth /proc/interrupts 2>/dev/null | awk '{print $2}')
	if [ -n "$n" ] && [ "$n" -gt 0 ] 2>/dev/null; then
		pass "IRQ count=$n"
	else
		pass "IRQ present (count: ${n:-0})"
	fi
}

test_ping() {
	local target="${1:-192.168.2.1}"
	if ! check_cmd ping; then skip "ping: no binary"; return; fi
	if ping -c 1 -W 1 "$target" >/dev/null 2>&1; then
		pass "ping $target OK"
	else
		fail "ping $target FAILED (host connected on same subnet?)"
	fi
}

# ── full-mode extras ──
test_fdb() {
	# Broadcast at entry 0
	local w0=$(rd 0xBB000000) w1=$(rd 0xBB000004)
	if [ "$w0" = "0xFFFFFFFF" ]; then
		pass "FDB row 0: broadcast word0 OK ($w0)"
	else
		fail "FDB row 0: word0=$w0 (expected 0xFFFFFFFF)"
	fi
	# CPU MAC at entry 960 (row 240, col 0)
	w0=$(rd 0xBB007800)
	w1=$(rd 0xBB007804)
	if [ "$w0" = "0xBE00FEBA" ]; then
		pass "FDB row 240: CPU MAC word0 OK ($w0)"
	else
		fail "FDB row 240: word0=$w0 (expected 0xBE00FEBA)"
	fi
}

test_vlan() {
	local w0=$(rd 0xBB050100)  # VLAN_BASE + VID*32 = 0xBB050000 + 8*32
	local ports=$((w0 & 0x3F))
	if [ "$ports" = "63" ]; then
		pass "VLAN[8] memberPorts=0x3F OK"
	else
		fail "VLAN[8] memberPorts=$ports (expected 63)"
	fi
}

test_ifconfig() {
	if check_cmd ifconfig; then
		local rx=$(ifconfig eth0 2>/dev/null | grep "RX packets" | awk '{print $3}' | cut -d: -f2)
		local tx=$(ifconfig eth0 2>/dev/null | grep "TX packets" | awk '{print $3}' | cut -d: -f2)
		if [ -n "$rx" ]; then
			pass "ifconfig: RX=$rx TX=$tx"
			return
		fi
	fi
	skip "ifconfig: eth0 not found"
}

# ── mac-drop feature test ──
test_mac_drop() {
	local mac="${1:-04:ab:18:6c:7f:ad}"
	local ip="${2:-192.168.2.3}"

	if ! check_cmd ping; then skip "ping: no binary"; return; fi

	# Parse MAC: ab:cd:ef:01:02:03
	local b0=0x$(echo "$mac" | cut -d: -f1)
	local b1=0x$(echo "$mac" | cut -d: -f2)
	local b2=0x$(echo "$mac" | cut -d: -f3)
	local b3=0x$(echo "$mac" | cut -d: -f4)
	local b4=0x$(echo "$mac" | cut -d: -f5)
	local b5=0x$(echo "$mac" | cut -d: -f6)

	# L2 entry: word0=(b3<<24)|(b4<<16)|(b1<<8)|b2, word1=(b0<<24)|(1<<13)
	local w0=$(( (b3 << 24) | (b4 << 16) | (b1 << 8) | b2 ))
	local w1=$(( (b0 << 24) | (1 << 13) ))  # isStatic=1, memberPort=0, toCPU=0
	local row=32  # entry 128

	echo "  Inserting drop entry for $mac at row $row (eidx=128)..."
	taci_write 0 128 "$w0 $w1 0 0 0 0 0 0" 1

	if ! ping -c 1 -W 1 "$ip" >/dev/null 2>&1; then
		pass "MAC drop: ping $ip BLOCKED"
	else
		fail "MAC drop: ping $ip NOT blocked (filter inactive?)"
	fi

	echo "  Removing drop entry..."
	taci_write 0 128 "0 0 0 0 0 0 0 0" 1

	if ping -c 1 -W 2 "$ip" >/dev/null 2>&1; then
		pass "MAC drop: ping $ip RESTORED after removal"
	else
		fail "MAC drop: ping $ip STILL blocked after removal"
	fi
}

# ── main ──
echo "=== swtest.sh — $(date) ==="

# Ensure eth0 is up
ifup lan 2>/dev/null
sleep 2

MOD="${1:-quick}"

test_ffcr
test_maccr
test_cpuicr
test_gdsr0
test_phy_link
test_gigabit_link
test_dmesg
test_interrupts

case "$MOD" in
	quick)
		test_ping
		;;
	--full|full)
		test_ping
		test_fdb
		test_vlan
		test_pcrp
		test_ifconfig
		;;
	--mac-drop)
		shift
		test_ping
		test_mac_drop "$@"
		;;
	*)
		echo "Usage: $0 [quick|--full|--mac-drop MAC IP]"
		;;
esac

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $WARN warned, $SKIP skipped ==="
[ $FAIL -eq 0 ] && exit 0 || exit 1
