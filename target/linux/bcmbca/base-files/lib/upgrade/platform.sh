# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

RAMFS_COPY_BIN="fw_printenv fw_setenv"

PART_NAME=firmware

platform_check_image() {
	[ "$#" -gt 1 ] && return 1
	return 0
}

platform_do_upgrade() {
	case "$(board_name)" in
	arcadyan,wg660242-st)
		nand_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
