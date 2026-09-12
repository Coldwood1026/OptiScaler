# XeFG 帧节奏（Frame Pacing）— 改动说明

> [English](README-XeFG-Pacing.md) ｜ **中文**
>
> 本文只讲本 OptiScaler 本地修改版里的**一项能力**：让 Intel 生成的帧在 2X 以上
> **均匀地吐出来**，而不是"挤成一坨、后面空一段"。
>
> **修改日期：2026-09-12** ｜ 基线：上游 OptiScaler（GPL-3.0）
> 姊妹文档：[`README-XeFG-MFG-Unlock.zh-CN.md`](README-XeFG-MFG-Unlock.zh-CN.md)
> 详细逆向记录见 [`optiscaler xemfg version/`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/README.md)。

---

## ⚠️ 免责声明

与 MFG 解锁文档一致：与 **Intel 无任何关联**、**不分发任何 Intel 二进制文件**、
所有补丁都在**运行时内存中**完成、**磁盘上的文件一个字节都不改**。
仅供互操作性研究之用，无任何担保，不是法律意见。

---

## 1. 解决的问题

MFG 解锁之后，3X 及以上才变得可达。而 3X 以上的观感不对：帧**挤在一起，后面跟一段空档**，
表现为抖动，倍率越高越像帧序错乱。

根因不是 provider 没有"间隔"这个概念。它有，而且**算得是对的** —— 每个 burst 算一次，
在 `0x220254`：

```asm
0x220254  mov  r9, [r8+8]        ; count  = 这个 burst 里生成的帧数
0x220258  lea  rcx, [r9+1]       ; count + 1 = 倍率
0x22025E  mov  rax, [rbp-0x41]   ; 一个真实帧的时长
0x220262  div  rcx
0x220265  mov  r12, rax          ; -> 时长 / 倍率，即每帧应有的间隔
```

问题在于这个数没被用上。紧随其后的呈现循环（`0x220280..0x22030A`）把每个生成帧
**背靠背**丢给交换链，而 `r12` 要等循环结束才被消费 —— 被 `0x220317` 那个限帧块用掉，
而那个块还额外要求 `count > 2` 以及另外两个条件成立。于是 burst 以交换链能接受的
最快速度吐出去，限帧块再把**最后一帧**按住不放。这就是那一坨。

还有第二个更隐蔽的缺陷。provider 自己有一套真正的**帧调度器**，在 `0x21EE30`，
它给每个帧从 `0x3430` → `0x224B30` 算出一个呈现时刻：

```
deadline = base
         + min( index * median / (count + 1),          ; 纳秒
                f * index / (count + 1) * 1000000 )   ; 毫秒 -> 纳秒
         - recv * 1000000
```

这个公式有两处问题：

| 缺陷 | 后果 |
|---|---|
| `base` 只有**一处**写入 —— 那个 fence 块，而它只在 `gate != 0 && index == 1` 时运行 | 其它所有 index 的 deadline 都是拿**那个栈槽里碰巧剩下的东西**算出来的。provider 自己那次调用带的 `index == count`，所以 2X 以上它唯一的一次调用就是**拿垃圾算的**。而 2X 恰好是它自己那次调用 `index == 1` 的唯一倍率 —— 2X 也正是唯一一个从来就正常的倍率。 |
| `min()` 把步长夹到 `f`，即 `ring+0x1B8` 的那个 float，夹在 `[0.125, 500.0]` 毫秒 | `f` 比真实帧间隔小 —— **4X 实测 8.5 ms，而真实帧是 20.6 ms**。步长算出来 **2.13 ms，而 burst 需要 5.03 ms**。 |

而且 provider **只给每个 burst 的最后一帧**调这个调度器（从 `0x220411` 调）。
其余每个生成帧被呈现时，根本没有为它算过任何呈现时刻。

另外还有一条会让整件事**自锁**的帧时间反馈通路。`FG_Hooks::FGPresent` 在入口处
（任何帧生成工作**之前**）采一次时钟，这个差值被当作 `frameRenderTime` 交给 provider；
provider 用它决定 burst 的规模，而且 `Present()` 不返回直到整串吐完。于是形成一个不动点：

