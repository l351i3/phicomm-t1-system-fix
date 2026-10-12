# CHANGELOG —— phicomm-t1-system-fix

> 本仓库聚焦 T1 的**系统修复线**（蓝牙 + 关机链路 + 救援盘）。
> 遥控器枢纽线的完整变更史见姊妹仓库 xiaomi-bluetooth-remote-hub 的 `docs/04-CHANGELOG.md`。

## 2026-10-07 · 前史

- T1 上做遥控器实验期间内核被覆盖且未备份（只备份了引导配置）→ 系统进入不稳定状态，随后启动还原作业。
- 教训固化：**动内核前，内核 + 引导配置必须成对备份**。

## 2026-10-08 · 蓝牙修复 + 关机链路定位

- **蓝牙全链路打通**：逐层排除 dtb / pinctrl / 时钟后定位"芯片处于 BootROM 态、需用户态注入固件"；`btflow` 下载 280 块固件（CC=279 / ER=0）→ `hci0 UP RUNNING`。
- 开机自动化：`t1bt-btinit.sh` + `t1bt-bluetooth.service`（8 次重试 / 30s 巡检自愈），重启与 kill 自愈实测通过。
- 产出「刷完即用」蓝牙版 Armbian 镜像（见 Releases）。
- **关机链路残留问题**定位：受控实验确认"关机时链路未干净下线"会给直连的上游网络设备带来一次性异常冲击（只拔网线即恢复、插回不复现——一次性事件）。
- 规避规程「先拔线再关机」实证成立；旁路盒子排除实验证明与 T1 之外因素无关。

## 2026-10-09 · 内核补丁（根治）

- 源码级定位：`dwmac-meson8b` 无 `.shutdown` 回调（关机时内核从不管网卡）→ 补丁 `0001-net-stmmac-dwmac-meson8b-drop-the-link-before-power-.patch`。
- **EXP-19 对照实验验收通过**：netconsole 实时捕获补丁打印（`shutdown: bringing the link down before power-off`），多次真实关机 + 8.5h 观测**零异常** → 纯内核方案成立。
- 历史方案 quiet-down 钩子退役（`scripts/quietdown/` 留档；恢复路径：`systemctl enable --now t1-quietdown`）。

## 2026-10-12 · 救援盘交付

- U 盘救援盘制作 + 刷新脚本 + 验收（插盘引导实测；含补丁内核与全部更新）。
- 系统修复线归档整理（本仓库成形）。

## 附录 · 版本与哈希速查

| 资产 | 版本 / 标识 |
|---|---|
| btflow | scene23（终版） |
| 内核补丁 | `0001-…drop-the-link-before-power-.patch`（39 行） |
| 新内核 | `Image-t1-shutdown-6.12.65-ophub`（见 Releases） |
| 内核回滚备份 | `zImage.bak-stock-6.12.65`（见 Releases） |
| 蓝牙版本镜像 | `Armbian_26.11.0_…_bluetooth.img.gz`（见 Releases） |
