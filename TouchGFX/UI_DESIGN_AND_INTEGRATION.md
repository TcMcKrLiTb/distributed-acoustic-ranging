# TouchGFX 页面设计与后续接入说明

版本：V1.0  
核对日期：2026-10-25  
适用工程：DistributedAcousticRanging，TouchGFX 4.26.1，480 × 272 横屏，RGB565  
文档范围：记录当前页面设计、控件用途和后续接入方式。文中的“建议”“后续”均表示尚未实现的行为。

## 1. 当前检查结论

本次对 Designer 工程、文本数据库、生成的 ViewBase 和手写界面代码进行了静态核对，当前页面结构可以作为后续功能开发的基础。

| 检查项 | 当前结果 |
| --- | --- |
| 页面 | 5 页：`screen`、`settings`、`calibration`、`waveform`、`diagnostics` |
| 控件 | 115 个，其中 113 个可见；主页原有 `button1`、`titleText` 隐藏 |
| 导航 | 25 个导航按钮；20 条跨页跳转的目标正确；当前页没有跳转到自身的事件 |
| 布局 | 可见控件没有同页重名、越界或明显的非背景矩形重叠 |
| 动态文本 | 38 个 Wildcard，均启用缓冲区，容量与生成的头文件一致 |
| 生成结果 | 可见控件的位置、尺寸与生成代码一致；142 条文本的字体和对齐与生成数据库一致 |
| 诊断页 | 已改为 `diagnosticsSyncText`；四行数值均右对齐；没有 LED 测试按钮 |

当前没有必须返工的 Designer 布局问题。`sampleRateText` 仍为左对齐，如果希望采样率靠右边界排列，可在 Designer 中改为 Right；这属于样式选择，不影响功能。其他页面部分文字仍使用默认黑色，后续可按第 3 节统一配色，无需因此调整布局。

本次没有运行新的模拟器测试或实物测试，上述结论针对配置与代码的一致性。

### 1.1 已实现与待实现的边界

| 内容 | 当前状态 |
| --- | --- |
| 五页切换、底部当前页高亮 | Designer 与生成代码已实现 |
| 主页原有 5 项动态信息 | 手写 `screenView::handleTickEvent()` 已更新板卡状态、距离、声源侧、测量状态、统计信息 |
| 主页新增的 8 项动态信息 | 有控件和默认文字，尚未接入实时数据 |
| 设置页温度、模式、单位、应用 | 有控件和初始外观，尚未绑定业务事件 |
| 标定流程与保存 | 有控件，尚未实现流程和保存 |
| 波形数据、时基、幅度、冻结 | 有图表和按钮，尚未接入数据或控制 |
| 诊断页同步／音频／结果切换 | 有三个按钮和共用四行文字，尚未绑定切换事件 |
| 自动温度不可用状态 | 当前仅以灰色和“无传感器”提示表达；尚未通过 `setTouchable(false)` 禁用触摸 |
| SIM 标记 | 五页均为可见的静态文字；后续需要根据真实运行模式控制显隐 |

主页在 `SIMULATOR` 分支中会显示固定示例数值；这些数值属于预览。实物分支读取 `Ranging_GetStatus()`。新增字段目前仍保留 Designer 默认文字。

## 2. 五页划分的设计思路

测量结果、参数修改、标定步骤和调试数据需要不同的阅读方式。五页分别承担一个主要任务，避免在 480 × 272 屏幕内堆积所有信息。

| 页面显示名称 | Screen 名称 | 主要任务 | 设计重点 |
| --- | --- | --- | --- |
| 测距 | `screen` | 快速查看距离和本次测量是否可信 | 距离最大；状态、质量、环境参数分层显示 |
| 设置 | `settings` | 修改温度来源、温度、识别模式和显示单位 | 输入与应用分开，预留双板确认状态 |
| 标定 | `calibration` | 输入已知间距、引导采样、检查并应用补偿 | 步骤文字优先，候选结果与已应用状态分开 |
| 波形 | `waveform` | 查看本板 PCM 的时间和幅度特征 | 图表固定区域，保留时基、幅度、冻结控制 |
| 诊断 | `diagnostics` | 定位同步、音频采集和测距拒绝问题 | 三个分组复用四行数据，避免拥挤 |

共同区域如下：

| 区域 | 坐标范围 | 用途 |
| --- | --- | --- |
| 顶部栏 | `y = 0..27` | 页面标题或板卡状态、顶部同步信息、SIM 标记 |
| 内容区 | `y = 28..227` | 每页主要内容 |
| 底部导航 | `y = 228..271` | 五个等宽 96 px 的入口，按钮高度 44 px |

