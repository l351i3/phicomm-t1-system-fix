# EXP-19 Runbook：新内核对照实验（meson8b-dwmac shutdown 补丁独立验收）

- **日期**：2026-10-09
- **设备**：斐讯 T1（PHICOMM-T1，Amlogic S912）
- **状态**：✅ **已完成（2026-10-09 08:57 收尾）**——验收通过：零异常实锤 + netconsole 物证；最终决策「纯内核方案」，钩子已退役。结果详见 §6
- **目标**：隔离 t1-quietdown 钩子变量（marker 旁路），验证内核补丁 `.shutdown` 回调能否**独立**实现「关机干净下链」（零异常）

---

## 0. 当前状态（准备阶段已完成项）

| 项          | 状态                   | 证据                                                                                |
| ---------- | -------------------- | --------------------------------------------------------------------------------- |
| 新内核安装      | ✅ 已装为 `/boot/zImage` | sha256 `32aef84a…`，`cmp` 与编译产物逐字节一致                                               |
| stock 回滚备份 | ✅ 双份                 | T1 `/boot/zImage.bak-stock-6.12.65`（`5691411d…`）+ Mac `boot-backup/`              |
| 引导链        | ✅ 无需改动               | `uEnv.txt: LINUX=/zImage` 不变；boot.cmd 直接从 eMMC 加载                                 |
| 钩子旁路装置     | ✅ 已部署 + 单测通过         | `t1-quietdown.sh` 含 EXP19 bypass 块（`0daec1ff…`）；单测：marker 存在→脚本 exit 0、eth0 保持 UP |
| 钩子原版备份     | ✅                    | `t1-quietdown.sh.pre-exp19`（`24db0acc…`）                                          |
| 观测链路       | ✅ 长跑中                | netwatch（Mac）每秒采样 T1/GW2/R1/NET                                                   |
| 版本判定特征     | ✅                    | stock=`Thu Jan 15 04:03:19 UTC 2026` vs 新内核=`Thu Oct 8 23:45:49 CST 2026`         |
| 回滚手册       | ✅ 本文件 §2             | 三层预案                                                                              |

---

## 1. 判定口径

**主观测（netwatch）**：

- 预期：T1 列 `OK→FAIL`（设备关机，正常）；**`NET`/`R1`/`GW2` 列保持 OK**
- 异常特征：`NET` 列在 poweroff 后 12~20s 内变 FAIL 且**持续**（>30s）直到链路变化

**副观测**：上游设备侧核验由维护者内部完成（不随本库发布）。

**辅助（T1 侧留痕）**：

- 关机后 `quietdown.log` **不应有新增条目**（钩子被旁路，从未执行动作）——反证变量隔离成功
- 内核回调的 `netdev_info` 消息记录在 journal（`shutdown: bringing the link down before power-off`），验证后可见

---

## 2. 回滚预案（三层）

**触发条件**：重启后 T1 失联 > 3 分钟，或新内核启动异常。

```bash
# 层 1：SSH 可用（启动成功但有异常）
cp /boot/zImage.bak-stock-6.12.65 /boot/zImage && systemctl reboot
```

**层 2：T1 完全失联 → 用户物理操作**：

1. 用户拔插电源一次（冷重启，先试普通回引导）
2. 仍失联 → **U 盘救援**：插救援 U 盘上电（插盘即引导 U 盘系统）→ 我远程执行：
   ```bash
   # （救援系统的挂载点以现场侦察为准）
   mkdir -p /mnt/emmc && mount /dev/mmcblk2p1 /mnt/emmc
   cp /mnt/emmc/zImage.bak-stock-6.12.65 /mnt/emmc/zImage && sync && umount /mnt/emmc
   # 断电、拔 U 盘、重新上电
   ```

**层 3：Mac 异地终极兜底**：

- `remote-hub/t1-kernel/boot-backup/zImage.bak-stock-6.12.65`（sha256 `5691411d…`）
- 新内核归档：`remote-hub/t1-kernel/Image-t1-shutdown-6.12.65-ophub`

**链路异常应急**（万一实验中出现，虽然预期不会）：拔 T1 电源或网线 → 网络 3~6s 自愈 → 复查补丁设计。

