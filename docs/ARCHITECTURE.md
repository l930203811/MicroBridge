# 微音桥 MicroBridge — 架构与模块设计

对应 `docs/PRD.md`。本文描述**设计约束与模块边界**，实现以 `Source/` 为准。

---

## 1. 分层与线程模型

```
┌───────────────────────── UI 线程 ─────────────────────────┐
│ 矩阵 / 通道条 / 预设 / 设备选择                            │
│   └─ 修改 RouteGraph（建边、删边、增益、静音、独奏）       │
│        └─ compile() → 新的 shared_ptr<const ProcessingPlan>│
│             └─ atomic store(release)  ───────────────┐     │
└──────────────────────────────────────────────────────┼─────┘
                                                        ▼
┌──────────────────────── 实时音频线程 ─────────────────────┐
│ audioDeviceIOCallbackWithContext                          │
│   └─ atomic load(acquire) 取一份 plan，整个 block 持有     │
│        └─ 按 channelOrder 顺序做乘加，写入各设备缓冲       │
└───────────────────────────────────────────────────────────┘
```

**核心原则**：图只在 UI 线程存在；实时线程看到的永远是一份编译好的、不可变的、
按拓扑序排平的边表。音频线程不遍历图、不判断环、不查容器、不分配内存。

这个「不可变快照原子换出」模式在 S-Rack 的分离结果渲染中已验证过，
此处直接沿用。

## 2. 数据模型（`Source/RouteGraph.h`，纯 C++20，无 JUCE 依赖）

```cpp
enum class EdgeKind { ChannelToChannel, ChannelToDevice };

struct RouteEdge { int src, dst; EdgeKind kind; float gain; };
//   src 恒为通道索引
//   kind == ChannelToChannel → dst 为通道索引
//   kind == ChannelToDevice  → dst 为设备索引

struct PlanEdge { int src, dst; float gain; };  // gain 已乘上源通道推子

struct ProcessingPlan {
    std::vector<int>       channelOrder;   // 拓扑序：src 先于 dst
    std::vector<PlanEdge>  channelSends;   // 按 src 的拓扑序排好
    std::vector<PlanEdge>  deviceSends;    // src = 通道, dst = 设备
    std::vector<float>     faders;
    std::vector<uint8_t>   active;
    int channelCount, deviceCount;
};
```

`RouteGraph` 持有 `std::atomic<std::shared_ptr<const ProcessingPlan>> plan_`；
`plan()` 用 `memory_order_acquire` 读一次，`processBlock` 全程使用这一份。

### 语义

| 操作 | 编译结果 |
|---|---|
| 推子 `fader[c]` | 通道 c 所有出边的 gain × fader[c] |
| 静音 c | `active[c] = 0`，c 的全部出边不进 plan |
| 存在任一独奏 | 仅 `solo[c] != 0` 的通道 `active = 1` |
| 零 / 负增益边 | 直接丢弃（稀疏化） |
| 目标通道被静音 | 该条 `C→C` 边不进 plan |

## 3. 环检测与拒绝

`A → B → A` 在实时音频里是无限递归。MVP 采取**建图时拒绝**：

- `tryAddEdge()` 先做参数校验（越界、自环、负增益、非有限值）；
- 把候选边放进 `trial` 副本，对 `C→C` 子图跑 Kahn 拓扑排序；
- 若有节点无法出队 → 判为环，返回 `false` 并写明原因，**原图不变**；
- 通过后才提交并重新编译。

拒绝是唯一在 MVP 保证低延迟的做法。备选项（M3 再评估）：
允许环但每条回边插入 1 个 block 延迟（DAW 的 PDC 思路，会破坏低延迟指标）；
「不限但靠限幅」会自激，已否决。

## 4. `processBlock` 伪代码（M2 目标形态）

```cpp
auto plan = graph.plan();                     // acquire，一次
clear (allDeviceBuffers, numSamples);
fill  (channelBuffer[inputChannel], inputDeviceData);

for (int c : plan->channelOrder)              // 拓扑序，无分支遍历
{
    for (auto& e : plan->channelSends for src == c)
        addWithGain (channelBuffer[e.dst], channelBuffer[c], e.gain, numSamples);

    for (auto& e : plan->deviceSends for src == c)
        addWithGain (deviceBuffer[e.dst], channelBuffer[c], e.gain, numSamples);
}
```