页面切换沿用当前 Designer 中的东西方向 Wipe 动画。切换页面只改变显示内容，后续不应因此停止测距、重新初始化音频或清空已应用配置。

## 3. 公共控件与视觉约定

### 3.1 底部导航与顶部栏

| 控件 | 含义与后续操作 |
| --- | --- |
| `navRangeButton` | 打开 `screen`，已绑定导航 |
| `navSettingsButton` | 打开 `settings`，已绑定导航 |
| `navCalibrationButton` | 打开 `calibration`，已绑定导航 |
| `navWaveformButton` | 打开 `waveform`，已绑定导航 |
| `navDiagnosticsButton` | 打开 `diagnostics`，已绑定导航 |
| `backgroundBox`、`headerBox` | 页面底色与顶部白色区域，通常不参与业务更新 |
| `pageTitleText` | 四个新增页面的固定标题，普通 `TextArea` |
| `settingsSyncText`、`calibrationSyncText`、`waveformSyncText`、`diagnosticsSyncText` | 各自页面的板卡／同步摘要；后续从同一份系统状态生成，均为 64 字符缓冲区 |
| `demoText` | SIM 提示；预览数据时显示，真实数据模式隐藏；数据失效应另显示原因，不能自动切换成模拟成功值 |

同名控件位于不同 Screen 的不同 View 类内，例如各页的 `demoText`，可以分别操作。修改一个页面的控件不会自动修改另外四页。

### 3.2 字体、颜色与状态表达

| 项目 | 约定 |
| --- | --- |
| `Large` | Verdana 40，用于主页距离；字库以数字、大写英文及必要符号为主 |
| `UI16` | HYJiangNan 16，用于标题、重点数值、步骤、导航等 |
| `UI14` | HYJiangNan 14，用于说明、状态、诊断行和一般按钮 |
| 主要文字 | `#182537`，用于距离、重点状态、数值 |
| 次要文字 | `#4E6075`，用于标签、解释、同步摘要、统计和提示 |
| 选中按钮 | 深蓝 `#164D7A`，白色文字 |
| SIM 文字 | `#9A6200` |
| 主页 `statusBox` | 保留当前 `#F2F2F2`，用于承托底部状态说明 |

这些是统一样式的目标；当前主页和诊断页已经采用主要／次要文字配色，其他页面还保留部分黑色文字。

按钮的“按下”和“选中”是两个不同状态：当前控件是 `ClickButtonTrigger`，按下外观会在松手后复原。温度来源、模式、单位和诊断分组的长期选中状态，需要由代码保存并重新设置释放态颜色。灰色外观也不等于已禁用，必须同时设置触摸能力并在业务入口检查条件。

## 4. 测距页 `screen`

### 4.1 信息组织

左侧显示距离与声源侧；右侧放质量依据；下方显示温度、声速、标定和统计。用户先看结果，再通过状态和质量判断能否使用本次结果。

| 控件 | 缓冲区容量 | 用途及后续更新规则 |
| --- | --- | --- |
| `boardStatusText` | 64 | 板卡 A/B、同步状态；当前已由 `master`、`locked` 更新 |
| `distanceText` | 32 | 核心距离；当前读取 `distance_mm` 并显示米。后续根据显示单位格式化，内部保持统一单位 |
| `directionText` | 32 | 声源位于 A/B 侧；当前读取 `source_a`，与距离结果使用同一事件 |
| `rangeStateText` | 64 | 等待同步、等待声音、传输、计算、等待显示及事件号；当前已有更新 |
| `statisticsText` | 64 | 当前已有 Q、接受数、拒绝数；未来要明确统计范围和重置条件 |
| `syncOffsetText` | 64 | 板间同步偏差，单位 us 或 μs；需要同步模块提供，不能使用声学时差替代 |
| `eventInfoText` | 64 | 本次事件号和音频／击掌类型；事件号已有来源，类型尚需后端提供 |
| `correlationText` | 64 | 相关系数等质量指标；需要后端明确指标定义和量纲 |
| `peakMarginText` | 64 | 主峰与竞争峰的区分程度；采用后端统一定义，不能自行填入估计数值 |
| `temperatureText` | 64 | 当前实际应用的温度，而非设置页尚未应用的草稿 |
| `soundSpeedText` | 64 | 测距正在使用的声速；与双板配置版本一致 |
| `calibrationStatusText` | 64 | 标定未应用／已应用等状态；使用实际生效结果 |
| `measurementStatusText` | 64 | 一句话解释：等待数据、结果有效、拒绝原因、结果陈旧等 |

`RangingStatus::quality_permille` 当前用于统计行中的 Q 百分比，不能未经定义就同时当作相关系数和峰差。`delay_ns` 是测距时差，也不是同步偏差。

