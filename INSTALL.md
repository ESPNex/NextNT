# Installing NextNT

## Building the images

Set up the build environment and configure a build as described in [Building](README.md#building). Then build the image you need from the output folder, for example `output-Clang-amd64-debug`:

| Command | Creates | Use it to |
|---|---|---|
| `ninja bootcd` | `bootcd.iso` | Run NextNT from a CD image, or start NextNT Setup from it and install NextNT on a disk. |
| `ninja nextntimg` | `NextNT.img` | Boot a disk with NextNT already installed. It boots with UEFI or BIOS. |
| `ninja nextntvhd` | `NextNT.vhd`, `NextNT.vhdx` | Use the same preinstalled disk in virtual machines: the VHD in VirtualBox and Hyper-V Generation 1, the VHDX in Hyper-V Generation 2, which does not take VHD disks. |

Each target builds everything it needs first, and the files are written to the output folder.

## Installing from the setup CD

Boot `bootcd.iso` in a virtual machine or on a PC, and follow the instructions of NextNT Setup. Setup lists the drives the way Windows Setup does, and can create, delete, format, shrink and extend partitions. It installs NextNT on NTFS and creates the EFI system partition it needs.

## Using the preinstalled disk

`NextNT.img` needs no setup. Start it in a virtual machine such as QEMU, or write it to a USB drive or an SD card and boot the machine from it. Use `NextNT.vhd` for virtual machines that take VHD disks, and `NextNT.vhdx` for Hyper-V Generation 2 virtual machines, with Secure Boot turned off.

The preinstalled disk also carries NextNT Setup. Run `C:\<architecture>\nextnt.exe`, for example `C:\arm64\nextnt.exe`, to install NextNT on another disk from the running system.

## Help

If something goes wrong, report it in [Issues](https://github.com/espnex/nextnt/issues) with the architecture, the build type, the commit you built and the steps that lead to the problem. Guides and notes are in the [Wiki](https://github.com/espnex/nextnt/wiki), and you can ask the team on [Discord](https://discord.gg/BpcKg7FZ2) or [Matrix](https://matrix.to/#/#nextnt:matrix.org).
