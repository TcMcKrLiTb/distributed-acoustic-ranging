# 串口日志

2026-10-31 更新：正式估计器为 DIRECT，CMP 每次输出 LONG、FRONT、DIRECT 三行，
`dsp_ms` 位于 DIRECT 行；AUDIO INIT 追加 `rearm=200ms quiet=100ms`。
CMP追加`span=begin:end`以记录实际分析跨度；支持提前触发重定位的版本启动含`late_limit=1024`。
当前实现和测试见 [DIRECT 验证记录](Ranging/DIRECT_VALIDATION.md)，下文 LONG 发布描述属于修改前基线。

默认使用精简模式，便于观察两板的声音事件和测距结果。串口仍为 1152000、8N1、无流控。

当前 `feature/newRanging` 调试版默认开启 PCM 分块导出，并增加同一配对数据的
`LONG`/`FRONT` 算法对照。实际测距仍由原来的 `LONG` 决定；`FRONT` 只输出诊断，
不会代替拒绝结果或更新两屏距离。双串口采集、首轮实验与 WAV/JSON 还原方法见
[PCM 串口诊断](Diagnostics/README.md)，正常事件日志仍然保留。

```text
BOOT acoustic ranging; UART 1152000
RANGE INIT range_mm=200..2000
SYNC role=MASTER(A) state=ACQUIRING
AUDIO INIT fs=48000 ch=0 gain=80 warmup=1s confirm=96..480
AUDIO READY
SYNC role=MASTER(A) state=LOCKED_ESTIMATE
AUDIO EVENT seq=1 arrival=12.345678901 interval=NA peak=800 noise=5 threshold=128 sync=LOCKED
RANGE RESULT id=1 distance_mm=400 source=A quality=985
```

数字及启动日志的先后顺序仅为示例。

手动温度设置仅在配置改变、完成同步或重算旧结果时输出少量 `TEMP` 日志，
配置心跳和重复确认不周期刷屏：

```text
TEMP APPLY rev=2 temp_x10=300 c_mm_s=349228
TEMP SYNCED rev=2
```

`temp_x10` 为摄氏温度乘 10，`c_mm_s` 为修正声速（毫米/秒），`rev` 为主板分配的配置版本。
`TEMP RECALC` 表示使用上次有效时差重新换算，并非检测到新的音频事件；
不增加 `RANGE RESULT` 编号或成功计数。`dt_ns` 是已经扣除输入链路偏置的时差，
可直接按本次声速计算理论距离。`valid=0` 表示重算结果超出原有量程。
设置与验收步骤见 [温度补偿说明](Ranging/TEMPERATURE.md)。

- `SYNC` 只在板子角色或同步状态改变时输出，包括失锁、断链和重新锁定。
- `AUDIO READY` 在背景学习完成后输出一次，正常的 HOLDOFF/ARMED 循环不打印。
  如果采样不连续、时钟跳变或重启 DMA 导致重新学习，完成后会再次提示。
- 每次接受声音事件输出一条 `AUDIO EVENT`。`arrival` 和 `interval` 的单位均为秒，
  保留 9 位小数；首次或时钟不连续时 `interval=NA`。
- 事件保留序号、到达时间、间隔、滤波峰值 `peak`、触发时的噪声估计 `noise`、
  触发门限 `threshold` 和同步状态。削顶时追加 `clipped=1`。
- 原有最少 96 点确认和双阈值/形态门限不变。形态合格但峰值不足 `2 * threshold` 的连续弱候选，
  可保留最早交点并最多确认 480 点；延长期间连续 48 点低于 `threshold / 2` 时作废。
  强音通常仍为 96 点，不增加事件行长度。发生延长确认时，精简事件行才追加 `confirm=N`：
  单位是采样点，标称 48 kHz 下 96/480 点约为 2/10 ms。
  `arrival` 始终来自保留的最早交点；`confirm` 不是附加在到达时间上的传播延迟。
  例如 `AUDIO EVENT ... peak=300 noise=4 threshold=128 sync=LOCKED confirm=240`。
  `peak` 仍是确认期间的峰值；延长确认时统计跨度会改变，不能把新旧事件行的峰值直接当作固定窗口幅度对照。