```
period = renderTime * (count + 1)
```

burst 阻塞得越久，告诉 provider 的渲染时间就越大，下一串就被允许越慢。

---

## 2. 改了什么

全部位于 **`OptiScaler/`** 下。新增一个头文件，改动三个文件，外加配置管路。

### 2.1 新增

| 文件 | 说明 |
|---|---|
| [`OptiScaler/proxies/XeFGPacing.h`](OptiScaler/proxies/XeFGPacing.h) | **新增**，header-only（约 1000 行，注释很密）。整个 pacing 引擎：三处 thunk hook、provider 调度器的接入、deadline 修正、统计、以及入口 `Install`。不新增编译单元。 |

### 2.2 修改

| 文件 | 改动 |
|---|---|
| [`OptiScaler/proxies/XeFGUnlock.h`](OptiScaler/proxies/XeFGUnlock.h) | 在解锁的应用流程里调用 `XeFGPacing::Install(base)` —— pacing 在 MFG 补丁**之后**安装，且只在解锁真正执行时安装。失败不致命。 |
| [`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`](OptiScaler/framegen/xefg/XeFG_Dx12.cpp) | `frameRenderTime` 的交接。兜底链改为 `_ftDelta` → `XeFGPacing::RenderTimeMs()` → `state.lastFGFrameTime`，并且实际交出去的值用 `NoteFedFrameTime` 记下来。 |
| [`OptiScaler/menu/menu_common.cpp`](OptiScaler/menu/menu_common.cpp) | XeFG 区的 **Extra Pacing** 勾选框，以及 MFG 倍率下拉框的改版（见 §4）。 |
| [`OptiScaler/Config.h`](OptiScaler/Config.h) / [`OptiScaler/Config.cpp`](OptiScaler/Config.cpp) | 两个新选项：`XeFG\ExtraPacing`（默认 **true**）与 `FTInput` 背后的帧时间来源枚举。 |

---

## 3. Pacing 是怎么工作的

有一个设计取舍值得说明白：**这里没有发明任何 pacing 算法。** 间隔用的是 provider
自己的 —— 每个生成帧只是被转交给 provider 自己的调度器，参数直接用 present 路径
寄存器里**本来就有**的那些。（dashdogy 的补丁做的也是这件事，只不过他 hook 的时候
手里没有 present 的参数，得用一个约 1500 字节的 helper 重新拼出来；而我们是在
present 内部，这些参数是白送的。）

### 3.1 三处 hook

每一处 hook 点都是一个 5 字节 `jmp` thunk，后面跟 11 字节 `int3` 填充：
**16 个字节里除了一个跳转什么都没有**。刚好放得下一个 14 字节的
`jmp qword ptr [rip+0]`，于是 thunk 可以被重定向到地址空间任意位置，
**不需要搬移任何一条指令，也不需要 code cave**。每个 thunk 的 16 个预期字节
都会在写入前比对，写入后回读校验。

| # | Thunk | 目标 | 作用 |
|---|---|---|---|
| 1 | `0x25C0` | `0x21F730` | **Present。** 每个生成帧的呈现都过这个 thunk。detour 检查 `_ReturnAddress()`，只 pace 真正需要的那两个调用点：`0x2202ED`（burst 循环）与 `0x220467`（burst 的最后一帧）。该 thunk 的另外两个调用者（`0x220A52`、`0x220DAD`）原样转发。 |
| 2 | `0x3100` | `0x21EE30` | **provider 的调度器。** 每个被 pace 的帧带着 `arg5` = 它在 burst 里的索引交给它。五个参数全部由 present 自己的参数推出：`arg2` 是 burst（`present arg5 - 0x38`），`arg5` 是循环本来就在走的索引，`arg3` 是 `burst[0xC0] & 1`，`arg4` 由 `0x4DA0` 喂进 ring 得到。 |
| 3 | `0x3430` | `0x224B30` | **deadline。** 每个被调度的帧调一次 —— **我们的和 provider 的都在内** —— 而且它交回一个**可写的**出参指针，所以 §1 里那两处缺陷都在这一个收口处修掉。 |