复杂度 O(非零边数)，与 `N²` 无关。实际实现会把「每个 src 的边范围」预编译成
`Span` 索引，避免循环里做条件筛选。

### 实时线程禁令

禁止：`new` / `malloc`、`std::vector` 扩容、`std::string`、锁 / 互斥、
`std::function`、文件 I/O、日志、UI 调用、异常、浮点除零检查外的分支遍历。
允许：定长栈数组、预分配缓冲、`FloatVectorOperations`、原子读。

增益平滑（约 40 ms 线性）在控制线程把目标值算好，实时线程只对系数做插值；
不要因为一条推子变化就重算整个矩阵。

## 5. 多设备时钟（M2，最高风险）

Windows 没有 macOS 那样的原生聚合设备：每个 WASAPI / ASIO 设备有独立晶振和
独立回调线程，ppm 级漂移，长时间运行必然欠载爆音；缓冲尺寸还可能不同。

方案：

1. **单一内部主时钟**（以主输出设备回调为节拍）；
2. 每个从设备一个**自适应环形缓冲**，按观测到的读写差微调水位；
3. 设备间做**轻量 ASRC**（JUCE 的 resampler 基础 + 自研漂移估计），
   不引入重采样库的额外依赖；
4. 所有设备统一到同一内部 block size。

验收硬指标：双设备连续 30 分钟无漂移、无爆音。MVP 上限 2 个设备。

## 6. VST3 插件链预留点（当前不实现）

用户明确：本版**不做插件托管**，因为会显著增大体积与依赖，定位是轻量化机架。

预留方式：`ProcessingPlan` 的节点结构将来可扩展为
`{ channel, InsertSlot, ... }`，`InsertSlot` 在 M1/M2 恒为空实现（零开销、不链接
VST3 SDK）。若 M3 决定加入，只需在 `compile()` 中把插件节点编进拓扑序，
`processBlock` 的形态不变；不需要重画整张路由图。

**注意**：「不做混音编辑」≠「不能挂插件」。前者指不要时间线，后者是实时通道插入。
本版是出于**体积约束**主动暂缓，不是架构上排斥。

## 7. 模块与目录

| 文件 | 职责 | JUCE 依赖 |
|---|---|---|
| `Source/RouteGraph.h` | 图模型、环检测、plan 编译、原子换出 | 无 |
| `Source/AudioEngine.h` | 设备 I/O、回调、M2 的多设备缓冲与 ASRC | 有 |
| `Source/MainComponent.h` | 矩阵 / 通道条 / 预设 UI | 有 |
| `Source/Main.cpp` | 应用入口 | 有 |
| `Tests/RouteGraphTest.cpp` | 纯 C++ 单元测试 | 无 |

把路由内核做成无 JUCE 依赖的纯头文件，是为了让「环检测、快照换出、静音独奏」
这些最容易出错的逻辑能秒级编译测试，不必等 JUCE 全量重编。

## 8. 测试策略

- **单元**（`RouteGraphTest`）：无环链路拓扑序、成环拒绝且图不被污染、自环 /
  越界 / 负增益 / NaN、重复边更新增益、快照不可变、静音断边、独奏筛选、
  零增益稀疏化、单通道多设备、删边重编、resize 清空。当前 11 组。
- **M1 基线**：单设备直通，64–128 samples 试听 30 分钟，记录 CPU 与环回延迟。
- **M2 长跑**：双设备 30 分钟，检查欠载计数与漂移曲线。
- **实时性**：代码审查 + 长跑期间监控分配计数（后续可接入计数钩子）。

## 9. 已知风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 多设备时钟漂移 | 长时间后爆音 | 主时钟 + 自适应环形缓冲 + ASRC，30 分钟长跑验收 |
| JUCE 8 Direct2D 渲染 | 界面闪烁 / 抖动 | 基线用软件渲染，电平表限帧 ~30 fps |
| `AudioDeviceManager` 假设单输入单输出 | 多设备并发要自研 | 只借用其设备枚举与基础 I/O，调度层完全自研 |
| 命名撞车 | 商标风险 | 开源名可用；注册前做第 9/42 类近似查询 |
| 虚拟设备依赖外部安装 | 用户上手门槛 | README 写明 VB-CABLE / Voicemeeter，M3 再评估随包驱动 |
