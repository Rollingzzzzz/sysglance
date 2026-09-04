#!/bin/sh
# Cross-compile SysGlance from Linux (mingw-w64) or Windows (MSYS2 / winlibs).
set -e
gcc=x86_64-w64-mingw32-gcc
command -v $gcc >/dev/null 2>&1 || gcc=gcc   # MSYS2/winlibs native
wr=$(command -v x86_64-w64-mingw32-windres 2>/dev/null || command -v windres)

# embed the icon and the requireAdministrator manifest (ETW needs elevation)
$wr -O coff -I src src/sysglance.rc -o sysglance.res.o

$gcc -O2 -Wall -municode -mwindows \
    -D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000 \
    -o sysglance.exe src/sysglance.c sysglance.res.o \
    -lgdi32 -luser32 -ladvapi32 -lpdh -liphlpapi -lshell32 -lws2_32 -lpsapi -ldxgi
echo "built: sysglance.exe"
# diagnostic companions (see README)
$gcc -O2 -Wall -o gpuprobe.exe src/gpuprobe.c -lpdh
$gcc -O2 -Wall -Wno-array-bounds -o netprobe.exe src/netprobe.c -ladvapi32
echo "built: gpuprobe.exe netprobe.exe"