主页现有代码在 `valid == false` 时显示占位距离。后续如果决定保留“上次有效结果”，应同时显示结果年龄或“上次”提示，避免用户把旧结果当作刚测出的值。距离、方向、质量和事件号应来自同一个结果快照，避免混用不同事件。

### 4.2 隐藏的旧控件

`button1` 和 `titleText` 是兼容原页面留下的隐藏控件。`button1Clicked()` 仍经 Presenter／Model 调用 LED 翻转。当前不需要删除；将来清理时必须同时处理 Designer Interaction、View override 和旧调用链，不能只删生成类中的控件。

## 5. 设置页 `settings`

设置页应维护“编辑中的配置”和“正在使用的配置”两份状态。点击加减、模式或单位时先修改草稿；点击“应用到双板”后才发出应用请求。

| 控件 | 当前初始内容／外观 | 后续操作 |
| --- | --- | --- |
| `tempSourceLabel` | 温度来源 | 固定标签 |
| `tempManualButton` | 手动，选中 | 选择手动输入，允许温度加减 |
| `tempAutoButton` | 自动，灰色 | 没有传感器时禁用；有有效温度数据后再允许选择 |
| `tempSourceHintText` | 自动：无传感器 | 普通静态文字；未来可切换具名 Text ID；需要任意动态内容时再在 Designer 加 Wildcard |
| `temperatureLabel`、`temperatureUnitText` | 温度值、℃ | 固定标签和单位 |
| `temperatureMinusButton`、`temperaturePlusButton` | −、+ | 调整草稿温度，建议先采用 0.5 ℃ 步进；范围和步进集中定义，不从显示字符串反向读取 |
| `temperatureValueText` | 20.0，容量 32 | 显示草稿温度；推荐内部用 0.1 ℃ 的整数单位 |
| `soundSpeedText` | 声速 343.42 m/s，容量 64 | 温度改变后显示待应用声速；主页仍显示已应用声速 |
| `measurementModeLabel` | 测量模式 | 固定标签 |
| `modeAutoButton`、`modeAudioButton`、`modeClapButton` | 自动选中 | 三选一，仅选择已经实现的识别模式；未实现时应明确提示或禁用 |
| `unitLabel` | 显示单位 | 固定标签 |
| `unitMeterButton`、`unitCmButton` | 米选中 | 二选一，只改变格式化显示；不改变内部距离值 |
| `applySettingsButton` | 应用到双板 | 校验草稿、提交请求、等待确认；处理中防止重复提交 |
| `configSyncText` | 界面预览，容量 64 | 显示未应用、应用中、双板已确认、超时或失败 |

当前后端默认声速是 `343420 mm/s`，对应 `c = 331.3 + 0.606 × T`、`T = 20 ℃`。设置页已有温度控件不代表声速已经能在运行时修改：`ranging.c` 目前使用静态 `const RangeConfig`，后续需要设计配置命令和安全的应用时机。

推荐的应用过程：

1. 本地编辑草稿并即时重绘温度、声速和选中状态。
2. 校验范围、传感器可用性、模式支持情况与链路条件。
3. 提交带请求号／配置版本的命令；由后端协调双板在一致的事件边界应用。
4. 双板确认后更新“已应用配置”，再让主页显示新的温度和声速。
5. 超时或失败时保留草稿并明确提示；仅本地修改成功时不能显示“双板已确认”。

离线预览可以完成第 1 步，并模拟第 2～5 步的状态变化；预览成功必须保留 SIM 标识。显示单位可以作为本地偏好，也可以随配置同步，后续选择一种明确规则。

## 6. 标定页 `calibration`

页面把“采样得到的候选偏置”和“实际应用的偏置”分开，避免刚得到一组数据就影响正常测距。用户先输入准确的声孔间距，按引导完成采样，检查结果，再保存。

