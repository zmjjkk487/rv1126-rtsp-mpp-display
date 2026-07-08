#!/bin/bash
# 正点原子 ATK-DLRV1126B RTSP MPP 硬解码 Demo — 交叉编译脚本
# 架构: FFmpeg avformat 拆帧 → MPP 硬解 H.264 → (RGA|CPU) NV12→RGB → fbdev 显示

set -e

PROJECT="rv1126_rtsp_mpp"
OUTDIR="output"
BIN="$OUTDIR/$PROJECT"

# ---- 工具链 ----
TC="/opt/atk-dlrv1126b-toolchain"
CC="${TC}/bin/aarch64-buildroot-linux-gnu-gcc"
SYSROOT="${TC}/aarch64-buildroot-linux-gnu/sysroot"
STRIP="${TC}/bin/aarch64-buildroot-linux-gnu-strip"

if [ ! -f "$CC" ]; then
    echo "错误: 找不到 $CC"
    echo "请先安装工具链到 $TC"
    exit 1
fi

echo "编译器: $CC"
$CC --version | head -1
echo "sysroot: $SYSROOT"

# ---- MPP 库检查 ----
MPP_LIB_FOUND=0
for lib in "$SYSROOT/usr/lib/librockchip_mpp.so" \
           "$SYSROOT/usr/lib/librk_mpp.so"; do
    if [ -f "$lib" ]; then MPP_LIB_FOUND=1; break; fi
done

if [ "$MPP_LIB_FOUND" = "0" ]; then
    echo ""
    echo "============================================"
    echo "  MPP .so 不在 sysroot 中"
    echo "  需要从板子拷贝:"
    echo ""
    echo "  1. 板子上: find /usr/lib -name '*rockchip_mpp*'"
    echo "  2. 拷贝:   scp root@板子IP:/usr/lib/librockchip_mpp.so* $SYSROOT/usr/lib/"
    echo "============================================"
    echo ""
fi

# ---- RGA 库检查 (运行时 dlopen, 编译只需 -ldl) ----
echo "RGA: 运行时 dlopen librga.so, 无 RGA 自动回退 CPU"

# ---- 构建 ----
CFLAGS="-Wall -O2 -g -std=c11 -D_POSIX_C_SOURCE=199309L -D_DEFAULT_SOURCE --sysroot=$SYSROOT"
CFLAGS="$CFLAGS -I. -I$SYSROOT/usr/include/rockchip"

LDFLAGS="--sysroot=$SYSROOT"
LDFLAGS="$LDFLAGS -lrockchip_mpp -lavformat -lavcodec -lavutil"
LDFLAGS="$LDFLAGS -lpthread -lrt -lm -lz -ldl"

SRCS="main.c log.c config.c fbdev.c ffmpeg_demux.c mpp_dec.c rga_convert.c"

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
echo "数据流: FFmpeg → MPP(H264硬解) → RGA/CPU(NV12→RGB) → fbdev → 屏幕"
echo "============================================"