调度的是 **`1..count-1`** 这些帧；最后那一帧仍由 provider 自己的调用点负责，
并且 detour 会**镜像 provider 限帧块的条件**，保证那一帧上只有一方在等，
而不是两个等待叠成一帧超长。

### 3.2 为什么 burst 的最后一帧要单独一处 hook

因为它根本不从循环里出来。它是在循环之后、provider 提交完整个 burst 之后，
从 `0x220462` 出去的，`arg5` 还是同一个 burst，但 `arg7 = 0` 而不是 `1`。
放着不管，每个 burst 都会以**两个时间戳相同的帧**收尾 —— 4X 下实际交付的图案是
`0, i, 2i, 2i`，而不是 `0, i, 2i, 3i`。

### 3.3 deadline 是怎么修的

在 `0x3430` 那处 hook 上，用 `g_nextDeadlineNs` 顶掉 provider 本来会算出的值：

- burst 的步长是 `median / (count + 1)`，**整个 burst 固定不变**，
  这样它的帧之间不会互相漂移；
- 只要 provider 的 `base` 不是新鲜的（也就是除 index 1 以外的每个 index），
  就把步长**重新锚**到它上面；
- 把 `min()` 削掉的那部分加回来。

### 3.4 那道闸

调度器不是一直开着的，它的入口闸是 context 的两个字节（`0xdbb0` → `0x21ED40`）：

```asm
0x21ED40  cmp   byte ptr [rcx + 0x340], 0
0x21ED47  je    0x21ed4c
0x21ED49  xor   al, al                    ; [ctx+0x340] != 0 -> 假
0x21ED4B  ret
0x21ED4C  movzx eax, byte ptr [rcx + 0x341] ; 否则返回 [ctx+0x341]
```

`0x21EE30` 之后还有 `test al, al; je`，也就是说第二个字节不置位它也直接退。
而 `0x340` **正是 provider 那个限帧块所依据的同一个字节** —— 所以调度器和限帧块
在构造上就是互斥的：限帧块管着 pacing 的时候，调度器是关的，这时调它就是个静默空操作。

`SchedulerUsable()` 把这个条件抄了下来。只有它真的会运行时，才会往里交东西；
而被拒的次数是**单独计数**的 —— 因为**从外面看，一次被拒的调用和一次完成的调用
完全一样**，把这两者混为一谈正是一个早期构建"悄悄什么都没干"的原因。

### 3.5 兜底，以及被删掉的 fence 等待

如果调度器的 thunk hook 不上（字节对不上、hook 失败），pacing 会退回一个
**墙钟**，用最近 15 帧测真实帧周期。它是兜底，不是设计。

**"等 provider 自己的 D3D12 fence"实现过，然后被删了。** provider 的交换链 context
里确实有 fence（`ctx+0x328`，事件在 `ctx+0x330`），它也懂得怎么正确地等。
从这里做同样的事栽在一个很朴素的原因上：`WaitForSingleObject` 会把一个很短的
超时**向上取整到系统时钟节拍**，于是每一次超时的等待都**多等约 7 ms**；超过一半的
等待都是超时结束的，结果实测的生成帧间隔比目标**高出 30–40%**。这条路就只剩墙钟了。

### 3.6 交给 provider 的帧时间

为了打断 §1 末尾那个自锁，交给 provider 的值不再等于实测的 present-to-present 差值。
当前 burst 已经造成的阻塞（`g_burstBlockQpc`）会从周期里减掉，把**减完的那个**交出去，
走 `RenderTimeMs()`。`XeFG_Dx12.cpp` 用 `NoteFedFrameTime` 把它记下来，
pacing 报告就把它印在它由之而来的那个周期旁边 —— **这个并排就是这次改动的全部意义**：
它把"估计值到底有没有真的不同于实测周期"变成一个**数**，而不是一种感觉。

旧行为保留成一个运行期 A/B 开关，不用重编译就能对照：