| 控件 | 缓冲区容量 | 用途及后续操作 |
| --- | --- | --- |
| `referenceDistanceLabel`、`referenceUnitText` | 静态 | 声孔间距、m；已知距离指两个声学参考点的间距 |
| `referenceMinusButton`、`referencePlusButton` | — | 修改已知距离；建议内部用 mm；采样过程中禁止改变参考距离 |
| `referenceDistanceText` | 32 | 默认 0.500 m；后续显示草稿参考距离 |
| `calibrationStatusText` | 64 | 显示未应用、候选可用、已应用等状态 |
| `sideAProgressText`、`sideBProgressText` | 各 32 | 当前为 A/B 侧有效 0/5；只统计通过质量检查的样本，不代表需要增加进度条 |
| `calibrationStepText` | 96 | 核心步骤提示：准备、在哪一侧发声、移动声源、等待确认、失败原因 |
| `calibrationBiasText` | 64 | 显示候选偏置；界面用 us，内部按后端定义保存 ns |
| `calibrationQualityText` | 64 | 显示稳定性、对称性、样本离散程度等检查结论 |
| `startCalibrationButton` | — | 开始采样；如果方案需要“确认已移到 B 侧”，可在相应阶段复用此按钮并更换文字 |
| `cancelCalibrationButton` | — | 中止当前候选流程，恢复编辑；保留原来已应用的标定 |
| `saveCalibrationButton` | — | 仅候选结果通过检查后允许；提交双板应用请求并等待确认 |

A/B 两组文字为双侧采样方案预留入口，`0/5` 是当前占位显示，不是任务书强制规定的算法或采样次数。最终方案和目标有效样本数应由标定算法统一给出。

如果采用双侧方案，建议状态流为：

```text
编辑间距 → A侧采样 → 提示移动到B侧 → 用户确认 → B侧采样
         → 计算候选偏置 → 质量检查 → 等待用户保存 → 双板确认应用
```

开始采样、采样失败、取消、完成和保存确认，都由状态机驱动文字与按钮可用性。切换页面后不能仅因 View 重建而丢失标定状态；流程放在后端或持久的 Model 状态中。

当前测距核心的偏置定义是 B 链路相对 A 的延迟，计算顺序为 `dt = tB - tA - bias`，再取绝对值计算距离。显示和保存时应保留偏置符号。标定的具体估计方法参见已有 Ranging 说明，不在 UI 中重新实现一套算法。

“保存标定”未来还需要明确是仅本次运行生效，还是写入非易失存储。只有确认完成对应的持久化流程，才能向用户承诺重启后仍保留。

## 7. 波形页 `waveform`

### 7.1 当前图表配置

| 控件／配置 | 当前值与后续用途 |
| --- | --- |
| `waveformInfoText` | 容量 64；默认“本板PCM，等待数据”；后续显示声道、公共窗口标识、冻结状态等 |
| `sampleRateText` | 容量 64；默认 `FS -- Hz`；显示实际采样率 |
| `waveformGraph` | Designer 类型 StaticGraph，生成类型 `touchgfx::Graph<256>`，容量最多 256 点 |
| 图表区域 | `(12, 62, 456, 112)`，单条深蓝折线 |
| X 轴 | 当前范围 0..4，建议统一解释为 ms；主刻度间隔 1，即初始 1 ms/格 |
| X 精度 | `setScaleX(1000)`，每 0.001 ms 一个内部单位，相当于 1 us |
| Y 轴 | 当前范围 −1200..1200，主刻度间隔 600；后续显示 PCM 计数值，尚未定义为电压 |
| `timebaseButton` | 静态文字“时基 1 ms/格”；后续循环选择窗口跨度并同步更新刻度 |
| `amplitudeButton` | 静态文字“幅度 自动”；后续选择固定范围或自动范围 |
| `freezeButton` | 静态文字“冻结”；后续切换显示冻结／继续 |

当前生成代码没有注入随机数据点。Designer 画布中的示意曲线不代表已接入 PCM。

### 7.2 后续数据更新规则

1. 后端提供稳定的一帧快照，包括 PCM、采样率、样本数、统一的时间起点和帧序号。
2. UI 取得快照后，在 GUI 线程中清空旧图并填入当前窗口的数据；每帧不超过 256 点。
3. X 必须是相对窗口起点的时间。不能直接把样本序号 0、1、2 当作当前以 ms 表示的横坐标。
4. 48 kHz 下 4 ms 跨度对应 192 个采样间隔；如含两个端点为 193 点。直接把 256 个连续样本塞进 0..4 ms 会裁掉后段或压错时间轴。
5. 较长窗口需要选点或保峰抽取；以显示为目的处理数据，不得修改测距算法使用的原始 PCM。
6. “冻结”只暂停替换显示快照，不停止音频 DMA、同步、采样或正常测距。继续时切回最新完整帧。

同步波形的关键是两板采用相同的公共时间窗口和配置版本。分别按各板本地声音峰值对齐，会掩盖传播时差，无法用来验证相位关系。后续还需后端协调公共窗口和刷新，不是加一个图表就已经完成同步显示。

当前 Graph 只保存一组 X/Y 数据。在同一个 Graph 上再添加一个 `GraphElementLine`，仍会绘制同一组数据；若未来要求单屏同时显示两路独立波形，需要另行设计独立数据集或第二个图表。现在的页面定位是各板显示自己的 PCM。

