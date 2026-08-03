#!/bin/bash
# ✅ 主方案编译 — GStreamer + RGA 混合管线 (推荐)
# 管道: rtspsrc → depay → parse → mppvideodec → appsink(NV12) → RGA → fbdev

set -e

# 进程名 ≤15 字符 (Linux comm 截断限制), 否则 killall/pgrep/pidof 匹配不到
PROJECT="rtsp_display"
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

# ---- Web 管理后台 (ONVIF 发现) ----
WEB_BIN="$OUTDIR/rv1126_web"
echo ""
echo "编译 Web 管理后台..."
WEB_CFLAGS="--sysroot=$SYSROOT -Wall -O2 -g -std=c11 $CFLAGS"
WEB_LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib -lcurl -lcrypto -lm"
WEB_SRCS="src/web/web_main.c src/web/onvif_disco.c src/web/onvif_soap.c"
$CC $WEB_CFLAGS -o "$WEB_BIN" $WEB_SRCS $WEB_LDFLAGS
echo "编译成功: $WEB_BIN"
echo "文件大小: $(ls -lh "$WEB_BIN" | awk '{print $5}')"
echo ""
echo "部署:"
echo "  scp $WEB_BIN root@<IP>:/root/"
echo "  scp -r src/web/static root@<IP>:/root/camera-web/"
echo "板端运行:"
echo "  ./rv1126_web"
echo "  # 浏览器打开 http://<板子IP>:8080"
echo "============================================"
