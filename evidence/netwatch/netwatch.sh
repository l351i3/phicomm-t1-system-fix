#!/bin/bash
# netwatch.sh - Mac 侧网络监测（G1 电源键受控实验用；长跑版：86400 次循环 ≈ 14h10m）
# 每秒轮询: T1(T1_IP) / 旁路由网关(LAN_IP) / 上游路由器(GW_IP) / 外网(223.5.5.5)
# 输出: remote-hub/netwatch-<时间戳>.log（追加，带毫秒级时间戳）
cd "$(dirname "$0")" || exit 1
LOG="netwatch-$(date '+%m%d-%H%M%S').log"
echo "# netwatch start $(date '+%F %T')" > "$LOG"
for i in $(seq 1 86400); do
  ts=$(date '+%F %T')
  p16=$(ping -c1 -t1 T1_IP >/dev/null 2>&1 && echo OK || echo FAIL)
  p2=$(ping -c1 -t1 LAN_IP >/dev/null 2>&1 && echo OK || echo FAIL)
  p1=$(ping -c1 -t1 GW_IP >/dev/null 2>&1 && echo OK || echo FAIL)
  pnet=$(ping -c1 -t2 223.5.5.5 >/dev/null 2>&1 && echo OK || echo FAIL)
  echo "$ts T1=$p16 GW2=$p2 R1=$p1 NET=$pnet" >> "$LOG"
  sleep 0.5
done
echo "# netwatch end $(date '+%F %T')" >> "$LOG"
