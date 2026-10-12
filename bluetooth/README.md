# 斐讯 T1（PHICOMM-T1）BCM4345C0 蓝牙完整解决方案

适用于：斐讯 T1 盒子（Amlogic S912）+ Armbian（6.x 内核，ophub 系）。
解决：T1 刷 Armbian 后蓝牙完全不可用（`hci0` 从未出现）的问题。

## 背景：为什么 T1 的蓝牙一直不能用

T1 的 WiFi/蓝牙芯片是 BCM4345C0（UART 接口，37.4MHz 晶振）。
Amlogic 官方 DT 与 T1 的社区 DTB 都**没有蓝牙子节点**，因此：

- 内核 `btbcm`/`hci_bcm`（serdev 路线）没有 DT 节点可依附，不会启动芯片；
- 用户态 `hciattach bcm43xx` 路线同样失败——即使通信参数完全正确，
  固件下载过程也会随机冻结（表现为"憋死"或硬件错误洪流 `04 10 01 00`）。

**根因（本项目实测定位）**：芯片 ROM 的 UART 处理无法消化"整包突发写入"。
host 一次 `write()` 连续灌入 16 字节时，芯片内部 FIFO/处理竞争导致数据损坏。

**对策**：把每条 HCI 命令**逐字节发送**（每字节一次 `write` + 间隔 ≥100µs）。
实测 500µs 间隔下，280 块固件（56KB）全量下载 **CC=279 / ER=0**，50.5 秒完成。

## 方案组成

```
t1bt-bluetooth/
├── btflow                  # 初始化程序 (aarch64 预编译; 源码 btflow.c)
├── btflow.c                # 源码 (scene23 = 完整流程)
├── BCM4345C0_....hcd       # 蓝牙固件 (56KB)
├── t1bt-btinit.sh          # 看护脚本 (初始化 + 自愈巡检)
├── t1bt-bluetooth.service  # systemd 单元 (开机自启)
├── install.sh              # 一键安装
└── README.md               # 本文件
```

工作流程（全部在用户态完成，**不需要改内核、不需要改 DTB**）：

```
开机 → t1bt-bluetooth.service
  → GPIO 断电重上电 (芯片全新会话)
  → S7 健康判据 (0xFC2E 命令; 不健康则断电重试, 最多 8 次)
  → 逐字节发送 280 块固件 (500µs/字节, ~50s)
  → 芯片固件激活, 物理速率 89760→115200 → host 切换验证
  → 原位挂 N_HCI ldisc + H4 协议 → 内核出现 hci0
  → fork 常驻进程持有 tty (维持 hci0 存活)
  → bluetoothd 自动接管, 蓝牙可用
```

看护逻辑：只要 `hci0` 消失（holder 进程意外退出等），脚本自动断电重来，
无需人工介入。判定依据 `/sys/class/bluetooth/hci0` 是否存在，动作幂等。

## 安装

```bash
# 在 T1 上（Armbian 系统内）:
tar xzf t1bt-bluetooth.tar.gz
cd t1bt-bluetooth
sudo ./install.sh

# 立即启动（或直接 reboot）
sudo systemctl start t1bt-bluetooth.service

# 约 1~2 分钟后验证:
hciconfig -a          # 应出现 hci0: UP RUNNING, BD Address, HCI 4.2
bluetoothctl show     # 应显示 Powered: yes
tail -f /var/log/t1bt-btinit.log   # 看初始化进度
```

## 验证（实测结果参考）

```
hci0:  Type: Primary  Bus: UART
       BD Address: 80:C5:F2:XX:XX:XX  ACL MTU: 1021:8  SCO MTU: 64:1
       UP RUNNING
       RX bytes:795 ... commands:50 errors:0
       HCI Version: 4.2 (0x8)   Manufacturer: Broadcom Corporation (15)
```

扫描测试：`bluetoothctl --timeout 12 scan on` —— 实测 12 秒扫到 14 个 BLE 设备。

## 故障排查

| 现象 | 检查 |
| --- | --- |
| 开机 5 分钟仍无 hci0 | `tail -50 /var/log/t1bt-btinit.log`，看 try 记录 |
| 反复出现 `REGEN: S7 不健康` | 正常现象（上电命运二相性），脚本会自动重试；若 8 次全失败会等 60s 再来 |
| `TIOCSETD N_HCI` 报错 | `modprobe hci_uart` 后再试（正常已由 bluetooth 模块链自动加载） |
| hci0 出现但 DOWN | `hciconfig hci0 up`；bluetoothd 通常会自动上电 |
| 想看芯片级细节 | `journalctl -u t1bt-bluetooth`; 脚本日志含每次尝试的完整输出 |

## 卸载

```bash
sudo systemctl disable --now t1bt-bluetooth.service
sudo rm -f /etc/systemd/system/t1bt-bluetooth.service /usr/local/bin/t1bt-btinit.sh
sudo rm -rf /root/t1bt
sudo systemctl daemon-reload
# 固件文件可保留（无害）: /lib/firmware/brcm/BCM4345C0_*.hcd
```

## 已知边界

- 速率：稳定运行在 115200。芯片固件拒绝 0xFC18 切到更高速率（返回 0x12），
  115200 对蓝牙音频/扫描等常规用途足够。
- 冷启动耗时：约 50~80 秒完成初始化（含固件下载），期间蓝牙不可用。
- 本方案依赖固定路径：`/root/t1bt/btflow` 与 `/lib/firmware/brcm/BCM4345C0_*.hcd`。
