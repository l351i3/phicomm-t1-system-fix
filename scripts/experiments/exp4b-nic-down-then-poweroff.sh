#!/bin/bash
# exp4b-nic-down-then-poweroff.sh — 关机链路异常研究 · 实验 4B：先放网卡再关机（根治链原型）
# ------------------------------------------------------------------
# 目的：验证「eth0 先 down（链路已放下）→ 再走完整关机流程」在网线在位时是否安全。
# 若干净 → 根修 = 装关机前 quiet-down 钩子（本脚本即原型）。
# 若仍链路异常 → 说明链路异常发生在"停机断电瞬间"，需 PHY 级处理（实验 4C 备选）。
# ------------------------------------------------------------------
# 执行（远端 detached）：systemd-run --unit=exp4b --collect /root/remote-hub/exp4b-nic-down-then-poweroff.sh
# 判定（Mac 侧 netwatch）：
#   - T1=FAIL 起点 = eth0 down 时刻；随后关注 NET 是否在 关机中途/停机后 出现 FAIL
#   - NET 全程 OK（含停机后静置）→ ✅ 根治链成立
# 恢复：拔网线 →（等 2 秒）→ 插回 → 自动唤醒启动

D=/root/remote-hub
LOG=$D/exp4b-run.log
exec >> "$LOG" 2>&1

echo "===== EXP4B begin $(date '+%F %T') ====="
logger "EXP4B: begin"

dmesg -w > $D/exp4b-dmesg.log 2>&1 &

echo "--- [before] ---"
cat /sys/class/net/eth0/carrier /sys/class/net/eth0/operstate
ethtool eth0 | grep -E "Speed|Duplex|Link detected"

nmcli device set eth0 managed no
sleep 1
ip link set eth0 down
logger "EXP4B: eth0 DOWN (cable attached); halting in 5s"
echo "eth0 down at $(date '+%F %T')"

sleep 5
logger "EXP4B: going poweroff now"
echo "poweroff at $(date '+%F %T')"
systemctl poweroff
