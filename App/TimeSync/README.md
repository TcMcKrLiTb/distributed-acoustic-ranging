# TimeSync 移植说明

当前实现从 `workofembeddedsystem` 的实际源码移植，沿用其报文格式、主从选举、
8Hz 四时间戳交换、PI 参数和 ETH target → TIM2 的硬件触发方式。
参考固件已有用户示波器实测：收敛稳定后连续十次脉冲上升沿差均不大于 50ns。
该结果是移植基线；本工程与 TouchGFX/FreeRTOS 共存后的精度需重新实板验收。

## 代码边界

| 文件 | 职责 |
| --- | --- |
| `sync_core.c/.h` | 报文编码/校验、四时间戳计算、异常样本过滤、PI 伺服；与参考源码一致 |
| `sync_app.c/.h` | 主从选举、协议状态机、会话重启/超时、状态汇总；仅由同步任务调用 |
| `sync_task.c` | CMSIS-RTOS 任务入口、每轮休眠、跨任务状态快照 |
| `time_sync.h` | 提供给其他应用和 TouchGFX Model 的 C/C++ 接口 |
| `Port/sync_port_stm32.c` | LAN8742、裸以太网收发、硬件时间戳、PTP 时钟调整、LED |
| `Port/sync_pps_stm32.c` | TIM2 单脉冲、整秒目标装载、相位跳变后的重装、漏触发恢复 |
| `Port/sync_log.c` | 静态日志队列；同步任务投递，defaultTask 输出 UART |
| `sync_hw.h` | 适配当前由 `main.c` 定义的外设句柄 |
| `sync_format.c` | 64 位有符号偏差转十进制，兼容 newlib-nano |

SyncTask 是 ETH/PTP 和协议状态的唯一任务所有者。其他任务通过公开接口读取快照，
不直接调用 `SyncApp_*`、`SyncPort_*` 或修改 PTP/TIM2 寄存器。
本版使用 EtherType `0x88B5` 的私有二层协议，不需要 IP、LwIP 或 DHCP，
也不是可以直接和通用 IEEE 1588 PTP 主时钟互通的协议栈。

## 启动及任务

CubeMX 在 `main()` 中先初始化外设，再创建任务并启动调度器。
`Sync_Task()` 内初始化日志队列及 ETH/PHY/PTP/PPS，然后循环执行
`SyncApp_Poll()`、发布快照、`osDelay(1)`。
维持现有 SyncTask 的 High 优先级、1024 words（4096 字节）栈配置。

defaultTask 通过 `App_BackgroundPoll()` 每轮最多取出一条日志并阻塞发送到 UART1。
队列深度 4，每条最多 383 个文本字节；队列满时丢弃日志，不阻塞同步任务。
UART 发送失败也计入 `log_dropped`。日志消费缓冲放在静态 RAM，
避免占用 defaultTask 的 256 words 栈。该队列不消耗 FreeRTOS 动态堆。
UART1 当前由此日志消费者独占；未来其他模块的串口输出应统一调度。

## CubeMX 和寄存器配置

迁移沿用已配置的 RMII、LAN8742、RNG、UART1、MPU、RAM 和任务；
OPM/URS 在 PPS 初始化中显式设置，无需为此切换 CubeMX 的 PWM 通道模式。

- HCLK=216MHz，APB1=54MHz，当前 TIM2 时钟=108MHz。
- TIM2：PSC=0，向上计数，CH1 PWM2、高电平有效，CCR1=1，ARR=108000，
  PA15/AF1，Trigger slave mode，ITR1 remap=`TIM_TIM2_ETH_PTP`。
  `SyncPps_Init()` 检查关键配置，并根据实际定时器时钟设置 ARR、CCR1、OPM、URS。
  PWM2 高电平宽度为 `(ARR + 1 - CCR1) / TIM2_clock`，当前恰为 1ms。
- 不通过 `HAL_TIM_PWM_Start()` 或任务软件置位产生 PPS；ETH 整秒 target 经 ITR1
  启动 TIM2，硬件产生边沿。周期为 1 秒，高电平为 1ms。
- `main.c` 的唯一 PWM HAL 回调分发到 `SyncPps_OnPulseFinished()`；
  唯一 PeriodElapsed HAL 回调分发 TIM2 并保留 TIM6 的 `HAL_IncTick()`。
  TIM2 IRQ 保持优先级 5，TIM6 保持优先级 4。
- ETH 收发采用任务轮询，不需要添加 ETH NVIC 中断，也不引入 ETH ISR 中的 RTOS API。
- `HAL_ETH_USE_PTP` 必须继续定义在 `stm32cubemx` INTERFACE 目标，
  以确保应用和 HAL 对含条件字段的句柄结构有一致理解。
