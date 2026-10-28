#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Ahmed ARIF <arif.ing@outlook.com>
#
# Builds the LGPL FFmpeg payload of a LiberNT release from a build directory
# configured with -DENABLE_FFMPEG=ON -DFFMPEG_FROM_SOURCE=ON:
#   libernt-ffmpeg-<release>-<arch>.zip  headers for the build, system32 DLLs, licenses
#   libernt-ffmpeg-<release>-<arch>.msi  the DLLs the second stage installs
# The MSI is written by wixl and msibuild (msitools).

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile
from pathlib import Path
from xml.sax.saxutils import quoteattr

DLLS = ("avutil-60.dll", "swresample-6.dll", "avcodec-62.dll", "avformat-62.dll")
LICENSE = "FFmpeg-COPYING.LGPLv2.1.txt"
MSI_ARCH = {
    "amd64": ("x64", "System64Folder", "yes", "x64;1033"),
    "arm64": ("x64", "System64Folder", "yes", "Arm64;1033"),
    "i386": ("x86", "SystemFolder", "no", "Intel;1033"),
}
UPGRADE_NAMESPACE = uuid.UUID("6c6f9a8e-2f1d-4b7e-9b52-4c1b8e0f3a77")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_zip(root, source_release, arch, release, output):
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for header in sorted((root / "include").rglob("*")):
            if header.is_file():
                archive.write(header, Path("include") / header.relative_to(root / "include"))
        for dll in DLLS:
            archive.write(root / "bin" / dll, Path("system32") / dll)
        archive.write(root / LICENSE, Path("licenses") / LICENSE)
        archive.writestr("payload.json", json.dumps({
            "name": "libernt-ffmpeg",
            "release": release,
            "arch": arch,
            "ffmpeg": source_release,
            "license": "LGPL-2.1-or-later",
            "system32": list(DLLS),
        }, indent=2) + "\n")


def write_msi(root, arch, release, version, output, work):
    wixl_arch, folder, win64, template = MSI_ARCH[arch]
    upgrade = str(uuid.uuid5(UPGRADE_NAMESPACE, arch)).upper()
    files = "\n".join(
        f'          <Component Id="c{i}" Guid="*" Win64="{win64}">'
        f'<File Id="f{i}" Source={quoteattr(str(root / "bin" / dll))} KeyPath="yes"/></Component>'
        for i, dll in enumerate(DLLS))
    refs = "\n".join(f'      <ComponentRef Id="c{i}"/>' for i in range(len(DLLS)))
    wxs = work / "ffmpeg.wxs"
    wxs.write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="LiberNT FFmpeg {release}" Language="1033" Version="{version}"
           Manufacturer="LiberNT" UpgradeCode="{upgrade}">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perMachine"/>
    <MajorUpgrade DowngradeErrorMessage="A newer LiberNT FFmpeg is already installed."/>
    <Media Id="1" Cabinet="ffmpeg.cab" EmbedCab="yes"/>
    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="{folder}">
{files}
      </Directory>
      <Directory Id="WindowsFolder">
        <Directory Id="ThirdParty" Name="3rdParty">
          <Component Id="license" Guid="*" Win64="{win64}"><File Id="license" Source={quoteattr(str(root / LICENSE))} KeyPath="yes"/></Component>
        </Directory>
      </Directory>
    </Directory>
    <Feature Id="Codecs" Level="1">
{refs}
      <ComponentRef Id="license"/>
    </Feature>
  </Product>
</Wix>
''')
    subprocess.run(["wixl", "-a", wixl_arch, "-o", str(output), str(wxs)], check=True)
    if arch == "arm64":
        subprocess.run(["msibuild", str(output), "-s", f"LiberNT FFmpeg {release}", "LiberNT",
                        template, "{" + str(uuid.uuid4()).upper() + "}"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--arch", required=True, choices=sorted(MSI_ARCH))
    parser.add_argument("--release", required=True, help="payload release, e.g. n8.1.3-1")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--no-build", action="store_true", help="package an existing ffmpeg-install")
    args = parser.parse_args()

    for tool in ("wixl", "msibuild"):
        if not shutil.which(tool):
            sys.exit(f"{tool} (msitools) is required")
    if not args.no_build:
        subprocess.run(["ninja", "-C", str(args.build_dir), "ffmpeg"], check=True)

    root = args.build_dir / "submodules" / "ffmpeg-install"
    missing = [str(root / "bin" / dll) for dll in DLLS if not (root / "bin" / dll).is_file()]
    if missing or not (root / LICENSE).is_file() or not (root / "include" / "libavcodec" / "avcodec.h").is_file():
        sys.exit(f"{root} is not a complete FFmpeg install: {missing}")

    cache = (args.build_dir / "CMakeCache.txt").read_text()
    source_dir = Path(next(line.split("=", 1)[1] for line in cache.splitlines()
                           if line.startswith("CMAKE_HOME_DIRECTORY:")))
    source_release = (source_dir / "submodules" / "ffmpeg" / "RELEASE").read_text().strip()
    numbers = [int(part) for part in source_release.split(".")[:3]]
    numbers += [0] * (3 - len(numbers))
    version = ".".join(str(n) for n in numbers + [int(args.release.rsplit("-", 1)[1])])

    args.out.mkdir(parents=True, exist_ok=True)
    stem = f"libernt-ffmpeg-{args.release}-{args.arch}"
    with tempfile.TemporaryDirectory() as work:
        write_zip(root, source_release, args.arch, args.release, args.out / f"{stem}.zip")
        write_msi(root, args.arch, args.release, version, args.out / f"{stem}.msi", Path(work))
    for suffix in (".zip", ".msi"):
        path = args.out / f"{stem}{suffix}"
        print(f"{sha256(path)}  {path.name}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
