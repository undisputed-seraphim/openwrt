PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

RAMFS_COPY_BIN='fw_printenv fw_setenv'
RAMFS_COPY_DATA='/etc/fw_env.config /var/lock/fw_printenv.lock'

askey_ap5100w_check_image() {
	local tar_file="$1"
	local board_dir ksize rsize kmtd rmtd

	board_dir=$(tar tf "$tar_file" | grep -m 1 '^sysupgrade-.*/$')
	board_dir="${board_dir%/}"
	[ -n "$board_dir" ] || return 1

	ksize=$(tar xOf "$tar_file" "$board_dir/kernel" | wc -c)
	rsize=$(tar xOf "$tar_file" "$board_dir/root" | wc -c)
	[ "$ksize" -gt 0 ] && [ "$rsize" -gt 0 ] || return 1

	# The bootloader partition table is fixed: 6 MiB of kernel and 24 MiB
	# of rootfs.  mtd only notices at the end of the partition, after it
	# has already erased what it wrote, so refuse the image up front.
	kmtd=$(sed -n 's/mtd[0-9]*: \([0-9a-f]*\).*"linux".*/\1/p' /proc/mtd)
	rmtd=$(sed -n 's/mtd[0-9]*: \([0-9a-f]*\).*"rootfs".*/\1/p' /proc/mtd)
	[ -n "$kmtd" ] && [ -n "$rmtd" ] || return 1

	if [ "$ksize" -gt "$((0x$kmtd))" ]; then
		echo "AP5100W: kernel image ($ksize bytes) does not fit the $((0x$kmtd))-byte linux partition" >&2
		return 74
	fi
	if [ "$rsize" -gt "$((0x$rmtd))" ]; then
		echo "AP5100W: rootfs image ($rsize bytes) does not fit the $((0x$rmtd))-byte rootfs partition" >&2
		return 74
	fi
}

askey_ap5100w_do_upgrade() {
	local tar_file="$1"
	local board_dir

	sync
	board_dir=$(tar tf "$tar_file" | grep -m 1 '^sysupgrade-.*/$')
	board_dir="${board_dir%/}"

	tar xOf "$tar_file" "$board_dir/kernel" | mtd write - linux

	# rootfs_data is a squashfs-split partition: its start is the new
	# rootfs size rounded up to the erase block size, so it moves when
	# the image size changes.  Without erasing the tail, the new
	# rootfs_data starts inside the old rootfs or old JFFS2, fstools
	# identifies it as FS_NONE and falls back to a tmpfs overlay.
	# Erase the whole partition; mtd write only touches the blocks it
	# needs, leaving the future overlay area blank so fstools formats
	# it as JFFS2 on the next boot.
	mtd erase rootfs
	tar xOf "$tar_file" "$board_dir/root" | mtd write - rootfs
}

# The TP-Link bootloader gets its flash layout from "bootargs".
# Use this to our advantage, and:
#  1. Disable the second rootfs (usrimg2)
#  2. Extend the first rootfs (usrimg1) to include more space
#  3. Increase the baudrate to the expected 115200
tplink_sg2xxx_fix_mtdparts() {
	local args
	args="bootargs mtdparts=spi_flash:896K(boot),128K(env),6144K(sys)"
	args="$args,20480K(usrimg1@main),0K(usrimg2)"
	args="$args,4096K(usrappfs),1024K(para)\n"
	args="$args baudrate 115200"

	echo -e "$args" | fw_setenv --script -
}

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	askey,ap5100w)
		askey_ap5100w_check_image "$1" || return
		;;
	draytek,g2282x)
		draytek_g2282x_set_part_name || return 1
		;;
	esac

	return 0
}

# Active slot is a "bootpartition" variable in the SYSINFO mtd
# partition, not standard U-Boot env. Self-upgrades the active slot
# instead of flipping it.
draytek_g2282x_set_part_name() {
	local bootpart sysinfo
	sysinfo=$(find_mtd_part SYSINFO)
	bootpart=$(sed -n 's/.*bootpartition=\([01]\).*/\1/p' "${sysinfo:-/dev/null}")
	case "$bootpart" in
	0) PART_NAME="Kernel" ;;
	1) PART_NAME="Kernel2" ;;
	*)
		echo "draytek,g2282x: could not read bootpartition from SYSINFO" >&2
		return 1
		;;
	esac
}

platform_do_upgrade() {
	local board=$(board_name)

	case "$board" in
	draytek,g2282x)
		draytek_g2282x_set_part_name || return 1
		default_do_upgrade "$1"
		;;
	plasmacloud,esx28|\
	plasmacloud,mcx3|\
	plasmacloud,psx8|\
	plasmacloud,psx10|\
	plasmacloud,psx28)
		PART_NAME="inactive"
		platform_do_upgrade_dualboot_plasmacloud "$1"
		;;
	tplink,sg2008p-v1|\
	tplink,sg2008p-v3|\
	tplink,sg2210p-v3)
		tplink_sg2xxx_fix_mtdparts
		default_do_upgrade "$1"
		;;
	linksys,lgs328c|\
	linksys,lgs328mpc-v2|\
	linksys,lgs352c)
		nand_do_upgrade "$1"
		;;
	askey,ap5100w)
		askey_ap5100w_do_upgrade "$1"
		;;
	zyxel,xgs1930-28hp|\
	zyxel,xmg1915-10e|\
	zyxel,xmg1915-10ep|\
	zyxel,xs1930-10|\
	zyxel,xs1930-12f|\
	zyxel,xs1930-12hp)
		PART_NAME="factory"
		default_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
