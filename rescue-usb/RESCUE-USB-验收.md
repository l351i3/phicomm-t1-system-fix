# T1 U 盘救援盘 · 制作与验收记录（2026-10-12）

> 目标：系统出问题时，插上 U 盘 + 上电即可继续使用（平时不插，正常走 eMMC）。
> 状态：**全部完成 ✅ —— 原地刷新 + 实测①插盘引导 + 实测②拔盘回 eMMC（双向闭环）。**

## 1. 盘身份

| 分区 | 大小 | 格式 | LABEL | UUID |
|---|---|---|---|---|
| sda1 | 511M | vfat | BOOT | 36AD-BCF6 |
| sda2 | 28.1G | ext4 | ROOTFS | 88e4250c-7462-43f5-bb12-525a6e5b2b7c |

- 硬件：SanDisk Ultra USB 3.0 28.6G（即 10-08 修复 eMMC 时所用的那块 T1 系统盘）。
- 引导机制：T1 U-Boot `boot_order="usb mmc"`（USB 优先）——插盘上电即从 U 盘引导；拔盘自动回 eMMC；引导失败自动回退，**无变砖风险**。
- 引导层为 10-08 久经考验结构（boot.ini 自适应版 + boot.scr 双设备版 + 原厂 autoscript 兼容层），本次**原地刷新、引导脚本零改动**。

## 2. 刷新执行 ✅（脚本外验证证据见 verify-20261012.txt）

- `rescue-refresh.sh`：EXIT=0，16:54:25–16:56:03（1m43s）。
- rootfs 全量同步：1,184,411,263 B / 26,160 文件（rsync --delete）；样本对账全等（/etc 1589、/usr 50266、/opt 3231、/root 246、/var/lib 2567）。
- 内容更新：zImage `32aef84a…` + uInitrd `73c640bb…` + dtb 全套（含 -nott）→ 与 eMMC 逐字节一致；fstab 改 `LABEL=BOOT` + 自身 UUID；uEnv FDT=…-nott.dtb；禁用 armbian_first_run；清 macOS 垃圾。
- 脚本外独立复验：boot.ini / boot.scr / boot.cmd 与刷新前备份逐字节相同（引导层未被触碰）。

## 3. 实测①：插盘重启 → U 盘引导 ✅（17:01）

- 35 秒恢复 SSH：`/ = /dev/sda2（ROOTFS）`、`/boot = /dev/sda1（BOOT）`、cmdline `root=UUID=88e4250c…`；eMMC 分区全程未挂载。
- 七服务全 active（bluetooth / t1bt-bluetooth / t1-evwatch / t1-voice-fwd / t1-atvv / t1-relay / t1-webui）、零失败单元。
- 网络：eth0 T1_IP + tailscale0 正常；webui 本机 HTTP 200。
- 蓝牙栈健康：hci0 在位（80:C5:F2:…，Powered）；**遥控器已自动重连**（HOG 输入设备 bluez-hog-device 于 17:02:54 就绪）——按键/语音链路随时可用。

## 4. 实测②：拔盘重启 → 回 eMMC ✅（17:21）

- 流程：T1 干净关机（17:19:47 确认离线）→ 拔 U 盘 → 断电重插 → 自动回 eMMC 引导。
- 证据（[C] 快照，自动检测抓取）：`/ = /dev/mmcblk2p2（ROOTFS_EMMC）`、`/boot = /dev/mmcblk2p1（BOOT_EMMC）`、cmdline `root=UUID=d6082f8e…`；**lsblk 中无 sda——U 盘确已拔出**；七服务全 active；eth0 T1_IP + tailscale 正常；遥控器 17:24 重连成功（evwatch 已见 event2 键事件——按键/语音链路恢复）。
- **双向闭环达成：插盘上电 = U 盘系统；拔盘上电 = eMMC 系统。**

> 备注：快照抓取时 T1 刚完成引导（uptime 0 分钟），其日志时间为无 RTC 假时钟值（NTP 校正后为真实时间）——与 §5 同一已知现象，非异常。

## 5. 疑点排查记录（均澄清）

- **日志「16:17」时间戳**：T1 无 RTC——引导早期 clock 取 fake-hwclock 旧值，NTP 校正后跳回真实时间。属正常行为，非异常。
- **BPF `Invalid name_offset` 日志（44 条）**：内核在**加载模块时**打印的 BTF 外观性警告。复查时现场 `modprobe nfs` 复现 +2 条（44→46）→ 实锤＝模块加载触发；属该自定义内核（6.12.65-ophub，10-08 构建）的既有特性，**与 U 盘引导无关**（eMMC 同内核同模块行为一致），零功能影响。
- 从 Mac 发起的 :8765 curl 检查被本机权限层拦截（未重试）；T1 本机与浏览器路径不受影响。

## 6. 日常使用 & 复刷新

- **救援**：插 U 盘 → 上电/重启 → 自动从 U 盘跑（=当前系统的完整副本，含关机链路异常补丁内核与全部更新）。
- **恢复日常**：拔 U 盘 → 重启 → 回 eMMC。
- **保持同步**（eMMC 系统做大更新后建议重跑，否则救援盘落后）：

```bash
ssh t1root
bash /root/rescue-usb/rescue-refresh.sh --yes
```

## 7. 文件

- 本目录：`RESCUE-USB-验收.md`（本文）/ `verify-20261012.txt`（[A] 刷新后静态复验 + [B] U 盘引导快照 + [C] 拔盘回 eMMC 快照）/ `refresh-20261012.log`（刷新日志，已去 rsync 进度刷屏）/ `rescue-refresh.sh` / `make-rescue-usb.sh` / `boot.cmd.usb`
- T1：`/root/rescue-usb/`（脚本 + 日志）、`/root/rescue-refresh-backup-20261012/`（刷新前备份：引导脚本 14 件 + rootfs 痕迹 2618 文件）