### 7.3 与当前配置匹配的加点示例

以下是未来 `waveformView` 内可使用的成员函数示例；需要在对应头文件中声明。参数是已经复制稳定的单声道快照，不是正在被 DMA 写入的数组。本示例仅处理当前 0..4 ms 初始窗口，不包含抽取、公共窗口获取和冻结状态机。

```cpp
#include <cstdint>

void waveformView::showInitialWindow(const int16_t* pcm,
                                     uint16_t count,
                                     uint32_t sampleRate)
{
    waveformGraph.clear();
    if (!pcm || sampleRate == 0)
    {
        return;
    }

    waveformGraph.setGraphRangeX(0, 4); // 普通API的单位是ms
    for (uint16_t i = 0; i < count && i < 256; ++i)
    {
        const uint64_t timeUs =
            (static_cast<uint64_t>(i) * 1000000ULL + sampleRate / 2U)
            / sampleRate;
        if (timeUs > 4000U)
        {
            break;
        }
        // Scaled API直接收内部坐标；此处X为us，Y仍为PCM计数。
        waveformGraph.addDataPointScaled(static_cast<int>(timeUs), pcm[i]);
    }
    waveformGraph.invalidate();
}
```

普通 `addDataPoint()` 会按 Scale 再缩放，`addDataPointScaled()` 则直接接收已经缩放的值；两者不要混用单位。改变时基时，要同时修改图表范围、X 网格间隔、X 标签间隔和按钮文字。自动幅度必须避免零宽度范围，并对静音和单点尖峰作适当处理。

## 8. 诊断页 `diagnostics`

### 8.1 三个分组共用四行

| 控件 | 后续操作 |
| --- | --- |
| `diagSyncButton` | 选择同步分组，当前初始为选中外观 |
| `diagAudioButton` | 选择音频分组 |
| `diagResultsButton` | 选择结果分组 |
| `diagRow1LabelText`、`diagRow1ValueText` | 第 1 行标签与数值，初始为角色／链路 |
| `diagRow2LabelText`、`diagRow2ValueText` | 第 2 行标签与数值，初始为同步偏差 |
| `diagRow3LabelText`、`diagRow3ValueText` | 第 3 行标签与数值，初始为同步状态 |
| `diagRow4LabelText`、`diagRow4ValueText` | 第 4 行标签与数值，初始为 PPS 异常计数 |
| `diagnosticHintText` | 容量 64；显示预览、数据年龄、未接入或简短解释 |

以上八个行文本均为 64 字符缓冲区。标签左对齐、数值右对齐；切换分组时同时更新标签与数值。

初始显示同步分组：角色／链路、同步偏差、同步状态、PPS 异常计数。后续分组内容建议如下，只有后端确实提供了数据才显示实际值：

| 行 | 同步分组 | 音频分组 | 结果分组 |
| --- | --- | --- | --- |
| 1 | 板卡角色／链路 | 采样率／声道 | 接受／拒绝计数 |
| 2 | 同步偏差 | 峰值／噪声 | 相关／峰差 |
| 3 | 同步状态 | 削顶／丢帧计数 | 最近处理耗时 |
| 4 | PPS 异常计数 | 采集硬件错误 | 最近拒绝原因 |

切换分组的操作顺序为：保存当前分组编号 → 更新三个按钮选中外观 → 更新四行标签 → 使用当前快照填入四行数值 → 重绘变化区域。无需跳到新的 Screen，也无需复制三套文字控件。

没有数据时显示 `--` 或“未接入”，不要显示虚假的 `0`。例如真实异常计数为 0 与暂时没有统计接口，含义不同。

该页不设置 LED 测试按钮，也不承担修改测量配置的任务。

## 9. 开发时应修改哪些文件

| 文件区域 | 负责内容 | 操作方式 |
| --- | --- | --- |
| `DistributedAcousticRanging.touchgfx` | 页面、控件名、尺寸、样式、Interaction | 通过 Designer 修改并 Generate Code |
| `assets/texts/texts.xml` | 文本、字体、字库、对齐 | 优先通过 Designer Texts 界面修改并生成 |
| `generated/gui_generated/` | 控件成员、缓冲区、初始配置、导航回调 | 自动生成，不直接编辑 |
| `gui/include/gui/<页面>_screen/` | 手写 View／Presenter 声明 | 添加公开更新接口、回调和界面状态 |
| `gui/src/<页面>_screen/` | 手写 View／Presenter 实现 | 更新显示，处理本地事件，向 Model 提交意图 |
| `gui/include/gui/model/`、`gui/src/model/` | Model 与 ModelListener | 保存跨页状态，获取后端快照，通知活动页面 |
| `App/Ranging/`、`App/TimeSync/`、`App/AudioArrival/` | 测距、同步、采集 | 提供快照／命令接口，在所属任务中处理硬件与协议 |

