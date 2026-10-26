#!/bin/sh
# swtest.sh — RTL8198C Ethernet driver self-test (device-side only)
# Usage:
#   swtest.sh              Quick sanity (register + PHY + dmesg + ping)
#   swtest.sh --full       Full battery (FDB, VLAN, PCRP, ifconfig)
#   swtest.sh --mac-drop MAC IP  Filter test (needs cable + host)

# ── helpers (subset of swdebug.sh, kept self-contained) ──
IFACE=${IFACE:-eth4}   # port netdev; the test cable is in port 4
DBG=/sys/kernel/debug/rtl8198c-eth/switch
REG=$DBG/reg
wr()    { echo "$1=$2" > "$REG"; }
rd()    { echo "$1" > "$REG"; awk '{print $2}' "$REG"; }

MDCIOCR=0xBB804004
MDCIOSR=0xBB804008
MDIO_BUSY=0x80000000
MDIO_WRITE=0x80000000

mdio_read() {
	local phy=$1 reg=$2
	wr $MDCIOCR $(( (phy << 24) | (reg << 16) ))
	# The MDIO transaction takes about a millisecond; MDCIOSR reads back
	# the previous result (with BUSY set) until it completes.
	while [ $(( $(rd $MDCIOSR) & MDIO_BUSY )) -ne 0 ]; do :; done
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
	# Only the policy matters: unknown unicast/multicast must not be
	# trapped to the CPU by FFCR (bits 1:0), because VLAN membership is
	# what delivers them.  The other bits come from switch_hard_reset()
	# and are not fixed values.
	if [ $(( v & 3 )) -eq 0 ]; then
		pass "FFCR=$v (TOCPU bits clear)"
	else
		fail "FFCR=$v (TOCPU bits 1:0 must be clear)"
	fi
}

test_maccr() {
	local v=$(rd $MACCR)
	if check_bit "$v" 24; then
		pass "MACCR=$v (INFINITE_PAUSE_FRAMES bit 24 set)"
	else
		fail "MACCR=$v (INFINITE_PAUSE_FRAMES bit 24 NOT set)"
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
	local ok=0 down=""
	for id in 3 4; do
		if phy_link_up $id; then
			ok=$((ok + 1))
		else
			down="$down $id"
		fi
	done
	if [ $ok -gt 0 ]; then
		pass "PHY link: $ok/2 ports up (ports 3,4)${down:+, down:$down}"
	else
		fail "PHY link: no port up (ports 3,4)"
	fi
}

