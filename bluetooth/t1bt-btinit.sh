#!/bin/bash
# t1bt-btinit.sh - T1 (Amlogic S912) BCM4345C0 UART 蓝牙初始化与看护
#
# 职责: 确保 hci0 可用。
#   - hci0 不存在 -> 执行完整初始化:
#       上电 -> S7 健康判据(不健康即 REGEN 退出重试) -> 逐字节固件下载(280 块)
#       -> 切 115200 验证固件激活 -> 原位挂 N_HCI ldisc + H4 协议
#       -> btflow fork 出常驻 holder 持有 tty (维持 hci0 存活)
#   - hci0 存在 -> 保持等待(服务常驻, 30s 巡检)
#
# 设计要点:
#   * 判定与动作分离: 只依据 /sys/class/bluetooth/hci0 是否存在决定是否重建;
#     重建动作不改变该判定输入, 幂等 (连跑两次行为一致)。
#   * holder 死亡 -> hci0 自动消失 -> 本循环自动重建 (无需人工介入)。
#   * 每次尝试前 btflow 自行对芯片 GPIO 断电 2s 再上电 (全新会话),
#     因此上一次失败不会污染下一次。
#   * 依赖 btflow scene23: 成功输出 "OK ATTACHED holder_pid=<pid>"。

set -u

HCD=/lib/firmware/brcm/BCM4345C0_003.001.025.0162.0000_Generic_UART_37_4MHz_wlbga_ref_iLNA_iTR_eLG.hcd
BIN=/root/t1bt/btflow
LOG=/var/log/t1bt-btinit.log
TMP=/run/t1bt-try.log
INIT_TRIES=8          # 单轮初始化内最多尝试次数(每次全新上电)
CHK_INTERVAL=30       # hci0 健康时的巡检间隔(秒)
TRY_TIMEOUT=220       # 单次 btflow 运行上限(秒); 500us 下载约 60s
LOG_MAX=5242880       # 5MB 日志轮转阈值

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

rotate_log() {
    if [ -f "$LOG" ] && [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt "$LOG_MAX" ]; then
        mv -f "$LOG" "$LOG.1"
    fi
}

init_once() {
    # 返回 0 = hci0 已就绪
    local i rc
    for i in $(seq 1 "$INIT_TRIES"); do
        # 边角覆盖: 上一次尝试实际已成功(例: 父进程被 KILL 但 holder 已 fork)
        if [ -e /sys/class/bluetooth/hci0 ]; then
            log "hci0 已出现(无需再次尝试)"
            return 0
        fi
        log "init try $i/$INIT_TRIES"
        timeout -s KILL "$TRY_TIMEOUT" "$BIN" 89760 89760 "$HCD" 0 0 0 23 > "$TMP" 2>&1
        rc=$?
        cat "$TMP" >> "$LOG"
        if grep -q "OK ATTACHED" "$TMP" || [ -e /sys/class/bluetooth/hci0 ]; then
            log "attach OK on try $i"
            return 0
        fi
        log "try $i failed (rc=$rc)"
        sleep 3
    done
    return 1
}

rotate_log
log "=== t1bt-btinit start (pid $$) ==="

# 确保 hci_uart 已加载 (scene23 挂 N_HCI ldisc 的前置条件; 幂等)
modprobe hci_uart 2>/dev/null || true

while :; do
    if [ -e /sys/class/bluetooth/hci0 ]; then
        sleep "$CHK_INTERVAL"
        continue
    fi
    log "hci0 不在, 开始初始化"
    if init_once; then
        log "hci0 就绪, 进入看护"
    else
        log "本轮全部失败, 60s 后重来"
        sleep 60
    fi
    sleep 5
done