上表路径相对于本文件所在的 TouchGFX 目录；`App/` 位于项目根目录。文末提供可直接打开的绝对路径链接。

## 10. 动态文字如何更新

### 10.1 已有 Wildcard 的使用方法

控件名为 `diagnosticsSyncText` 时，生成的成员为：

- 控件：`diagnosticsSyncText`。
- 缓冲区：`diagnosticsSyncTextBuffer`。
- 容量常量：`DIAGNOSTICSSYNCTEXT_SIZE`。

这些成员是 `protected`，可以在手写 `diagnosticsView` 中使用。生成构造函数已经把缓冲区绑定给控件，正常更新时不必每次重新 `setWildcard()`。

纯英文／数字可以这样更新：

```cpp
touchgfx::Unicode::snprintf(diagnosticsSyncTextBuffer,
                           DIAGNOSTICSSYNCTEXT_SIZE,
                           "BOARD %c  WAIT SYNC", 'A');
diagnosticsSyncText.invalidate();
```

缓冲区容量单位是 Unicode 字符个数，包含结尾的 0。不要换成指向栈上临时数组的 Wildcard 指针，也不要直接修改生成的 `_SIZE`；容量需要变化时回到 Designer 修改。

`Unicode::snprintf()` 的 `%s` 参数必须是 `UnicodeChar*`，不能直接传普通英文 `char*`。它也不支持普通 `%f`；需要时用 `snprintfFloat()`，或优先使用整数定点格式化。不要把标准库 `std::snprintf()` 的参数规则原样套用过来。

### 10.2 中文、℃、μ 等字符

普通 `char*` 到 Unicode 的简单拷贝适合 ASCII，不会自动完成 UTF-8 中文解码。对于 UTF-8 文本，可在手写 View 中使用：

```cpp
touchgfx::Unicode::fromUTF8(
    reinterpret_cast<const uint8_t*>(u8"等待同步"),
    diagnosticHintTextBuffer,
    DIAGNOSTICHINTTEXT_SIZE);
diagnosticHintTextBuffer[DIAGNOSTICHINTTEXT_SIZE - 1] = 0;
diagnosticHintText.invalidate();
```

以上 `u8` 示例适用于 C++11／14／17；本项目固件配置为 C++11，源文件保存为 UTF-8。动态数值与中文混排时，可先构造完整 UTF-8 字符串再转换，或使用具名 TypedText 加占位符。主页现有 `setText()` 辅助函数按 ASCII 使用，且只在主页 `.cpp` 的匿名命名空间中可见；其他页面要复用时先提取为公共辅助函数。

转换正确还不等于字库包含字形。新增运行时文案前，要在对应 Typography 的 Wildcard Characters／具名文本中补齐字符并生成资源。`UI14` 和 `UI16` 是独立字库；给其中一个补字不会自动补到另一个。`Large` 目前用于数字和大写 `M`，如果以后显示小写 `m`、`cm` 等，也要补对应字形。

对经常切换的固定文案，建议在 Designer 中建立稳定的具名 Text ID，例如 `TXT_FREEZE`、`TXT_RESUME`，再调用 `setText(TypedText(...))` 或 `setTypedText(...)`。不要把自动生成的 `__SingleUse_...` ID 当作长期业务接口。

例如先在 Designer 中创建 `TXT_RESUME`、配置文本“继续”并生成，之后可在手写波形 View 中包含 `<texts/TextKeysAndLanguages.hpp>`，调用 `freezeButton.setText(touchgfx::TypedText(T_TXT_RESUME));`，再 `freezeButton.invalidate();`。

### 10.3 改颜色、显隐和重绘

```cpp
#include <touchgfx/Color.hpp>

diagnosticHintText.setColor(touchgfx::Color::getColorFromRGB(78, 96, 117));
diagnosticHintText.invalidate();

demoText.invalidate();       // 先使旧区域需要重绘
demoText.setVisible(false);  // 切到真实数据模式时隐藏SIM
```

上面的隐藏操作仅在真实数据模式分支执行。重新进入预览模式时，先 `demoText.setVisible(true)`，再 `demoText.invalidate()`；真实数据暂时失效不应伪装成 SIM 模式。

文字、颜色或外观改变后重绘对应控件即可，不需要每次重绘整屏。运行中修改位置／尺寸时还应处理旧区域；本项目通常由 Designer 管理布局，运行时只改内容和状态。无变化的数值不必反复写缓冲区。

