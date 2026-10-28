#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Ahmed ARIF <arif.ing@outlook.com>

set -euo pipefail
W="${1:?usage: build_assemblies.sh <workdir> <wine-mono-11.3.0-*.msi>}"
MSI="${2:?usage: build_assemblies.sh <workdir> <wine-mono-11.3.0-*.msi>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CN="${WMBUILD_CONTAINER:-wmbuild}"
mkdir -p "$W"
M="$W/mono"; E="$M/external"
if [ ! -d "$M/.git" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/wine-mono/mono.git "$M"
    git -C "$M" sparse-checkout init --no-cone
    printf '%s\n' '/*' '!/mono/' '!/external/' '/mcs/' '!/mcs/tests/' '!/mcs/errors/' '!/mcs/class/*/Test/' '!/mcs/class/*/Test*/' > "$M/.git/info/sparse-checkout"
    git -C "$M" checkout -q 73610cc7350b7b51dd3bde3323a8ae28eaf5f7fc
    clone() { local url="$1" dir="$2" rev="$3"; shift 3; git clone -q --filter=blob:none --no-checkout "$url" "$E/$dir"; git -C "$E/$dir" sparse-checkout init --no-cone; printf '%s\n' "$@" > "$E/$dir/.git/info/sparse-checkout"; git -C "$E/$dir" checkout -q "$rev"; }
    mkdir -p "$E"
    clone https://gitlab.winehq.org/mono/corefx.git corefx 290d27ab8adb9f437ceeb82dbd0a36f01efad6ad '/src/Common/src/' '/src/System.*/src/' '/src/Microsoft.*/src/'
    clone https://github.com/mono/corert.git corert 11136ad55767485063226be08cfbd32ed574ca43 '/src/'
    clone https://github.com/mono/roslyn-binaries.git roslyn-binaries 1c6482470cd219dcc7503259a20f26a1723f20ec '/*'
    clone https://github.com/wine-mono/reference-assemblies.git binary-reference-assemblies eb33e216f039c0edc4f1406266af3ced6f44be1f '/v4.7.1/'
    git -C "$M" apply "$HERE/patches/mono-x509-crypt32.patch"
    git -C "$M" apply "$HERE/patches/mono-customattrs-reflected-base.patch"
    git -C "$M" apply "$HERE/patches/mono-winrt-projection-resolve.patch"
    git -C "$E/corefx" apply "$HERE/patches/corefx-certificatechoices.patch"
fi
P="$W/wpf"
if [ ! -d "$P/.git" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/wine-mono/wpf.git "$P"
    git -C "$P" sparse-checkout init --no-cone
    printf '%s\n' '/src/Microsoft.DotNet.Wpf/src/WindowsBase/' '/src/Microsoft.DotNet.Wpf/src/Shared/' '/src/Microsoft.DotNet.Wpf/src/Common/' '/eng/WpfArcadeSdk/tools/' '/src/Microsoft.DotNet.Wpf/*.pub' '/src/Microsoft.DotNet.Wpf/*.snk' > "$P/.git/info/sparse-checkout"
    git -C "$P" checkout -q 9178bf7cb4d5d33dfd36f0611153c9f95a337595
    git -C "$P" apply "$HERE/patches/wpf-seekable-parts.patch"
fi
mkdir -p "$W/winrt" && cp "$HERE/winrt/Windows.cs" "$W/winrt/Windows.cs"
curl -sSL -o "$W/resx2srid.cs" https://raw.githubusercontent.com/wine-mono/wine-mono/wine-mono-11.3.0/tools/resx2srid/resx2srid.cs
mkdir -p "$W/msi" && 7zz x -y -o"$W/msi" "$MSI" >/dev/null
L="$M/mcs/class/lib/net_4_x-win32"; G="$W/msi/lib/mono/gac"; mkdir -p "$L" "$M/mcs/class/lib/build"
for a in System.Configuration.Install/4.0.0.0__b03f5f7f11d50a3a/System.Configuration.Install.dll System.Windows.Forms/4.0.0.0__b77a5c561934e089/System.Windows.Forms.dll System.Drawing/4.0.0.0__b03f5f7f11d50a3a/System.Drawing.dll System.Xml/4.0.0.0__b77a5c561934e089/System.Xml.dll System.Configuration/4.0.0.0__b03f5f7f11d50a3a/System.Configuration.dll System.Core/4.0.0.0__b77a5c561934e089/System.Core.dll WineMono.Security/4.0.0.0__0738eb9f132ed756/WineMono.Security.dll System.Numerics/4.0.0.0__b77a5c561934e089/System.Numerics.dll System.Net.Http/4.0.0.0__b03f5f7f11d50a3a/System.Net.Http.dll; do cp "$G/$a" "$L/"; done
cp "$W/msi/lib/mono/4.5/mscorlib.dll" "$L/"
R="$W/refs/lib/mono/4.5-api"; mkdir -p "$R"
cp "$W/msi/lib/mono/4.5/mscorlib.dll" "$R/"
for a in System System.Xaml System.Xml System.Core System.Security System.Configuration System.IO.Compression; do cp "$G/$a"/4.0.0.0__*/"$a.dll" "$R/"; done
cat > "$M/mcs/build/config.make" <<'CFG'
prefix=/usr
exec_prefix=/usr
sysconfdir=/etc
mono_libdir=${exec_prefix}/lib
mono_build_root=/w/mono
RUNTIME = mono
VERSION = 6.13.0
INSTALL = /usr/bin/install
MONO_VERSION = 6.13.0
MONO_CORLIB_VERSION = 1A5E0066-58DC-428A-B21C-0AD6CDAE2789
BUILD_PLATFORM = linux
HOST_PLATFORM ?= win32
XTEST_PLATFORM ?= windows
PLATFORM_AOT_SUFFIX = .so
DISABLE_MCS_DOCS = yes
DEFAULT_PROFILE = net_4_x
STANDALONE_CSC_LOCATION=/w/mono/external/roslyn-binaries/Microsoft.Net.Compilers/3.9.0/csc.exe
SERVER_CSC_LOCATION=/w/mono/external/roslyn-binaries/Microsoft.Net.Compilers/3.9.0/csc.exe
CFG
docker rm -f "$CN" >/dev/null 2>&1 || true
docker run -d --name "$CN" -v "$W:/w" -w /w debian:bookworm sleep infinity >/dev/null
docker exec "$CN" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq --no-install-recommends mono-complete make python3 perl >/dev/null
cd /w/mono/mcs
sed -e "s/@MONO_VERSION@/6.13.0.0/g" -e "s/@MONO_CORLIB_VERSION@/1A5E0066-58DC-428A-B21C-0AD6CDAE2789/g" build/common/Consts.cs.in > build/common/Consts.cs
sed -e "s|@[A-Za-z_]*@|73610cc7350b7b51dd3bde3323a8ae28eaf5f7fc|g" build/common/sourcelink.json.in > build/common/sourcelink.json
mcs -nologo -out:class/lib/build/gensources.exe tools/gensources/gensources.cs
cp /usr/lib/mono/4.5/sn.exe /usr/lib/mono/4.5/ilasm.exe class/lib/build/
C=$(find /usr/lib/mono/gac/Mono.Cecil -name Mono.Cecil.dll | head -1)
cp "$C" class/lib/build/
mcs -nologo -r:"$C" -out:class/lib/build/cil-stringreplacer.exe tools/cil-stringreplacer/cil-stringreplacer.cs class/Mono.Options/Mono.Options/Options.cs
cd tools/resgen && mcs -nologo -unsafe -d:NET_4_0 -d:NET_4_5 -d:MONO -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:System.Xml.dll -r:System.Runtime.Serialization.Formatters.Soap.dll -out:/w/mono/mcs/class/lib/build/resgen.exe $(grep -v "^#" resgen.exe.sources | grep -v "^$" | tr "\n" " ")
cd /w/mono/mcs/class/System && rm -f ../lib/net_4_x-win32/System.dll
make PROFILE=net_4_x HOST_PLATFORM=win32 ENABLE_COMPILER_SERVER=0 HAVE_BTLS=1 BCL_OPTIMIZE=1 >/w/build-system.log 2>&1 || true
test -f ../lib/net_4_x-win32/System.dll
cp ../lib/net_4_x-win32/System.dll /w/System.patched.dll
cd /w/mono/mcs/class/System.ServiceProcess && rm -f ../lib/net_4_x-win32/System.ServiceProcess.dll
make PROFILE=net_4_x HOST_PLATFORM=win32 ENABLE_COMPILER_SERVER=0 BCL_OPTIMIZE=1 >/w/build-sp.log 2>&1 || true
cp ../lib/net_4_x-win32/System.ServiceProcess.dll /w/System.ServiceProcess.patched.dll
cd /w/mono/mcs/class/System.Security && rm -f ../lib/net_4_x-win32/System.Security.dll
make PROFILE=net_4_x HOST_PLATFORM=win32 ENABLE_COMPILER_SERVER=0 BCL_OPTIMIZE=1 >/w/build-sec.log 2>&1 || true
cp ../lib/net_4_x-win32/System.Security.dll /w/System.Security.patched.dll
cd /w/mono/mcs/class/corlib && rm -f ../lib/net_4_x-win32/mscorlib.dll
make PROFILE=net_4_x HOST_PLATFORM=win32 ENABLE_COMPILER_SERVER=0 BCL_OPTIMIZE=1 >/w/build-corlib.log 2>&1 || true
cp ../lib/net_4_x-win32/mscorlib.dll /w/mscorlib.patched.dll
mono /w/mono/external/roslyn-binaries/Microsoft.Net.Compilers/3.9.0/csc.exe -nologo -noconfig -nostdlib -target:library -optimize+ -deterministic -r:/w/msi/lib/mono/4.5/mscorlib.dll -out:/w/Windows.built.dll /w/winrt/Windows.cs
mkdir -p /w/bin
printf "#!/bin/sh\nexec mono /w/mono/mcs/class/lib/build/resgen.exe \"\$@\"\n" > /w/bin/resgen
printf "#!/bin/sh\nexec mono /w/mono/external/roslyn-binaries/Microsoft.Net.Compilers/3.9.0/csc.exe \"\$@\"\n" > /w/bin/csc
chmod +x /w/bin/resgen /w/bin/csc
mcs -nologo -out:/w/resx2srid.exe /w/resx2srid.cs
export PATH=/w/bin:$PATH
D=/w/refs/lib/mono/4.5-api
REFS="-r:$D/mscorlib.dll -r:$D/System.dll -r:$D/System.Xaml.dll -r:$D/System.Xml.dll -r:$D/System.Core.dll -r:$D/System.Security.dll -r:$D/System.Configuration.dll -r:$D/System.IO.Compression.dll"
cd /w/wpf/src/Microsoft.DotNet.Wpf/src/WindowsBase && rm -f WindowsBase.dll
make MONO_PREFIX=/w/refs RESX2SRID=/w/resx2srid.exe ACCESSIBILITY_DLL=/w/msi/lib/mono/gac/Accessibility/4.0.0.0__b03f5f7f11d50a3a/Accessibility.dll "LIB_REFS=$REFS" >/w/build-wb.log 2>&1
cp WindowsBase.dll /w/WindowsBase.patched.dll'
docker rm -f "$CN" >/dev/null
ls -la "$W"/System.patched.dll "$W"/System.ServiceProcess.patched.dll "$W"/System.Security.patched.dll "$W"/mscorlib.patched.dll "$W"/WindowsBase.patched.dll "$W"/Windows.built.dll
