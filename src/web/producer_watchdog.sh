#!/bin/sh
# producer_watchdog.sh — producer 看门狗: 崩了就重启
# 背景: RV1126B 的 MPP 编码核 (rkvenc2) 无独立 JPEG 核, 预览转码并发时
#       可能饿死 H264 编码器, 被驱动看门狗杀进程 (h264e_dpb 转储)。
#       产品化前的兜底方案: 崩了自动拉起, 演示不中断。
# 用法: 开机自启 (init 脚本) 或手动: nohup sh producer_watchdog.sh &
# 注意: 启动前若 :80 被 nginx 等占用 (SDK 默认服务), 先清理 —
#       producer 的 ONVIF HTTP 需要 80 端口

LOG=/tmp/watchdog.log
while true; do
    if ! pgrep -x producer >/dev/null; then
        # 8554 还被占着 (上一实例未完全退出) → 等下一轮, 避免重复拉起
        if netstat -tln 2>/dev/null | grep -q ':8554 '; then
            sleep 3
            continue
        fi
        # :80 被其他进程占用则清理 (nginx 等 SDK 自带服务)
        if netstat -tln 2>/dev/null | grep -q ':80 '; then
            killall -9 nginx 2>/dev/null
            sleep 1
        fi
        cd /root || exit 1
        setsid nohup ./producer 8554 </dev/null >>/tmp/producer.log 2>&1 &
        echo "[watchdog] $(date '+%F %T') producer 重启" >> "$LOG"
        sleep 8   # 等 producer 完成初始化 (v4l2/ISP 启动需要数秒)
    else
        sleep 5
    fi
done