| `FTInput` | 行为 |
|---|---|
| `Input`（**默认**） | 新的渲染时间估计：`_ftDelta` → `RenderTimeMs()` → 兜底 |
| `Opti` | 旧行为 —— 直接交 `state.lastFGFrameTime` |
| `Zero` | 交 `0.0` |

倍率上限不是一个我们自己的常数。`XeFGUnlock.h` 把配置的最大值写进 provider 自己报告的
最大值（`XeFG\MaxInterpolatedFrames`，默认 **31**），菜单再跟着报告值走 —— 所以上限**就是**
这个设置，菜单开箱即可选到 32X。XeFG 这条路径上已经没有任何地方再夹它。

这个设置不只是菜单边界。`XeFG_Dx12` 会在 swapchain 初始化时把同一个报告值读回去填进
`xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames`，所以把上限报高，等于每次启动
都向 provider 声明这个数字。6X 以上**未经验证**：如果 provider 按这个数字预分配资源，
症状会是初始化失败或显存耗尽，退回去的办法是把 `XeFG\MaxInterpolatedFrames` 设为 5。

---

## 4. 配置

| 位置 | 选项 | 默认 | 说明 |
|---|---|---|---|
| `OptiScaler.ini`，`[XeFG]` | `ExtraPacing` | `true` | 总开关。在安装时读取。 |
| 游戏内菜单，XeFG 区 | **Extra Pacing** | — | 同一个开关。**必须重启游戏** —— hook 在 provider 初始化时改写 present thunk，运行中不能切。 |
| `OptiScaler.ini`，`[XeFG]` | `InterpolationCount` | `1` | 插值次数，所以**倍率是这个值加一**。读入时校验为 `1..31`，即 **2X..32X**。 |
| `OptiScaler.ini`，`[XeFG]` | `MaxInterpolatedFrames` | `31` | 报给 provider 的上限，因此也是菜单的上限（倍率 = 这个值加一）。见 §3.6。 |
| `OptiScaler.ini` | `FTInput` | `Input` | 见 §3.6 的表。 |

### 倍率菜单

MFG 下拉框里按名字列出 **2X / 3X / 4X**，然后是一个 **Custom…**。2X–4X 是自己就能
撑住的倍率；5X 及以上走自定义槽位，那里可以随便填整数，只会被夹到 provider 报告的上限，
没有别的夹取。

**超过 4X 时菜单会显示一条警告，那不是装饰。** 那些倍率下 burst 的呈现速度超过了
显示器刷新率，而 provider 内部没有任何手段能把它拉回来 —— 必须从**外面**给呈现速率
加一个上限，也就是**开 VSync** 或设一个帧率上限。没有的话，多出来的帧会撕裂和抖动。

选中自定义槽位时会以 **INFO** 级别记一行日志（`MFG menu: Custom selected, asking for …`），
而按名字选的那三个是 debug 级别。这是故意的：自定义槽位是唯一一个**值有可能被下游拒绝**
的控件，而这一行就是用来把"菜单根本没提请求"和"提了、但被什么夹回去了"分开的。

---

## 5. 限制

- **依赖 MFG 解锁。** pacing 是在解锁的应用流程里安装的，而且只有 2X 以上才有意义，
  而 2X 以上正是解锁才变得可达的。原版 OptiScaler 根本走不到这里。
- **绑定在一个 provider 构建上。** 所有偏移都是从 `libxess_fg.dll` **1.3.1.78**
  （构建标识 `0x69CB0F4D`）推出来的。换个 provider 构建就是另一套偏移；thunk 字节
  比对会失败，pacing 会**拒绝安装**而不是把东西改坏 —— 但那些偏移本身需要重新推导。
- **4X 以上需要外部限速**（§4）。
- **旧的 fence 等待是故意删掉的**（§3.5）。要恢复它意味着重新测量，而不是重新打开开关。
- **这里不覆盖 provider 自己的硬上界**：当呈现速率超过刷新率时，
  burst 内部的调度再怎么修也翻不过去。

---

