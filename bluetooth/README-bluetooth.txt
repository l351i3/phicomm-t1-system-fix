================ T1 蓝牙使用说明 (BCM4345C0) ================

本镜像已预置【斐讯 T1（PHICOMM-T1）蓝牙完整方案】——固件下载 + HCI 挂载全自动，开机即用。

【功能验证】
  1) 等待开机约 1~2 分钟（首次开机自动初始化蓝牙）
  2) hciconfig -a          # 应出现 hci0: UP RUNNING, BD Address, HCI 4.2
  3) bluetoothctl show     # 应显示 Powered: yes
  4) bluetoothctl --timeout 12 scan on    # 应扫到周围 BLE 设备

【工作原理 (概要)】
  BCM4345C0 芯片 ROM 无法消化"整包突发 UART 写入"，导致固件下载随机冻结。
  本方案改为【逐字节发送】(字节间隔 500us)，实现 100% 可靠的固件下载
  (280 块, CC=279 ER=0, 约 50 秒)，随后将芯片原位挂到 Linux HCI 栈 (hci0)。
  全程用户态完成，无需改内核/DTB。

【服务与自愈】
  systemd 服务: t1bt-bluetooth.service (开机自启, 常驻看护)
  * 若 hci0 消失(意外退出等), 服务会自动重新初始化 —— 无需人工干预
  * 初始化日志: /var/log/t1bt-btinit.log
  * 服务状态:   systemctl status t1bt-bluetooth

【相关文件】
  /root/t1bt/btflow        初始化程序 (二进制)
  /root/t1bt/btflow.c      源码 (scene23 为完整流程)
  /usr/local/bin/t1bt-btinit.sh       看护脚本
  /etc/systemd/system/t1bt-bluetooth.service
  /lib/firmware/brcm/BCM4345C0_*.hcd  蓝牙固件

【常见问题】
  Q: 开机几分钟仍无 hci0?
  A: 查看 /var/log/t1bt-btinit.log。芯片存在"上电命运二相性"(约 1/3 概率
     首次上电不健康), 脚本会自动断电重试, 通常 1~3 次内命中, 无需干预。
  Q: 想手动重来一次?
  A: systemctl restart t1bt-bluetooth （会杀掉当前 holder 并自动重建）

（本说明由 T1 蓝牙项目生成; 详见 btflow.c 头注释与 README.md）
