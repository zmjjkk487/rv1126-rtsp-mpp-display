#!/bin/bash
# RV1126 GStreamer + RGA 混合管线 — 交叉编译
# 管道: rtspsrc → depay → parse → mppvideodec → appsink(NV12) → RGA → fbdev

set -e

PROJECT="rv1126_gst_display"
OUTDIR="output"
BIN="$OUTDIR/$PROJECT"

TC="/opt/atk-dlrv1126b-toolchain"
CC="${TC}/bin/aarch64-buildroot-linux-gnu-gcc"
SYSROOT="${TC}/aarch64-buildroot-linux-gnu/sysroot"

if [ ! -f "$CC" ]; then
    echo "错误: 找不到 $CC"
    exit 1
fi

echo "编译器: $CC"

# ---- 编译 ----
CFLAGS="--sysroot=$SYSROOT -Wall -O2 -g -std=c11"
CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=199309L"
CFLAGS="$CFLAGS -I$SYSROOT/usr/include/gstreamer-1.0"
CFLAGS="$CFLAGS -I$SYSROOT/usr/include/glib-2.0"
CFLAGS="$CFLAGS -I$SYSROOT/usr/lib/glib-2.0/include"
CFLAGS="$CFLAGS -I$SYSROOT/usr/include"

LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib"
LDFLAGS="$LDFLAGS -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0"
LDFLAGS="$LDFLAGS -lgstapp-1.0 -lgstvideo-1.0 -lm -ldl"

SRCS="main.c log.c config.c fbdev.c rga_convert.c"

mkdir -p "$OUTDIR"

echo "编译..."
$CC $CFLAGS -o "$BIN" $SRCS $LDFLAGS

echo ""
echo "============================================"
echo "编译成功: $BIN"
echo "文件大小: $(ls -lh "$BIN" | awk '{print $5}')"
echo ""
echo "部署:"
echo "  scp $BIN config.ini root@<IP>:/root/"
echo "板端运行:"
echo "  ./$PROJECT config.ini"
echo ""
echo "管道: rtspsrc → depay → parse → mppvideodec → appsink → RGA → fbdev"
echo "============================================"
