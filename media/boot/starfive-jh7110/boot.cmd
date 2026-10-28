# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Ahmed ARIF <arif193@gmail.com>
echo "NextNT: looking for FreeLoader on USB storage"
pci enum
usb start
for nextnt_dev in 0 1 2 3 4 5 6 7; do
	if test -e usb ${nextnt_dev}:1 efi/boot/bootriscv64.efi; then
		echo "NextNT: starting FreeLoader from usb ${nextnt_dev}:1"
		setenv nextnt_fdt ${fdtcontroladdr}
		if load usb ${nextnt_dev}:1 ${fdt_addr_r} dtb/starfive/jh7110-orangepi-rv.dtb; then
			setenv nextnt_fdt ${fdt_addr_r}
		fi
		load usb ${nextnt_dev}:1 ${kernel_addr_r} efi/boot/bootriscv64.efi
		bootefi ${kernel_addr_r} ${nextnt_fdt}
	fi
done
echo "NextNT: no USB disk holds efi/boot/bootriscv64.efi"
