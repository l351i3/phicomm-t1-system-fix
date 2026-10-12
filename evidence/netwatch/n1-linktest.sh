#!/bin/sh
# n1-linktest v2: eth0 link 闪断 4s 实验（nohup 脱离会话运行；自动恢复）
# v2 修复（2026-10-09 事故后）：① 去掉 $((浮点)) 算术（busybox sh 不支持 → 曾直接崩掉脚本、up 未执行、N1 掉线）
#                              ② 增加独立看护进程：60s 后无条件拉回 eth0（兜底）
# 判据：down 期间链路是否被干净断开（对端视角=路由器日志；本机视角=dmesg/carrier_changes）
LOG=/root/linktest.log
# ---- 独立看护：无论主流程发生什么，60 秒后强制拉回 eth0 ----
nohup sh -c 'sleep 60; ip link set eth0 up; echo "-- watchdog forced up at $(date "+%F %T")" >> /root/linktest.log' >/dev/null 2>&1 &
{
  echo "=== start v2 wall=$(date '+%F %T') uptime=$(cut -d' ' -f1 /proc/uptime) ==="
  echo "-- pre carrier_changes=$(cat /sys/class/net/eth0/carrier_changes 2>/dev/null)"
  echo "-- pre addr: $(ip -4 addr show dev eth0 2>/dev/null | grep inet)"
  sleep 8
  D0=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
  echo "-- DOWN wall=$(date '+%F %T') uptime_s=$D0"
  ip link set eth0 down
  sleep 4
  U0=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
  echo "-- UP wall=$(date '+%F %T') uptime_s=$U0 down_span=$((U0-D0))s"
  ip link set eth0 up
  ip link show dev eth0 2>/dev/null | grep -q ',UP' || { echo "-- up retry"; sleep 1; ip link set eth0 up; }
  sleep 3
  echo "-- post carrier_changes=$(cat /sys/class/net/eth0/carrier_changes 2>/dev/null)"
  echo "-- post addr: $(ip -4 addr show dev eth0 2>/dev/null | grep inet)"
  echo "-- post route: $(ip route 2>/dev/null | head -3 | tr '\n' '|')"
  echo "=== dmesg tail ==="
  dmesg | tail -30
  echo "=== end wall=$(date '+%F %T') uptime=$(cut -d' ' -f1 /proc/uptime) ==="
} >> $LOG 2>&1
