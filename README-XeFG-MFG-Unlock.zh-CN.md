# XeFG 多帧生成（MFG）解锁 — 改动说明

> [English](README-XeFG-MFG-Unlock.md) ｜ **中文**
>
> 这是 OptiScaler 的一个**本地修改版**，在 `framegen/xefg` 路径上增加了一项能力：
> 让 OptiScaler 的 **MFG 倍率下拉框** 在非 Intel 显卡上也能出现并实时切换
> （2X 起，上限由 `XeFG\MaxInterpolatedFrames` 决定，默认已不再夹取）。
>
> **修改日期：2026-09-11** ｜ 基线：上游 OptiScaler（GPL-3.0）
> 详细逆向记录见 [`optiscaler xemfg version/`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/README.md)。

---

## ⚠️ 免责声明

- 本项目与 **Intel 无任何关联，未获其认可或授权**。"Intel"、"XeSS"、"XeFG" 等名称
  仅作**指称性使用**（说明兼容对象），不代表任何背书关系。
- 本改动**不包含、不分发、不重打包任何 Intel 二进制文件**。
  它只对**使用者本机已合法拥有**的 `libxess_fg.dll` 在**运行时内存中**打补丁，
  磁盘上的文件**一个字节都不会被修改**。
- 使用者需自行确保对相关软件拥有合法授权，并自行承担使用风险。
- 仅供**互操作性研究与学习**之用。**无任何担保。**
- 本说明不是法律意见。

---

## 1. 解决的问题

Intel 把 MFG（多帧生成）锁在了一个"**我是不是 `igxess_fg.dll` 这个构建**"的判定上，
而**不是**一次真实的硬件能力查询。结果是在非 Intel 适配器上：

| 现象 | 原因 |
|---|---|
| OptiScaler 里**完全没有** MFG 倍率下拉框 | provider 上报 `maxSupportedInterpolations = 1`，而 OptiScaler 的门是 `maxInterpolationCount > 1` —— **报 1 就整块不画** |
| 开局只有 2X，调不上去 | 初始化路径把 MFG 模型降级（17→15 / 18→16） |
| 手动改倍率容易崩（同类 mod） | 把"解锁"与"换档"耦合在改模型权重上 |

本改动把这几道锁在 provider 侧拆开，**解锁与换档彻底分离**：
换档路径上没有任何模型重建，只是改一个整数。

---

## 2. 改动清单

所有改动都在 **`OptiScaler/`** 子目录内，共 **4 个源文件 + 2 个工程文件 + 1 个配置模板**。

### 2.1 新增文件

| 文件 | 说明 |
|---|---|
| [`OptiScaler/proxies/XeFGUnlock.h`](OptiScaler/proxies/XeFGUnlock.h) | **新建**（约 290 行）。补丁引擎：五条补丁的字节表、`.text` 边界检查、逐字节比对、写入回读校验、失败整体回滚。头文件式实现，无新增编译单元。 |

### 2.2 修改文件