## 6. 可观测性与维护

pacing 干的所有事，都通过**每 5 秒一条 INFO 日志**报告：

```
XeFG pacing: {mult}X, real frame {:.2f} ms ({:.1f} fps), target {:.2f} ms/frame;
gap {:.2f} avg / {:.2f} min / {:.2f} max ms over {} frames;
scheduler {} calls, {} refused, {:.2f} ms avg inside;
deadlines {} calls, {} rebased, {} clamped; render-est {:.2f} ms, fed {:.2f} ms
```

怎么读：

| 字段 | 含义 |
|---|---|
| `real frame` / `target` | 实测周期，以及 pacing 瞄准的周期 |
| `gap … avg / min / max` | 相邻**被 pace 的**帧之间的间隔 —— 这是"帧确实均匀出来"唯一**直接**的证据。这一行里其它内容都只是关于意图的声明。 |
| `scheduler … calls / refused` | 被拒的调用和完成的调用从外面看一模一样；这一栏就是把它们分开的东西 |
| `deadlines … calls / rebased / clamped` | provider 自己的算术被纠正了多少次 |
| `render-est` 对 `real frame` | **决定性的那一对。** 如果 `render-est` 明显**小于** `real frame`，说明 §1 那个自锁是真的、而且已经被打断。如果两者差不多，说明实测周期是被 GPU 或显示器定的，估计值根本没有限制住任何东西。 |
| `fed` | 实际交给 provider 的值。默认 `FTInput` 下它应当跟着 `render-est` 走；如果它跟着 `real frame` 走，说明 `RenderTimeMs()` 返回了 0、落到兜底分支了。 |

### ⚠️ 这里的格式串写错，会把 pacing 静默打死

这是唯一一个**已经真的毁掉过一整轮实测**的维护坑，值得完整写下来。

那条 `LOG_INFO` 跑在 **present 线程**上，外面**没有 `try`/`catch`**。`fmt` 对
**多余的实参是静默忽略**的，所以多给一个无害 —— 但**类型说明符对错了参数是致命的**：
它会在运行时抛 `fmt::format_error`，而异常会**直接穿过 hook 往外飞**。
又因为统计是在报告**之后**才复位的，第一次抛出就把 deadline 永远留在过去，
于是**之后每一次调用都抛**，pacing 在这场游戏剩下的时间里全废。

之所以值得单列一节，是因为它**完全静默**：本该报告问题的那一行，正是抛出的那一行，
所以日志只是**不再提 pacing 了** —— 这极容易被误读成"pacing 没有效果"。
有一整轮就是这么测完、拿到零可用数据的。

**改那条报告行之前，先数占位符。** 仓库里就有现成脚本：

| 脚本 | 用途 |
|---|---|
| `_analysis/check_reportstats_args.py` | 数格式占位符与实参是否逐位对得上，并按编译器实际产生的配对打印出来，会给"给计数器套了浮点说明符"报警 |
| `_analysis/probe_strings.py` | 按日志字符串判定磁盘上某份产物是哪一轮 |
| `_analysis/verify_pacing_build.py` | 断言这次改动该有的字符串都在、该删掉的都×0 |

### 怎么区分两份产物

**可靠的标记是字符串，不是文件大小**：只改了那条报告行的两份构建，产物**尺寸完全相同**，
因为多几个少几个字符都落在节对齐之内。

---

## 7. 溯源

这套 pacing 是在一台 RTX 2060 上跑 **`赛博朋克 2077`**、历经**九轮实测**做出来的：
每一轮的预测都要先跟上一轮的日志对上，才允许动下一处。其中若干早期结论
**被实测推翻并撤回** —— 包括 fence 等待（§3.5），以及"`0x21EE30` 是 fence 等待"
这个前提本身（它其实是调度器，而正是这一点让整个思路成立）。

逐轮记录（含被推翻的结论和仍未坐实的疑点）在
[`optiscaler xemfg version/08-PACING.md`](../XeSSMFG-Universal-v0.2.0.1/optiscaler%20xemfg%20version/08-PACING.md)。
