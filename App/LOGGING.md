# 串口日志

默认使用精简模式，便于观察两板的声音事件和测距结果。串口仍为 1152000、8N1、无流控。

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

## 完整诊断模式

需要检查无事件时的输入幅度、噪声、采样速率、PTP 偏移或 PPS 计数时，将
`App/TimeSync/Port/sync_log.h` 中的 `APP_LOG_VERBOSE` 默认值改为 `1`，重新构建并烧录。
也可以通过编译宏 `APP_LOG_VERBOSE=1` 为整个应用启用它。

完整模式恢复每秒的 `AUDIO STATUS` 和同步统计，以及原来 AUDIO EVENT / RANGE RESULT 的
详细字段。原有依赖 `epoch`、`board`、`dt_ns` 等字段的日志校验脚本应使用完整模式日志。
切回 `0` 恢复精简输出；两种模式都不改变采集、同步和测距判断。