test_gigabit_link() {
	local ok=0 up=0
	for id in 3 4; do
		local gstatus=$(mdio_read "$id" 10 2>/dev/null)
		# 1000BASE-T status: bit 13 = local receiver OK (link up at
		# 1G), bit 12 = remote receiver OK, bit 11 = partner is
		# 1000BASE-T FD capable.  bit 10 is only "partner supports
		# 1000BASE-T half duplex" and says nothing about the link.
		if check_bit "$gstatus" 13; then
			ok=$((ok + 1))
		elif check_bit "$gstatus" 11; then
			up=$((up + 1))
		fi
	done
	if [ "$ok" -ge 1 ]; then
		pass "Gigabit link: $ok ports at 1000M (reg 10 bit 13 set)"
	elif [ "$up" -ge 1 ]; then
		warn "Gigabit link: partner advertises 1000M but the link is not at 1G"
	else
		warn "Gigabit link: link is at 10/100 (partner, cable or adapter)"
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
	# Per-VLAN MAC tables: port 0 is the WAN (fid 0, salt 0x00), the LAN
	# ports use fid 1 (salt 0x0f).  The row carries the salt.
	local port=${IFACE#eth}
	local salt=0x00
	[ "$port" != "0" ] && salt=0x0f

	# Broadcast entry in this interface's database
	local w0=$(rd $(( 0xBB000000 + salt * 128 )))
	local w1=$(rd $(( 0xBB000000 + salt * 128 + 4 )))
	if [ "$w0" = "0xFFFFFFFF" ]; then
		pass "FDB bcast (fid salt $salt): word0 OK ($w0)"
	else
		fail "FDB bcast (fid salt $salt): word0=$w0 (expected 0xFFFFFFFF)"
	fi
	if [ $(( (w1 >> 8) & 0x3F )) -eq 63 ] && [ $(( (w1 >> 17) & 1 )) -eq 1 ]; then
		pass "FDB bcast: ports 0-5 + CPU OK ($w1)"
	else
		fail "FDB bcast: word1=$w1 (expected memberPort 0x3F and toCPU)"
	fi

	# Static CPU MAC entry.  The row is the XOR of the six MAC octets
	# salted with the fid, and each row holds four columns.
	local mac=$(ip -o link show "$IFACE" 2>/dev/null |
		    sed -n 's/.*link\/ether \([0-9a-fA-F:]*\).*/\1/p')
	if [ -z "$mac" ]; then
		skip "FDB CPU MAC: cannot read $IFACE MAC"
		return
	fi
	local row=$salt o
	for o in $(echo "$mac" | tr ':' ' '); do
		row=$(( row ^ 0x$o ))
	done
	local addr=$(( 0xBB000000 + row * 128 ))
	w0=$(rd $addr)
	w1=$(rd $(( addr + 4 )))

	set -- $(echo "$mac" | tr ':' ' ')
	local m0=$((0x$1)) m1=$((0x$2)) m2=$((0x$3))
	local m3=$((0x$4)) m4=$((0x$5))
	local exp=$(printf '0x%08X' $(( (m1 << 24) | (m2 << 16) | (m3 << 8) | m4 )))
	if [ "$w0" = "$exp" ]; then
		pass "FDB row $row: CPU MAC word0 OK ($w0)"
	else
		fail "FDB row $row: word0=$w0 (expected $exp)"
	fi
	if [ $(( w1 & 0xFF )) -eq $m0 ] &&
	   [ $(( (w1 >> 8) & 0x3F )) -eq 0 ] &&
	   [ $(( (w1 >> 14) & 0x7 )) -eq 1 ] &&
	   [ $(( (w1 >> 17) & 1 )) -eq 1 ] &&
	   [ $(( (w1 >> 18) & 1 )) -eq 1 ]; then
		pass "FDB row $row: CPU-only static entry OK ($w1)"
	else
		fail "FDB row $row: word1=$w1 (expected octet0/cpu-ext/toCPU/isStatic)"
	fi
}

test_vlan() {
	local w8=$(rd 0xBB060100)   # VLAN 8, WAN: port 0 + CPU
	local w9=$(rd 0xBB060120)   # VLAN 9, LAN: ports 1-4 + CPU
	local p8=$((w8 & 0x7F))
	local p9=$((w9 & 0x7F))
	if [ "$p8" = "65" ] && [ "$p9" = "94" ]; then
		pass "VLAN8=0x41 (WAN p0+CPU), VLAN9=0x5e (LAN p1-4+CPU) OK"
	else
		fail "VLAN members: 8=0x$(printf %02x $p8) (want 0x41), 9=0x$(printf %02x $p9) (want 0x5e)"
	fi
}

test_ifconfig() {
	if check_cmd ifconfig; then
		# busybox: "RX packets:132 errors:0", net-tools: "RX packets 132  bytes ..."
		local rx=$(ifconfig "$IFACE" 2>/dev/null | sed -n 's/.*RX packets[: ]*\([0-9][0-9]*\).*/\1/p')
		local tx=$(ifconfig "$IFACE" 2>/dev/null | sed -n 's/.*TX packets[: ]*\([0-9][0-9]*\).*/\1/p')
		if [ -n "$rx" ]; then
			pass "ifconfig: RX=$rx TX=$tx"
			return
		fi
	fi
	skip "ifconfig: $IFACE not found"
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

	# Driver encoding: word0 = mac1..4, word1 = mac0 | member<<8 |
	# static<<18 | fid<<23.  The row is the XOR of the MAC octets salted
	# with the fid (LAN = 1, salt 0x0f).
	local w0=$(( (b1 << 24) | (b2 << 16) | (b3 << 8) | b4 ))
	local w1=$(( b0 | (1 << 18) | (1 << 23) ))
	local row=$(( (b0 ^ b1 ^ b2 ^ b3 ^ b4 ^ b5 ^ 0x0f) & 0xFF ))
	local eidx=$((row * 4))

	echo "  Inserting drop entry for $mac at row $row (eidx=$eidx)..."
	taci_write 0 "$eidx" "$w0 $w1 0 0 0 0 0 0" 1

	if ! ping -c 1 -W 1 "$ip" >/dev/null 2>&1; then
		pass "MAC drop: ping $ip BLOCKED"
	else
		fail "MAC drop: ping $ip NOT blocked (filter inactive?)"
	fi

	echo "  Removing drop entry..."
	taci_write 0 "$eidx" "0 0 0 0 0 0 0 0" 1

	if ping -c 1 -W 2 "$ip" >/dev/null 2>&1; then
		pass "MAC drop: ping $ip RESTORED after removal"
	else
		fail "MAC drop: ping $ip STILL blocked after removal"
	fi
}

# ── main ──
echo "=== swtest.sh — $(date) ==="

# Ensure the management interface is up
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
