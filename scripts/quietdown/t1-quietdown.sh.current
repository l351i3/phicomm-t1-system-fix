#!/bin/bash
# t1-quietdown.sh — T1 关机前 quiet-down 钩子（关机链路异常修复）
# ------------------------------------------------------------------
# 目的：在任何关机/重启流程中，尽早把 eth0 链路放下，
#       避免「停机瞬间链路在位」触发链路异常（打坏主路由）。
# 依据：实验 4B（down→5s→poweroff 干净）+ 实验 5A（同态复测干净）
# 调用方：t1-quietdown.service 的 ExecStop（见同目录 unit 文件）
# ⚠️ 已知副作用（实验 5A/5B 定案）：quiet-down 会使「插线唤醒」失效；
#    装此钩子后，关机后的开机方式 = 拔插电源。
# ------------------------------------------------------------------
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

LOG=/root/remote-hub/quietdown.log
# 日志容错：日志不可写时不阻断 down（安全关键动作优先，输出降级走 systemd 捕获）
if touch "$LOG" 2>/dev/null; then exec >> "$LOG" 2>&1; fi

echo "===== quietdown $(date '+%F %T') (uptime: $(cut -d' ' -f1 /proc/uptime)s) ====="
logger "t1-quietdown: begin; dropping eth0 before shutdown"

# 1) 让 NetworkManager 放手（防止接口被其拉回 up）
timeout 5 nmcli device set eth0 managed no || echo "warn: nmcli failed"

# 2) 放下链路（异常阻断的关键动作）
timeout 5 ip link set eth0 down || echo "warn: ip link down failed"

# 3) 留证
echo "carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null) operstate=$(cat /sys/class/net/eth0/operstate 2>/dev/null)"
logger "t1-quietdown: done (eth0 down)"

# 尽力而为，不阻断关机流程（unit TimeoutStopSec=15 兜底）
exit 0
