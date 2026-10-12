# phicomm-t1-system-fix

> 斐讯 T1（PHICOMM-T1，Amlogic S912）在 Armbian 6.12 上的两处系统级修复：
> **① 蓝牙固件加载（BCM4345C0）· ② 软件关机链路残留修复（内核补丁）**，
> 外加一套 **U 盘救援盘** 与全部验收证据。
>
> 同样适用：同 SoC 系列（meson-gxm）且使用 BCM UART 蓝牙的盒子，可作为参考移植。

---

## 一、解决什么问题

### ① 蓝牙在 6.12 内核上不可用（BCM4345C0）

- **现象**：主线内核的 `hci_bcm` 驱动假设 UART 已由 bootloader 加载好固件；而 T1 上电后芯片停留在 BootROM 态，驱动拿不到应答 → `hci0` 起不来，遥控器/蓝牙全部不可用（`-84` 噪声、扫描为空）。
- **修复**：用户态固件加载器 **`btflow`**——逐字节把 `.hcd` 固件下载进芯片（500µs/字节、280 块），再原位挂 HCI（ldisc）→ `hci0` UP RUNNING。
- **交付**：`bluetooth/` 完整包（二进制 + C 源码 + 安装脚本 + 开机自愈服务）+ 已注入的 Armbian 镜像（见 Releases，「刷完即用」）。

### ② 软件关机时的链路残留（内核补丁）

- **现象**：T1 软关机（`poweroff`/`reboot`）流程中，网卡驱动 `dwmac-meson8b`（stmmac 系）**没有 `.shutdown` 回调**——关机时内核从不主动放下以太网链路，链路以"仍在位"的残留状态随断电消失。实测该残留会给直连的上游网络设备带来一次性异常冲击（表现为上游链路短暂失能，需链路变化才复位）。
- **修复**：内核补丁 `0001-net-stmmac-dwmac-meson8b-drop-the-link-before-power-.patch`——新增 `.shutdown` 回调：`rtnl_lock` → `dev_close`（停 DMA / 停 PHY）→ `msleep(5000)` 宽限，保证**任意有序关机/重启流程自动干净下链**。
- **验收**：EXP-19 对照实验（runbook 全文在 `docs/`）——netconsole 实时捕获补丁打印，多次真实关机 + 长时间高危暴露观测**全程零异常**；另有历史方案（quiet-down 钩子，`scripts/quietdown/`）留档，已被内核补丁取代。
- **上游**：补丁按内核社区格式书写（checkpatch 0 error / 0 warning），可直接用于向上游 stmmac 提交。

---

## 二、目录结构

```
phicomm-t1-system-fix/
├── bluetooth/              # 蓝牙修复包：btflow（二进制 + C 源码）、安装脚本、.hcd 固件、开机服务
├── kernel/
│   ├── patches/            # dwmac-meson8b shutdown 补丁（关机前干净放下链路）
│   └── evidence/           # 验收物证：netconsole 实时打印 / quiet-down 日志终版
├── rescue-usb/             # U 盘救援盘：制作脚本、引导文件、刷新脚本、验收记录
├── scripts/
│   ├── quietdown/          # 关机前 quiet-down 钩子（历史方案，已被内核补丁取代）
│   └── experiments/        # 关机对照实验脚本（4A / 4B）
├── evidence/
│   ├── netwatch/           # 逐秒连通性探针（脚本 + 实验日志）
│   └── n1/                 # 旁路盒子排除实验（证明非 T1 之外的因素）
└── docs/EXP-19-runbook.md  # 内核补丁独立验收 runbook（完整实验记录）
```

大文件（预编译内核、Armbian 镜像）走 **Releases** 分发，不进 git。

---

## 三、快速开始

### 蓝牙（在 T1 上）

```bash
# 见 bluetooth/README.md（详解）；最小路径：
scp -r bluetooth/ root@<T1_IP>:/root/t1bt-bluetooth/
ssh root@<T1_IP> 'cd /root/t1bt-bluetooth && ./install.sh'
# 安装后：t1bt-bluetooth.service 常驻（开机自愈），hci0 自动上线
```

### 内核补丁（自制内核）

```bash
# 以 ophub 6.12.y 源码树为例
cd linux-6.12.y
patch -p1 < .../patches/0001-net-stmmac-dwmac-meson8b-drop-the-link-before-power-.patch
make -j$(nproc) LOCALVERSION=-ophub Image
# 安装：替换 /boot/zImage（务必先备份原 zImage）→ 重启
# 回滚：cp /boot/zImage.bak-stock-* /boot/zImage && reboot
```

### 救援盘（U 盘）

```bash
# 见 rescue-usb/make-rescue-usb.sh：分区、写入 BOOT/ROOTFS、验证引导
# 用法：插盘上电即从 U 盘引导（U-Boot boot_order = usb→mmc）；拔盘回 eMMC
```

---

## 四、验证与证据

| 项 | 证据 |
|---|---|
| 蓝牙 | `btflow` 下载日志（CC=279 / ER=0）→ `hci0 UP RUNNING`；重启 + kill 自愈实测 |
| 关机链路 | `kernel/evidence/netconsole-reboot-print-2026-10-09.txt`（补丁实时打印）；`docs/EXP-19-runbook.md`（6 次动作时间线 + 8.5h 观测） |
| 救援盘 | `rescue-usb/RESCUE-USB-验收.md`（插盘引导实测 + 取证） |

---

## 五、占位符约定

文档与脚本中的地址均为占位符，部署时替换为你的环境值：

| 占位符 | 含义 |
|---|---|
| `T1_IP` | 盒子的局域网地址 |
| `WINDOWS_IP` / `MACOS_IP` | 目标主机（本修复线主要为 T1 侧，遥控枢纽项目见下方姊妹仓库） |
| `GW_IP` / `LAN_IP` | 网关 / 其他内网主机 |
| `username` | 用户名占位 |

---

## 六、姊妹项目

- **[xiaomi-bluetooth-remote-hub](https://github.com/l351i3/xiaomi-bluetooth-remote-hub)** —— 小米蓝牙遥控器三端枢纽（T1 中继 + macOS / Windows 目标端）；本仓库是其"系统修复线"的独立归档。

---

## 许可与免责

- 本仓库为个人自用系统的开源存档，**暂未附开源许可证**；如需复用，请先开 issue 联系。
- 代码按「现状」提供，不附带任何担保；刷机、改内核均有变砖风险，请自行评估并先做好备份。
- 蓝牙固件（`.hcd`）来自 Broadcom/linux-firmware 公开分发。
