#!/bin/bash
# exp4a-nic-down-test.sh — 关机链路异常研究 · 实验 4A：「带线 down 网卡」隔离测试
# ------------------------------------------------------------------
# 目的：验证「关机流程中 eth0 下线（dev_close）在网线在位时」是否为链路异常触发点。
# 背景：三次受控实验 + 驱动源码考古（meson8b-dwmac 无 .shutdown 回调）
#       → 异常点收敛到 = NM 停止时对 eth0 的 dev_close（链路在位）。
# 参考数据：remote-hub/netwatch-1008-*.log（实验 1/2/3）
# ------------------------------------------------------------------
# 执行（远端 detached，SSH 断开不影响）：
#   systemd-run --unit=exp4a --collect /root/remote-hub/exp4a-nic-down-test.sh
# 判定（Mac 侧 netwatch）：
#   - 干净：T1=FAIL 约 30s（自身下线窗口），GW2/R1/NET 全 OK
#   - 链路异常：NET 在 T1 下线瞬间起 FAIL → 按既有恢复流程：拔 T1 网线
# ------------------------------------------------------------------

D=/root/remote-hub
LOG=$D/exp4a-run.log
exec >> "$LOG" 2>&1

echo "=================================================="
echo "EXP4A begin $(date '+%F %T')"

# --- 阶段 0: 前状态快照 ---
echo "--- [before] ---"
cat /sys/class/net/eth0/carrier /sys/class/net/eth0/operstate
ethtool eth0 | grep -E "Speed|Duplex|Link detected"
ip -s link show eth0
nmcli device status | grep eth0

# --- 阶段 1: dmesg 滚动采集到文件（后台）---
dmesg -w > $D/exp4a-dmesg.log 2>&1 &
DMESG_PID=$!

# --- 阶段 2: 解除 NM 管辖 → down（网线保持在位！）---
logger "EXP4A: going DOWN with cable attached"
nmcli device set eth0 managed no
sleep 1
ip link set eth0 down
logger "EXP4A: eth0 is DOWN now"

# --- 阶段 3: 保持 20 秒（观察窗）---
sleep 20

# --- 阶段 4: up + 恢复 NM 管辖 ---
ip link set eth0 up
logger "EXP4A: eth0 is UP again"
sleep 3
nmcli device set eth0 managed yes
nmcli device connect eth0 || true
sleep 10

# --- 阶段 5: 后状态快照 + 连通性自检 + 收尾 ---
echo "--- [after] ---"
cat /sys/class/net/eth0/carrier /sys/class/net/eth0/operstate
ethtool eth0 | grep -E "Speed|Duplex|Link detected"
ip -s link show eth0
nmcli device status | grep eth0
echo "--- ping checks (T1 视角) ---"
ping -c 2 -W 2 LAN_IP >/dev/null 2>&1 && echo "GW2_OK" || echo "GW2_FAIL"
ping -c 2 -W 2 223.5.5.5 >/dev/null 2>&1 && echo "NET_OK" || echo "NET_FAIL"
kill $DMESG_PID 2>/dev/null
echo "EXP4A done $(date '+%F %T')"
echo "=================================================="
