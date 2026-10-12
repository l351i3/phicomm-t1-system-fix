#!/bin/bash
# 把 T1 蓝牙方案注入 ophub T1 镜像 (在 T1 本机运行)
# 用法: bash /root/imgwork/inject.sh
set -e

IMG=/root/imgwork/t1.img
MNT=/mnt/t1img
LOOPDEV=""

cleanup() {
    if [ -n "$LOOPDEV" ]; then
        umount "$MNT" 2>/dev/null || true
        losetup -d "$LOOPDEV" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "== [1] losetup =="
mkdir -p "$MNT"
LOOPDEV=$(losetup -Pf --show "$IMG")
echo "loop=$LOOPDEV"
sleep 1
lsblk "$LOOPDEV"

echo "== [2] mount root (p2) =="
mount "${LOOPDEV}p2" "$MNT"
ls "$MNT" | head -12

echo "== [3] 注入 btflow =="
mkdir -p "$MNT/root/t1bt"
install -m 0755 /root/t1bt/btflow "$MNT/root/t1bt/btflow"
install -m 0644 /root/t1bt/btflow.c "$MNT/root/t1bt/btflow.c"

echo "== [4] 固件(检查后注入) =="
HCD=BCM4345C0_003.001.025.0162.0000_Generic_UART_37_4MHz_wlbga_ref_iLNA_iTR_eLG.hcd
if [ -f "$MNT/lib/firmware/brcm/$HCD" ]; then
    echo "   镜像自带 $HCD"
else
    install -m 0644 "/lib/firmware/brcm/$HCD" "$MNT/lib/firmware/brcm/"
    echo "   已注入固件"
fi

echo "== [5] 脚本 + systemd 单元 =="
install -m 0755 /usr/local/bin/t1bt-btinit.sh "$MNT/usr/local/bin/t1bt-btinit.sh"
install -m 0644 /etc/systemd/system/t1bt-bluetooth.service "$MNT/etc/systemd/system/t1bt-bluetooth.service"

echo "== [6] enable 链接 (照抄真机形态) =="
mkdir -p "$MNT/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/t1bt-bluetooth.service "$MNT/etc/systemd/system/multi-user.target.wants/t1bt-bluetooth.service"
mkdir -p "$MNT/etc/systemd/system/bluetooth.target.wants"
ln -sf /lib/systemd/system/bluetooth.service "$MNT/etc/systemd/system/bluetooth.target.wants/bluetooth.service"
ln -sf /lib/systemd/system/bluetooth.service "$MNT/etc/systemd/system/dbus-org.bluez.service"

echo "== [7] 说明文件 =="
cp /root/imgwork/README-bluetooth.txt "$MNT/root/README-bluetooth.txt"

echo "== [8] 校验清单 =="
echo "--- root/t1bt ---";      ls -la "$MNT/root/t1bt/"
echo "--- bin ---";            ls -la "$MNT/usr/local/bin/t1bt-btinit.sh"
echo "--- unit ---";           ls -la "$MNT/etc/systemd/system/t1bt-bluetooth.service"
echo "--- wants ---";          ls -la "$MNT/etc/systemd/system/multi-user.target.wants/" | grep t1bt
echo "--- bt.target.wants ---";ls -la "$MNT/etc/systemd/system/bluetooth.target.wants/" 2>/dev/null | tail -3
echo "--- readme ---";         ls -la "$MNT/root/README-bluetooth.txt"

sync
echo "== [9] umount + fsck =="
umount "$MNT"
e2fsck -fn "${LOOPDEV}p2" | tail -5
losetup -d "$LOOPDEV"
LOOPDEV=""
echo "== 注入完成 =="