---

## 3. 实验步骤

### 阶段 A：新内核首启验证（我执行；失败时可能需要用户拔电）

1. 基线记录：`uname -a`（stock）、netwatch 尾部、时间戳
2. `touch /root/kernel/EXP19-HOOK-BYPASS`（旁路装置挂上，生产态标记）
3. `systemctl reboot`
4. 等待 ~90s，连通性探测；恢复后验证清单：
   - [ ] `uname -a` = `#1 SMP PREEMPT_DYNAMIC Thu Oct 8 23:45:49 CST 2026`（新内核标定）
   - [ ] `grep meson8b_dwmac_shutdown /proc/kallsyms`（补丁符号在内核中）
   - [ ] `ip -br link show eth0` = UP；`dmesg | grep dwmac` 无异常
   - [ ] 服务：sshd / tailscaled / t1-quietdown（active）
   - [ ] netwatch 全绿
5. 失败 → §2 回滚预案

### 阶段 B：带线关机对照实验（隔离钩子）

1. 基线快照：`quietdown.log` 尾部行数/时间戳、netwatch 尾部（实验起点打时间戳）
2. `systemctl poweroff`（**网线保持连接**）
3. 观测 ≥ 5 分钟：
   - netwatch：预期 T1=FAIL，NET/R1/GW2=OK
   - 若 NET=FAIL 持续 → 判定异常复现（补丁无效）→ 立即请用户拔 T1 电源/网线恢复网络
4. **【用户操作】观测确认后：拔插电源** → T1 上电 → eMMC 引导新内核
5. 等待回线（~90s），终验：
   - [ ] `uname -a` 连续一致（第二次启动仍为新内核）
   - [ ] `quietdown.log` **无新增条目**（钩子全程未动作 = 隔离成功）
   - [ ] 网络全链路 OK（netwatch）
6. `rm /root/kernel/EXP19-HOOK-BYPASS`（钩子恢复生产态）

### 阶段 C：收尾与结论

- [ ] 结论汇报 + 文档沉淀（内部取证记录更新为"已验证"）
- [ ] 钩子去留决策：保留（双保险）或退役（纯内核方案）——交用户拍板
- [ ] 交付清单：patch / Image / runbook / 验证日志

---

## 4. 用户操作清单（全程物理操作共 1~2 次）

| # | 时机            | 操作                 | 必要性 |
| - | ------------- | ------------------ | --- |
| 1 | 阶段 B 观测 5 分钟后 | **拔插 T1 电源**（重新上电） | 必然  |
| 2 | （仅当阶段 A 失败）   | 拔电重启一次；必要时插救援 U 盘  | 预案  |


---

## 5. 变更记录（T1 端）

| 文件                                           | 变更                                | 时间               |
| -------------------------------------------- | --------------------------------- | ---------------- |
| `/boot/zImage`                               | stock → 新内核（`32aef84a…`）          | 2026-10-09 00:15 |
| `/boot/zImage.bak-stock-6.12.65`             | 新增（stock 备份）                      | 同上               |
| `/root/remote-hub/t1-quietdown.sh`           | 加 EXP19 bypass 块（无 marker 时行为=原版） | 00:18            |
| `/root/remote-hub/t1-quietdown.sh.pre-exp19` | 原版备份                              | 00:18            |
| `/root/remote-hub/t1-quietdown.sh.exp19`     | 部署参考版                             | 00:18            |

---

## 6. 实验执行结果与收尾（终态，2026-10-09 09:00）

**结论**：✅ **验收通过**——内核补丁（`meson8b_dwmac_shutdown`）在钩子完全退让（marker 旁路，6 次零动作）的条件下独立防住链路异常；用户拍板**纯内核方案**，钩子已退役。

### 6.1 执行时间线（6 次动作，全部零异常）