- `RANGE RESULT` 保留结果编号、毫米距离、声源方向和相关质量；`quality` 为千分比，
  例如 985 表示约 98.5%。两板分别保存日志，默认行内不重复打印板号。
- `RANGE REJECT`、`AUDIO ERROR`、`AUDIO RECOVER` 保留。
  完成互相关搜索后被拒绝的测量，会附带相关质量（千分比）与最佳整数延迟点数，例如：
  `RANGE REJECT reason=quality_or_range error=3 quality=612 lag=-9`。
  `lag` 是重采样到 A 的采样间隔后、抛物线插值前的最佳候选位置，本轮搜索范围为 -127..127（旧快照为 -96..96）；
  它不是最终的两板传播时差，还需结合两片段的 PTP 起始时间。
  未完成搜索的拒绝或超时不输出这两个字段，以免误用上次计算值。
- 每秒检查一次采集健康计数，仅在丢块、时间检查失败、硬件错误、时钟捕获跳过的计数变化，
  或该周期有原始采样削顶时输出 `AUDIO WARN`。
  `drop/timing/hwerr/skip` 为累计计数，`clip` 为该检查周期内的削顶采样数。
  稳定无异常时没有周期刷屏。

## 当前测距调试版

启动额外输出 `RANGE DSP primary=LONG compare=FRONT long=128:1920 front=128:384 lag=-127:127`，
区间右端不包含。每对进入 DSP 的片段会在主板输出两条同数据对照，例如：

```text
RANGE CMP a=1 b=1 win=LONG error=3 q=437 q2=419 gap=18 lag=12 frac=-7144 dt_ns=-2015992 mm=692
RANGE CMP a=1 b=1 win=FRONT error=0 q=962 q2=930 gap=32 lag=11 frac=11800 dt_ns=-2036858 mm=699 dsp_ms=20
```

`error=0` 才表示对应算法通过全部检查。`q/q2/gap` 分别为最佳相关分数、距最佳整数位置超过12点处
的最大竞争分数及两者差值，按千分值显示；`lag` 为整数位置，`frac` 为 Q16 小数采样修正。
`q/q2` 的负相关分数显示为 0，`gap` 使用内部未截断分数之差。
`dt_ns` 为候选原始有符号 B-A 时间差，`mm` 为使用当前声速和偏置换算的候选距离；
即使 `error` 非零也可能输出候选值，不能当成已接受的测距。搜索尚未完成的字段、无法计算的时间差或距离显示为 `NA`。
`FRONT` 行末 `dsp_ms` 是两次算法的总经过时间，含抢占。详细解释和实验安排见
[PCM 串口诊断](Diagnostics/README.md)。

正式 `RANGE RESULT`、`RANGE REJECT` 仍由 `LONG` 决定。因此出现 FRONT 的 `error=0`，
但两屏没有更新距离、随后仍有 `RANGE REJECT`，是本轮诊断的预期行为。
PCM 导出和对照日志可穿插出现，采集脚本会完整保留原始日志；提供测试数据时保留两个串口的全部文件。
本轮两板应烧录同一份新 `build/Debug/DistributedAcousticRanging.elf`，不要混用旧 Release。
修改前快照为 `3363294`。旧 PCM 回放只能验证固定片段行为，不能通过补零重建缺失的采样并当作新实测。

## 完整诊断模式

需要检查无事件时的输入幅度、噪声、采样速率、PTP 偏移或 PPS 计数时，将
`App/TimeSync/Port/sync_log.h` 中的 `APP_LOG_VERBOSE` 默认值改为 `1`，重新构建并烧录。
也可以通过编译宏 `APP_LOG_VERBOSE=1` 为整个应用启用它。

完整模式恢复每秒的 `AUDIO STATUS` 和同步统计，以及原来 AUDIO EVENT / RANGE RESULT 的
详细字段。原有依赖 `epoch`、`board`、`dt_ns` 等字段的日志校验脚本应使用完整模式日志。
切回 `0` 恢复精简输出；两种模式都不改变采集、同步和测距判断。