## 11. 按钮如何绑定、选中与禁用

### 11.1 推荐先使用 Designer Interaction

对新业务按钮可采用与原主页 `button1Clicked()` 相同的方式：

1. 在 Designer 为按钮添加 Click 触发的 Interaction。
2. Action 使用调用虚函数的方式，命名为清楚的业务动作，例如 `onTemperaturePlus`、`onSaveCalibration`、`onDiagAudio`。
3. Generate Code，在 ViewBase 中得到对应的虚函数。
4. 在手写 View 的头文件中声明 `void onTemperaturePlus() override;`，在 `.cpp` 中实现。
5. View 修改本地显示草稿，或通过 Presenter 请求 Model／后端处理。

业务函数名是后续建议，当前尚未生成。底部导航已经配置完成，保留现有 Interaction 即可。

### 11.2 也可以在手写 View 绑定回调

当前 FlexButton 的回调参数类型是 `const touchgfx::AbstractButtonContainer&`，与旧的普通 `Button` 不同。如果采用代码绑定：

- 在派生 View 声明持久的 `touchgfx::Callback<派生View, const touchgfx::AbstractButtonContainer&>` 成员。
- 构造函数中用 `this` 和成员处理函数初始化回调。
- 在 `setupScreen()` 调用基类后，对对应业务按钮调用 `setAction(callback)`。
- 处理函数用控件地址区分来源，例如 `&src == &diagAudioButton`。

例如为冻结按钮绑定手写事件时，需在 `waveformView` 中增加以下成员，并补全处理函数：

```cpp
// waveformView.hpp：类成员，不能放在setupScreen的局部作用域
touchgfx::Callback<waveformView,
                  const touchgfx::AbstractButtonContainer&> actionCallback;
void onAction(const touchgfx::AbstractButtonContainer& source);

// waveformView.cpp：替换原有空构造函数，并在setupScreen中绑定
waveformView::waveformView()
    : actionCallback(this, &waveformView::onAction)
{
}

void waveformView::setupScreen()
{
    waveformViewBase::setupScreen();
    freezeButton.setAction(actionCallback);
    // 此处同时根据Model保存的冻结状态恢复按钮和画面。
}

void waveformView::onAction(const touchgfx::AbstractButtonContainer& source)
{
    if (&source == &freezeButton)
    {
        // 后续：请求切换显示冻结状态，再刷新按钮文字与外观。
        // 本示例仅说明事件接线，不会停止DMA或实现冻结。
    }
}
```

ViewBase 的 `flexButtonCallbackHandler()` 是私有函数，不应直接改写或尝试 override。对同一按钮应选择一种绑定方式，避免手写 `setAction()` 覆盖 Designer 刚生成的事件。不要重新绑定已经工作的导航按钮。

### 11.3 选中状态和禁用状态

长期选中状态可以用 `setBoxWithBorderColors()`、`setTextColors()` 更新，再 `invalidate()`。选择分组时要同时更新所有同组按钮，避免同时出现两个“选中”外观。`setPressed()` 表达触摸按下，不适合保存当前选项。

禁用示例可以写在 `settingsView::setupScreen()` 或统一状态刷新函数中：

```cpp
tempAutoButton.setTouchable(false); // 没有温度传感器
tempAutoButton.invalidate();
```

设置页还应在业务入口检查传感器可用性。相同原则适用于“正在应用”的按钮、“尚无合格候选”的保存按钮和采样中的参考距离加减。

`timebaseButton`、`amplitudeButton`、`freezeButton`、`startCalibrationButton` 当前的按钮文字都是静态 TypedText。如果后续要显示动态时基、冻结／继续或确认 B 侧，应先增加具名文本或为按钮文字配置合适的 Wildcard，不能假定已有文字缓冲区。

## 12. 状态保存、线程与后端接入

### 12.1 建议的数据流

```text
后端任务 → 稳定快照／消息队列 → Model → 活动Presenter → View控件
触摸事件 → View → Presenter → Model提交命令 → 所属后端任务
```

当前 `Model::tick()` 为空，主页直接在 `handleTickEvent()` 中读取 `Ranging_GetStatus()`。可以先保留原有路径，再逐步把跨页需要的数据汇总为统一快照；文档中的推荐数据流尚未接入。

| 层次 | 应负责的内容 |
| --- | --- |
| View | 格式化文字、重绘、按钮外观、短暂交互状态 |
| Presenter | 页面需求与 Model 接口之间的连接，进入页面时恢复显示 |
| Model | 跨页缓存、编辑草稿、已应用配置、分组／时基偏好、快照版本与命令状态 |
| 后端任务 | 实际音频采集、同步、测距、标定计算、双板配置协调和持久化 |

