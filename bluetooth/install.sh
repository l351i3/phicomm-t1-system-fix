#!/bin/bash
# T1 (斐讯) BCM4345C0 UART 蓝牙一键安装脚本
# 适用: Amlogic S912 / 斐讯 T1 / Armbian (含 ophub 系内核)
# 用法: sudo ./install.sh
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
HCD_NAME="BCM4345C0_003.001.025.0162.0000_Generic_UART_37_4MHz_wlbga_ref_iLNA_iTR_eLG.hcd"

if [ "$(id -u)" != "0" ]; then
    echo "错误: 请用 root 运行 (sudo ./install.sh)"
    exit 1
fi

echo "==> [1/5] 安装初始化程序 btflow -> /root/t1bt/"
mkdir -p /root/t1bt
if [ -x "$HERE/btflow" ]; then
    install -m 0755 "$HERE/btflow" /root/t1bt/btflow
    echo "    使用预编译 aarch64 二进制"
elif command -v gcc >/dev/null 2>&1; then
    gcc -O2 -Wall -o /root/t1bt/btflow "$HERE/btflow.c"
    echo "    本机 gcc 编译完成"
else
    echo "错误: 无预编译二进制且系统无 gcc"
    exit 1
fi

echo "==> [2/5] 安装固件 -> /lib/firmware/brcm/"
mkdir -p /lib/firmware/brcm
install -m 0644 "$HERE/$HCD_NAME" "/lib/firmware/brcm/$HCD_NAME"

echo "==> [3/5] 安装服务脚本 -> /usr/local/bin/"
install -m 0755 "$HERE/t1bt-btinit.sh" /usr/local/bin/t1bt-btinit.sh

echo "==> [4/5] 安装 systemd 单元"
install -m 0644 "$HERE/t1bt-bluetooth.service" /etc/systemd/system/t1bt-bluetooth.service
systemctl daemon-reload

echo "==> [5/5] 启用服务 (开机自启)"
systemctl enable t1bt-bluetooth.service

echo
echo "安装完成。"
echo "  * 立即启动:   systemctl start t1bt-bluetooth.service"
echo "  * 或直接重启: reboot  (开机约 1~2 分钟内 hci0 自动就绪)"
echo "  * 查看进度:   tail -f /var/log/t1bt-btinit.log"
echo "  * 验证蓝牙:   hciconfig -a  /  bluetoothctl show"
echo "  * 卸载:       见 README.md 的卸载一节"
