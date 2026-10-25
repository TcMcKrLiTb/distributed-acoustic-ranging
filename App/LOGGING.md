# 串口日志

默认使用精简模式，便于观察两板的声音事件和测距结果。串口仍为 1152000、8N1、无流控。

当前 `feature/newRanging` 调试版默认开启 PCM 分块导出，并增加同一配对数据的
`LONG`/`FRONT` 算法对照。实际测距仍由原来的 `LONG` 决定；`FRONT` 只输出诊断，
不会代替拒绝结果或更新两屏距离。双串口采集、首轮实验与 WAV/JSON 还原方法见
[PCM 串口诊断](Diagnostics/README.md)，正常事件日志仍然保留。

```text
BOOT acoustic ranging; UART 1152000
RANGE INIT range_mm=200..2000
SYNC role=MASTER(A) state=ACQUIRING
AUDIO INIT fs=48000 ch=0 gain=80 warmup=1s
AUDIO READY
SYNC role=MASTER(A) state=LOCKED_ESTIMATE
AUDIO EVENT seq=1 arrival=12.345678901 interval=NA peak=800 noise=5 threshold=128 sync=LOCKED
RANGE RESULT id=1 distance_mm=400 source=A quality=985
```

数字及启动日志的先后顺序仅为示例。

- `SYNC` 只在板子角色或同步状态改变时输出，包括失锁、断链和重新锁定。
- `AUDIO READY` 在背景学习完成后输出一次，正常的 HOLDOFF/ARMED 循环不打印。
  如果采样不连续、时钟跳变或重启 DMA 导致重新学习，完成后会再次提示。
- 每次接受声音事件输出一条 `AUDIO EVENT`。`arrival` 和 `interval` 的单位均为秒，
  保留 9 位小数；首次或时钟不连续时 `interval=NA`。
- 事件保留序号、到达时间、间隔、滤波峰值 `peak`、触发时的噪声估计 `noise`、
  触发门限 `threshold` 和同步状态。削顶时追加 `clipped=1`。
- `RANGE RESULT` 保留结果编号、毫米距离、声源方向和相关质量；`quality` 为千分比，
  例如 985 表示约 98.5%。两板分别保存日志，默认行内不重复打印板号。
- `RANGE REJECT`、`AUDIO ERROR`、`AUDIO RECOVER` 保留。
  完成互相关搜索后被拒绝的测量，会附带相关质量（千分比）与最佳整数延迟点数，例如：
  `RANGE REJECT reason=quality_or_range error=3 quality=612 lag=-9`。
  `lag` 是重采样到 A 的采样间隔后、抛物线插值前的最佳候选位置，搜索范围为 -96..96；
  它不是最终的两板传播时差，还需结合两片段的 PTP 起始时间。
  未完成搜索的拒绝或超时不输出这两个字段，以免误用上次计算值。
- 每秒检查一次采集健康计数，仅在丢块、时间检查失败、硬件错误、时钟捕获跳过的计数变化，
  或该周期有原始采样削顶时输出 `AUDIO WARN`。
  `drop/timing/hwerr/skip` 为累计计数，`clip` 为该检查周期内的削顶采样数。
  稳定无异常时没有周期刷屏。

## 当前测距调试版

启动额外输出 `RANGE DSP primary=LONG compare=FRONT long=128:1920 front=128:384`，
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

## 完整诊断模式

需要检查无事件时的输入幅度、噪声、采样速率、PTP 偏移或 PPS 计数时，将
`App/TimeSync/Port/sync_log.h` 中的 `APP_LOG_VERBOSE` 默认值改为 `1`，重新构建并烧录。
也可以通过编译宏 `APP_LOG_VERBOSE=1` 为整个应用启用它。

完整模式恢复每秒的 `AUDIO STATUS` 和同步统计，以及原来 AUDIO EVENT / RANGE RESULT 的
详细字段。原有依赖 `epoch`、`board`、`dt_ns` 等字段的日志校验脚本应使用完整模式日志。
切回 `0` 恢复精简输出；两种模式都不改变采集、同步和测距判断。