当前 FrontendHeap 只有一份 View／Presenter 分区，切页不能依赖 View 成员一直存在。温度草稿、已应用配置、诊断分组、标定进度等需要保留时放入 Model 或后端；每页在 `setupScreen()`／Presenter `activate()` 时读取并恢复。`setupScreen()` 先调用基类；离页清理临时订阅或显示快照，不能顺手取消全局测距。

框架先调用新 View 的 `setupScreen()`，再调用新 Presenter 的 `activate()`。因此不要在 `setupScreen()` 中依赖一个只有等到 `activate()` 才会初始化的 Presenter 缓存；可以在后者中取得数据后调用 View 的公开刷新接口。

### 12.2 GUI 线程与共享数据

- 只在 GUI 线程中修改 TouchGFX 控件。中断、音频 DMA 回调、同步任务将数据发布到快照或队列。
- 不在触摸回调或 `Model::tick()` 中等待网络 ACK、Flash 写入或完整采样；提交命令后由后续状态通知完成。
- 同步任务拥有的接口不要从 GUI 直接调用。当前 `Ranging_SetLink()`、`Ranging_Receive()`、`Ranging_Poll()` 在头文件中明确属于 SyncTask。
- 对状态保留来源、事件号／配置版本、有效性和更新时间。失去同步或链路中断时及时更新状态，避免旧数据看起来仍然有效。
- PCM 快照要有明确的所有权与生命周期，使用稳定副本或双缓冲；不要在 GUI 复制时让 DMA 同时改写同一数组。

### 12.3 现有接口与仍需补充的数据

| 数据 | 当前情况 |
| --- | --- |
| 板卡角色、同步锁定、测量状态、结果有效性、声源侧 | `RangingStatus` 已包含 |
| 事件号、距离、Q、接受／拒绝数、时差、最后测距错误 | `RangingStatus` 已包含；格式化前检查 `Ranging_GetStatus()` 返回值 |
| 具体同步偏差、链路详情、PPS 异常数 | 需要同步层整理并提供 UI 可读快照，不应从 `delay_ns` 推导 |
| 音频／击掌类型、独立相关值、峰差、处理耗时 | 需要后端补充公开结果字段，不能由 UI 伪造 |
| PCM 公共窗口、声道、采集统计 | 需要音频层提供稳定快照 |
| 运行时温度／声速修改、双板确认、标定保存 | 当前没有完整的 UI 命令接口，需要后续实现 |

## 13. 不依赖双板的后续开发顺序

1. **本地交互**：设置页加减与互斥选择，诊断分组切换，波形控制的外观和状态。
2. **统一预览状态**：在明确的 SIM 模式中提供等待、有效、拒绝、超时、标定失败等场景；切页恢复同一份状态。
3. **接口层**：建立快照与命令状态，优先接入现有 `Ranging_GetStatus()`，新增字段没有数据时显示占位。
4. **后端能力**：运行时配置、标定、公共波形窗口和对应双板确认机制。
5. **实物验证**：以后具备环境时验证刷新、触摸、同步、参数生效、测量精度和异常恢复。

本地阶段可检查：五页导航是否连续可用；选项是否始终互斥；长中文与负数是否截断或缺字；无数据与真实零值是否区分；离页返回是否保留草稿；冻结是否仅停止显示；重新 Generate Code 后手写逻辑是否仍保留。预览数据不作为精度或双板一致性验证结果。

## 14. 主要文件索引

- [Designer 工程](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/DistributedAcousticRanging.touchgfx)
- [文本与字库配置](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/assets/texts/texts.xml)
- [主页手写 View](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/screen_screen/screenView.cpp)
- [设置页手写 View](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/settings_screen/settingsView.cpp)
- [标定页手写 View](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/calibration_screen/calibrationView.cpp)
- [波形页手写 View](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/waveform_screen/waveformView.cpp)
- [诊断页手写 View](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/diagnostics_screen/diagnosticsView.cpp)
- [Model 实现](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/src/model/Model.cpp)
- [ModelListener 声明](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/TouchGFX/gui/include/gui/model/ModelListener.hpp)
- [测距公开接口](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/App/Ranging/ranging.h)
- [测距和标定算法已有说明](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/App/Ranging/README.md)
- [当前声速与偏置默认配置](D:/STM32/STM32F746NGH6/DistributedAcousticRanging/App/Ranging/range_config.h)

生成文件可用于核对成员名、缓冲区和实际图表配置；业务代码仍写在手写 View／Presenter／Model 中。后续增删控件或修改 Interaction 后，应同步维护本文档的控件名称与实现状态。