| # | 时刻（10-09 CST） | 动作 | 钩子留痕 | T1 断开窗口 | NET/R1/GW2 |
|---|---|---|---|---|---|
| 0 | 00:18:02 | 部署单测（stop/start，非关机） | BYPASS | 无（T1 全程在线） | 全 OK |
| 1 | 00:19:16 | 阶段 A：reboot 首启验证 | BYPASS | 00:19:22→:45（24s） | 全 OK |
| 2 | 00:20:19 | 阶段 B：带线 poweroff | BYPASS | 00:20:24→00:33:14 | 全 OK |
| 3 | 08:37:29 | 采集轮 #1 reboot | BYPASS | 08:37:34→08:38:03 | 全 OK |
| 4 | 08:38:44 | 采集轮 #2 reboot | BYPASS | 08:38:48→08:39:18 | 全 OK |
| 5 | 08:40:20 | 采集轮 #3 reboot（物证轮） | BYPASS | 08:40:24→08:40:54 | 全 OK |

- 阶段 B 带线 poweroff 后"软关机·市电未断·网线在位"静置 12 分 55 秒（历史链路异常场景活体复现）→ 零异常
- netwatch 全日志终审：`NET=FAIL` 仅 6 行（10-08 21:38 已排除噪声）；10-09 00:20 起 NET=FAIL = 0（8.5h+ 持续观测）

### 6.2 决定性物证（netconsole 实时捕获）

`netconsole-reboot-print-2026-10-09.txt`（116 行 / 27 包）：

```
[   84.017087] meson8b-dwmac c9410000.ethernet eth0: shutdown: bringing the link down before power-off
```

时序三联：ExecStop 留痕 08:40:20 → 补丁打印（内核 84.0s，≈08:40:24）→ netwatch T1=FAIL 08:40:24 → 恢复 08:40:55。补丁下链时刻与外部观测断开时刻秒级吻合。

### 6.3 验收三要素

- [x] 新内核启动正常（5 次启动）：`#1 SMP PREEMPT_DYNAMIC Thu Oct 8 23:45:49 CST 2026` vs stock `Thu Jan 15 04:03:19 UTC 2026`
- [x] 运行内核含补丁符号：`meson8b_dwmac_shutdown`（kallsyms = 1）
- [x] 关机零异常：见 6.1 / 6.2

### 6.4 最终决策与退役动作（用户拍板：纯内核方案）

执行顺序（关键：**先 stop 再删 marker**——stop 时 marker 在位，ExecStop 走 BYPASS no-op，eth0 无损）：

1. `systemctl stop t1-quietdown` → inactive；ExecStop 留痕 #7（BYPASS no-op）
2. `systemctl disable t1-quietdown` → disabled（wants 软链已移除）
3. `cp t1-quietdown.sh.pre-exp19 t1-quietdown.sh` → 脚本还原原版（sha256 `24db0acc…`，EXP19 引用 0）
4. `rm /root/kernel/EXP19-HOOK-BYPASS` → ABSENT
5. 终验：`is-enabled=disabled` / `is-active=inactive` / wants-link=0

**恢复路径**（若未来内核被覆盖、需钩子兜底）：`systemctl enable --now t1-quietdown`

### 6.5 终态资产清单

| 资产 | 位置 | 状态 |
|---|---|---|
| 新内核 | T1 `/boot/zImage`（sha256 `32aef84a…`） | **运行中（生产）** |
| stock 备份 ×3 | T1 `/boot/zImage.bak-stock-6.12.65` + Mac `boot-backup/` + 新内核归档 `Image-t1-shutdown-6.12.65-ophub` | 在位 |
| 补丁 | `patches/0001-net-stmmac-dwmac-meson8b-drop-the-link-before-power-.patch` | 已交付（commit `be53dcb`） |
| quietdown 日志终版 | `quietdown-log-final-2026-10-09.txt`（16 行 = 8 条目：1 原版 + 7 BYPASS） | 已归档 |
| netconsole 物证 | `netconsole-reboot-print-2026-10-09.txt` | 已归档 |
| 钩子脚本（原版） | T1 `/root/remote-hub/t1-quietdown.sh`（`24db0acc…`） | 退役备用 |
| 钩子脚本（实验版） | T1 `/root/remote-hub/t1-quietdown.sh.exp19` | 留档 |

### 6.6 遗留（低优先）

- 新内核启动期 `BPF: Invalid name_offset` 报错 78 条（无功能影响，疑似编译工具链 BTF 差异，待查）
