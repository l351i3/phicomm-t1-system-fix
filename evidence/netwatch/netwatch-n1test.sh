#!/bin/bash
# N1 link 闪断预试专用探针：T1 .16 / N1 .2 / R1 .1 / MITV .123
# 逐秒并行 ping，1s 超时；运行到手动结束
LOG="/Users/username/WorkBuddy/2026-10-08-00-43-22/remote-hub/netwatch-n1poweroff.log"
: > "$LOG"
echo "# start $(date '+%F %T')" >> "$LOG"
probe() {
  local name="$1" ip="$2"
  while :; do
    if ping -c 1 -W 1000 "$ip" >/dev/null 2>&1; then st=OK; else st=FAIL; fi
    printf '%s %-5s %s\n' "$(date '+%F %T')" "$name" "$st" >> "$LOG"
    sleep 1
  done
}
probe T1 T1_IP &
probe N1 LAN_IP &
probe R1 GW_IP &
probe MITV LAN_IP &
wait