| 文件:行 | 改动 |
|---|---|
| [`OptiScaler/proxies/XeFG_Proxy.h:7`](OptiScaler/proxies/XeFG_Proxy.h#L7) | `#include "XeFGUnlock.h"` |
| [`OptiScaler/proxies/XeFG_Proxy.h:180`](OptiScaler/proxies/XeFG_Proxy.h#L180) | 在 `HookXeFG()` 里、`_dll = libxefgModule;` **之后**调用 `XeFGUnlock::Apply(_dll);` —— 必须晚于 DLL 映射、早于任何 `xefgSwapChain*` 调用 |
| [`OptiScaler/Config.h:594-595`](OptiScaler/Config.h#L594-L595) | 新增两个配置字段 `FGXeFGUnlockEnabled{true}` / `FGXeFGMaxInterpolatedFrames{31}` |
| [`OptiScaler/Config.cpp:216-226`](OptiScaler/Config.cpp#L216-L226) | 读 `XeFG\UnlockMFG` / `XeFG\MaxInterpolatedFrames`，后者超出 1..31 时回退默认 |
| [`OptiScaler/Config.cpp:1005-1008`](OptiScaler/Config.cpp#L1005-L1008) | 把这两项写回 ini，否则改过的值重启就丢 |
| [`OptiScaler/Config.cpp:219`](OptiScaler/Config.cpp#L219) | **顺带修掉一个上游 bug**：`FGXeFGInterpolationCount` 的钳位写的是 `> 3`，与本文件里其它地方使用的 **5**（下拉框有 2X..6X 五档）不一致。不修的话选 5X/6X 之后一重启就被 `reset()` 掉 |
| [`OptiScaler.vcxproj:483`](OptiScaler.vcxproj#L483) | 登记新头文件 |
| [`OptiScaler.vcxproj.filters:500`](OptiScaler.vcxproj.filters#L500) | 同上（筛选器） |
| [`OptiScaler.ini:236-260`](OptiScaler.ini#L236-L260) | **默认配置模板**补上两个新键的说明；并把 `InterpolationCount` 那句已过时的 `1 = 2X \| 2 = 3X \| 3 = 4X` 补全为到 6X |

### 2.3 **没有**改动的

- **`framegen/xefg/XeFG_Dx12.cpp` 一行未动。** 上报值一改，OptiScaler 既有的
  `_maxInterpolationCount` / `SetNumInterpolatedFrames()` 逻辑原样开始工作 ——
  UI 与换档完全沿用上游代码。
- **没有移植"帧节奏（pacing）"补偿。** XeSSMFG 需要它是因为它作为 `dxgi.dll` 代理
  接管了 Present、必须自己补节奏；OptiScaler 走 provider 的公开
  `xefgSwapChain*` API，节奏由 provider 自己负责（OptiScaler 里本来就没有 XeFG
  pacing 代码）。移植它意味着 hook 一个**非导出、且不由我们控制的** DLL 内部地址，
  正是那个 mod 崩溃的根源。理由详见 `08-PACING.md`。
- **没有动 `libxess_fg.dll` / `libxell.dll` / 任何 Intel 二进制。**

---

## 3. 五条补丁做了什么

全部打在 `libxess_fg.dll` 的**已映射内存镜像**上，`file offset = RVA − 0xC00`。
`N` = `XeFG\MaxInterpolatedFrames`（默认 31，合法范围 1..31）；`N == 1` 时**一个字节都不改**。

| # | RVA | 原 → 新 | 作用 |
|---|---|---|---|
| U1 | `0x20DA4F` | `0F 85 CC 00 00 00` → `E9 CD 00 00 00 90` | 每帧解析器不再在"XeLL 版本过旧"时回落到 2X |
| U2 | `0x1A5DE4` | `74 09` → `EB 06` | `Settings::mfgAllowed()` 恒真，初始化不再把 MFG 模型 17→15 / 18→16 降级 |
| U3 | `0x1A517D` | `BB 03 00 00 00` → `BB N …` | 抬高默认插帧上限（`max(下界, 覆盖值)`） |
| U4 | `0x1A45C2` | `C7 87 6C 01 00 00 01 00 00 00` → `… N …` | provider 自判"不支持 MFG"时，不再把覆盖值钉死成 1 |
| U5 | `0x20973B` | `B8 01 00 00 00` → `B8 N 00 00 00` | `xefgSwapChainGetProperties` 不再硬编码上报 1 —— **下拉框出不出现就看这一刀** |

> ⚠️ **U4 / U5 必须按 RVA + 整条指令逐字节比对来打。**
> `B8 01 00 00 00` 在 `.text` 里出现 **1669 次**、
> `C7 87 6C 01 00 00 01 00 00 00` 出现 **2 次** ——
> 任何"模式替换"式实现都会把 DLL 打烂。

打完五刀后，provider 上报上限的四条分支**全部收敛到 N**。

---

## 4. 新增配置项

写在 `OptiScaler.ini` 的 `[XeFG]` 段：

```ini
[XeFG]
; 解锁开关。默认 true
UnlockMFG=true

; 上报的插帧数上限 N，菜单里的倍率是 N+1。
; 1 = 2X … 31 = 32X。默认 31（我们这边不再夹上限）
MaxInterpolatedFrames=31
```

两者都可随时改，改完重启生效。`MaxInterpolatedFrames=1` 等价于完全关闭解锁。

**注意区分两个键**：`MaxInterpolatedFrames` 决定"**能选到几档**"，
`InterpolationCount` 决定"**开局用哪档**"。两者独立。

> ⚠️ 这个值**不只是菜单边界**。`XeFG_Dx12.cpp:366-385` 会在 swapchain 初始化时
> 把 provider 报告的上限原样填进
> `xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames`，所以把它报高
> 等于**每次启动都向 provider 声明这个数字**。1..6 是实测过的范围，更高**未验证**；
> 如果 provider 按这个数预分配资源，症状会是初始化失败或显存耗尽 ——
> 退回去的办法是设为 5。

---

## 5. 构建

与上游一致，无额外依赖：

```powershell
msbuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64
```

产物：`x64\Release\OptiScaler.dll`。

---

## 6. 关闭 / 回滚

三档，任选其一：

1. **改配置**（不动任何文件）—— `OptiScaler.ini` 里把 `XeFG\UnlockMFG` 设为 `false`，
   或把 `XeFG\MaxInterpolatedFrames` 设为 `1`。后者会让补丁**一条都不打**，
   provider 保持出厂行为。
2. **换回原版** —— 用上游构建覆盖即可。磁盘上的 Intel DLL 从未被修改，
   不需要做任何清理。
3. **单点失败也安全** —— 任一条补丁的字节比对不通过（例如 provider 换版本），
   引擎会**整体回滚**，provider 按原样运行。**失败是"没有 MFG"，不是崩溃。**
   版本不匹配时日志里会看到 `unrecognised provider build` 与 `rolled back`。

---

## 7. 验证状态

**2026-09-11 于 Cyberpunk 2077 实机验证通过**（RTX 2060，`libxell.dll` 1.3.0）：

- 五条补丁全部生效：`5 of 5 patches applied (0 skipped)`
- 上报值从 **1 变成 5**：`Max supported interpolations: 5`
- 倍率下拉框出现，2X..6X 可选
- **90 秒内连切 6 次（`3↔5↔4`）无任何崩溃**

**仍未做**：4X 以上的长跑画质与稳定性测试；6X 以上更是一个都没验过 ——
那次验证之后默认上限才从 5 抬到 31，所以 5X+ 和当时一样仍是空白。
若高倍率不稳定，把 `MaxInterpolatedFrames` 改成 `3`（4X，验证最充分的档位）。

---

## 8. GPL-3.0 修改声明

依据 **GNU GPL v3 §5(a)**：
*"The work must carry prominent notices stating that you modified it, and giving a relevant date."*

> 本仓库是 [OptiScaler](https://github.com/optiscaler/OptiScaler) 的修改版。
> 修改日期 **2026-09-11**，修改内容见本文 §2 的完整清单
> （新增 `OptiScaler/proxies/XeFGUnlock.h`，修改 `XeFG_Proxy.h`、`Config.h`、
> `Config.cpp`、`OptiScaler.vcxproj`、`OptiScaler.vcxproj.filters`、
> `OptiScaler.ini`）。
>
> 原始作品版权归 OptiScaler 作者所有，按 **GPL-3.0** 授权。
> 本修改版同样按 **GPL-3.0** 授权，完整许可证见 [`LICENSE`](LICENSE)。
> 修改部分的完整对应源码即本仓库源码。

---

## 9. 详细文档

完整的逆向分析、逐条安全性论证与实机日志在：

```
..\XeSSMFG-Universal-v0.2.0.1\optiscaler xemfg version\
```

| 文档 | 内容 |
|---|---|
| `README.md` | 总览与结论 |
| `01-THE-LOCK.md` | 锁的完整链路与调用图 |
| `02-UNLOCK-PATCH.md` | 补丁字节表 + 逐条安全性论证 |
| `07-WHY-NO-COMBO.md` | 为什么下拉框不出现（U4/U5 的由来） |
| `08-PACING.md` | pacing 是什么、为什么不移植 |
| `09-CHANGELOG-U4U5.md` | 本轮改动记录与实机证据 |
| `RVA-MAP.md` | RVA / 文件偏移速查 |