- ETH RAM 为 `0x2004C000..0x2004FFFF`、16KB；MPU 为 Normal、全访问、
  XN、非缓存/非缓冲、Shareable（TEX=1、C=0、B=0）。保留全局 I/D Cache。
- 链接 `.eth_dma` 为 NOLOAD，接收池和发送缓冲由端口初始化显式清零。
  当前 Debug/Release 均使用 12960 字节，终点为 `0x2004F2A0`（不含），
  剩余 3424 字节。TouchGFX 帧缓冲仍在 SDRAM，资源仍在 QSPI。
- UART1 当前为 **1152000、8N1**，TX=PA9，RX=PB7。
- 板载 LED1 使用当前 `main.h` 定义的 PI1；锁定估计时常亮，其余状态闪烁。

CubeMX 再生成代码后保留 USER CODE 区，并检查上述参数、根 CMake 的 PTP 宏和
自定义链接脚本中的 ETHRAM/SDRAM/QSPI 段。无需将移植代码放回生成的 Core 文件。

## 其他模块和 TouchGFX 使用

```cpp
#include "TimeSync/time_sync.h"

TimeSyncStatus status;
if (TimeSync_GetStatus(&status)) {
    // 根据 status.state / role / offset_ns 更新界面模型。
}

uint64_t time_ns;
if (TimeSync_NowNs(&time_ns)) {
    // 本地 PTP 时间；是否已同步还需结合 status 判断。
}
```

两个函数都只供任务上下文调用。快照复制受 FreeRTOS 临界区保护，
包含 64 位字段时也不会读到一半新、一半旧的数据。
首次发布前 `TimeSync_GetStatus()` 返回 false，并给出零初始化的 STARTING 状态。
初始化失败后会发布 ERROR；`TimeSync_NowNs()` 返回 false。
PTP 时间从本次启动的零开始，非 UTC，初始收敛时允许相位跳变。
本版没有修改现有界面显示。

MASTER 的 `offset_ns`/`delay_ns` 来自 SLAVE 最近一次 HELLO；
`frequency_ppb`、`samples`、`rejected` 始终是本板本地伺服数据。
`LOCKED_ESTIMATE` 沿用参考的连续 16 个有效样本、估算偏差 ±2500ns 判定，
它只用于状态提示，不能代替示波器的 ≤50ns 验收。
读取 `sample_valid` 后再解释 `offset_ns`；超时后可能保留上一笔数值。

## 软件验证

在工程根目录运行（需要主机 MinGW GCC，以及本地已生成的 HAL/CMSIS 文件）：

```powershell
./tools/test_timesync.ps1
# 若 GCC 不在 PATH：
./tools/test_timesync.ps1 -Gcc 'D:/Environment/mingw64/bin/gcc.exe'
```

测试输出放在已忽略的 `build/host-tests`。覆盖报文校验、有符号字段、时钟漂移/
启动偏差模拟、异常延迟、主从选举、重复/乱序帧、重启、断链、样本过期、错误状态、
64 位日志格式，以及使用真实 STM32 寄存器定义的 PPS 装载/重装/时钟计算/漏触发恢复。
模拟测试的纳秒数值不是硬件测量。

本次移植已通过 Debug 全量构建、Release 构建和上述四组主机测试。
已检查链接地址、唯一 HAL 回调和编译器栈使用报告；
实时任务栈余量、TouchGFX 交互及以太网时序仍需在实板运行中验证。

## 双板验收

1. 两板烧录相同固件，网线直连，使用支持 1152000 波特率的串口工具观察日志。
   由完整 96 位 UID 自动选主，UID 较小者为 MASTER，不需要分别编译固件。
2. 上电后确认各自 GUI 显示和触摸工作正常；日志应从 LINK_DOWN/WAIT_PEER
   进入 ACQUIRING，再进入 LOCKED_ESTIMATE；观察 `sample=VALID`。
3. 示波器两通道测两板 D9/PA15，接好参考地并校正探头/通道固有时差。
   确认约 1Hz、1ms 高电平；收敛稳定后连续记录十次上升沿时间差，
   对照参考实测基线：每次绝对差均不大于 50ns。
4. 保持图形刷新并操作触摸，检查 PPS 仍连续、`pps_missed` 不持续增长、
   `tx_fail` 不持续增长；同时观察实际栈余量。
5. 拔插网线、单板复位后应重新发现、选主、收敛。相位调整期间允许跳过一个
   整秒输出，避免旧 target 在时间跳变时错误触发。中断或任务被调试暂停后的采样
   不作为同步精度验收数据；运行恢复后重新等待稳定。
