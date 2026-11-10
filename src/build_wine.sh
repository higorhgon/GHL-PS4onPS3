#!/bin/bash
# GHL-PS4onPS3 - build with the Cell SDK under Wine (same setup as ds45pad).
set -e
cd "$(dirname "$0")"

SDK="${CELL_SDK:-$HOME/ps3sdk/cell}"
TOOLS="${DS45_TOOLS:-$HOME/ps3sdk/tools}"
export WINEPREFIX="${WINEPREFIX:-$HOME/ps3sdk/wineprefix}"
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" DISPLAY=

GCC="$SDK/host-win32/ppu/bin/ppu-lv2-gcc.exe"
PRXSTRIP="$SDK/host-win32/bin/ppu-lv2-prx-strip.exe"
INCLUDE="$SDK/target/ppu/include"
LIBDIR="$SDK/target/ppu/lib"
winpath() { echo "Z:$(realpath -m "$1")" | tr '/' '\\'; }

SRCS="libc main"
EXTRA=""
[ "$1" = debug ] && EXTRA="-DGHL_DEBUG"   # logs unknown report bytes (adds file I/O)
LDLIBS="-lusbd_stub -lfs_stub -llv2_stub"
AUTH=1010000001000003   # game process
CFLAGS="-mprx -Os -ffunction-sections -fdata-sections -fno-builtin-printf \
        -nodefaultlibs -std=gnu99 -Wall -Wno-unused-parameter -I $(winpath "$INCLUDE") $EXTRA"
LDFLAGS="-mprx -zgenprx -zgenstub -ffunction-sections -fdata-sections"
SCEFLAGS="-0 SELF -1 TRUE -s FALSE -2 0A -3 $AUTH -4 01000002 \
          -5 APP -6 0003004000000000 -A 0001000000000000"

OBJ=obj; OUT=ghl_ps4onps3.sprx
mkdir -p $OBJ; rm -f $OBJ/*
OBJS=""
for f in $SRCS; do
	wine "$GCC" $CFLAGS -c "$f.c" -o "$OBJ/$f.o"
	OBJS="$OBJS $OBJ/$f.o"
done
wine "$GCC" $LDFLAGS $OBJS -L "$(winpath "$LIBDIR")" $LDLIBS -o $OBJ/ghl.sym
wine "$PRXSTRIP" --strip-debug --strip-section-header $OBJ/ghl.sym -o $OBJ/ghl.prx
(cd "$TOOLS" && PS3="$(winpath "$TOOLS/data")" \
	wine scetool.exe $SCEFLAGS -e "$(winpath "$OLDPWD/$OBJ/ghl.prx")" \
	                              "$(winpath "$OLDPWD/$OBJ/ghl.signed")" > /dev/null)
[ "$(od -A n -t x1 -N 4 $OBJ/ghl.signed | tr -d ' ')" = 53434500 ] || { echo "ERRO: assinatura falhou"; exit 1; }
mv $OBJ/ghl.signed $OUT
echo "ok: $OUT ($(stat -c %s $OUT) bytes)"
