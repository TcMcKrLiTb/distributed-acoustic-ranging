# DHT11 / STM32F746 / FreeRTOS

## 接线与时基

- DATA：`PG6`，CubeMX label：`DHT11_SIG`。
- 保持开漏输出、无内部上下拉，使用模块外部 **10 kΩ** 上拉。
- 输出 `0` 拉低总线；输出 `1` 释放总线。直接读 GPIO `IDR`，不用切换输入输出模式。
- GPIO 初始输出为 `SET`，上电即释放总线；`.ioc` 已同步保存这一设置。
- TIM14 使用内部时钟，每计数 **1 µs**，16 位自由运行，无 PWM 输出、无定时器中断源。
  当前 APB1 定时器时钟 108 MHz，`PSC=107`、`ARR=65535`，约 65.536 ms 回绕。
  移植层根据实际 APB1 分频和 `TIMPRE` 计算 PSC，并检查能否准确分频到 1 MHz。
- CubeMX 中 TIM14 的 Activated 保持启用，Channel1 设为 Disable，NVIC 的
  `TIM8 trigger and commutation interrupts and TIM14 global interrupt` 取消勾选。
  PWM No Output、PWM 初始化和 TIM14 中断处理函数均已移除；初始化只调用
  `HAL_TIM_Base_Init()`，驱动用 `HAL_TIM_Base_Start()` 启动基础计数器。
  驱动同时清零 `DIER/CCER`，保持定时器中断源和通道输出关闭。
  TIM14 由该模块独占，请勿在其他代码中重置 CNT、启动 PWM 或开启其中断。
- PG6 保持 CubeMX 的开漏输出配置。移植层运行时将 EXTI6 路由到 PG6，配置
  双边沿及 `EXTI9_5_IRQn` 优先级 **3**，每次采样只开启约 4 ms 的信号边沿采集。
  此中断由 `App/DHT11/driver_dht11_interface.c` 定义，CubeMX 无需新增 GPIO EXTI 模式。
  EXTI6 和共享的 EXTI9_5 向量由该模块独占，当前工程没有其他使用者。

## 启动和读取

`App_Init()` 调用 `DHT11_Init()`，创建静态低优先级任务（768 words 栈）。
上电等待至少 2 s，调用 LibDriver 初始化整帧探测；成功后再等待至少 2 s 才发布第一帧。
后续每次事务结束后等待至少 2 s，失败也按此间隔重试，不阻塞启动或 defaultTask。
初始化探测本身会触发一次传感器应答，因此不能紧跟一次读请求。

其他任务通过缓存接口读取，不直接调用单总线驱动：

```c
#include "DHT11/dht11.h"

DHT11Status sample;
if (DHT11_GetLatest(&sample)) {
    /* 253 表示 25.3 ℃；humidity_percent 单位为 %RH。 */
    int16_t temperature_x10 = sample.temperature_c_x10;
    uint8_t humidity = sample.humidity_percent;
    /* 使用 temperature_x10、humidity。 */
}
```

接口只供任务上下文调用，对快照复制做短临界区保护。
返回 `true` 表示最新一次采样成功且样本不超过 6 s。
最近读取失败时立即返回 `false`，保留旧值用于诊断；不要忽略返回值或 `valid`。
`initialized` 只表示初始化探测曾成功，不代表当前接线正常。
`attempts` 包含初始化探测，`samples` 仅统计成功采样，`failures` 统计失败事务。
`sampled_ms` 是本机 HAL tick；`response_us` 不含 20 ms 起始低电平。
`edge_count` 正常应为 **84**（主机释放边沿、3 个握手边沿、40 对数据边沿）。

`last_error`：`0` 成功，`1` 读/总线/校验失败，`2/3` 为上游参数或句柄错误，
`4` 初始化无应答，`5` 端口初始化或时序异常，`6` 尚未就绪。
`timing_error` 区分边沿顺序/脉宽/完整性错误与普通无应答/校验错误。
采集诊断中的 `capture error`：1 为边沿顺序错误，2 为脉宽不合理，3 为超时；
同时记录 `edges` 和成功解码的 `bits`，便于定位。

串口输出复用现有 `SyncLog` 队列，例如：

```text
DHT11 ready error=0 timing=0 response_us=3900
DHT11 temp_x10=253 humidity=58% edges=84 response_us=3900 samples=1 failures=0
```

日志使用整数格式，不依赖 newlib-nano 浮点 printf。
日志队列满时可丢弃，状态接口仍保留最新结果。
该移植提供传感器采样和数据接口；现有 GUI、手动环境温度和双板声速配置逻辑保持原有行为。

## 与音频、PTP 共存

旧轮询移植保留硬件中断，却在回调空隙超过 15 µs 时丢弃当前帧。
实际 GUI/音频负载下出现多次 `error=5 timing=1`，因此改为信号边沿时间戳采集。

20 ms 起始低电平和整帧等待均使用 `vTaskDelay()`，**不暂停任务调度，不修改
PRIMASK/BASEPRI，不关闭音频、显示或 tick 中断**。
EXTI6 的短 ISR 仅读取 TIM14 时间戳和 PG6 电平，存入 84 项边沿缓冲；
优先级 3 可抢占现有音频 DMA/显示（5）、HAL TIM6（4），不调用 RTOS API、HAL 回调或日志。
记录完整帧后立即关闭 EXTI6 的 IMR，在任务中按脉宽解码，由 LibDriver 校验和并转换数据。
任务晚恢复不改变已经保存的时间戳，也不会因此丢帧。

从实际主机释放的上升沿计时，避免任务在准备和释放之间被抢占导致假超时。
计时差处理 16 位回绕；检查边沿交替、响应脉宽、数据低/高脉宽和帧完整性，
对 46～54 µs 的模糊高脉冲拒帧。未完整采集时使用 10 ms 的 HAL tick 超时门限，
任务排队可延后超时检查；完整波形自身限制为 6 ms。
无模块、卡线、缺边沿或校验失败都释放总线并在下一周期重试。

所有初始化/读取路径最后调用 `DHT11_PortFinish()` 清理。应用只使用快照接口，
不与传感器任务并发操作单总线。实板仍需检查 PG6 电气波形与实际 ISR 延迟。

## 验证

```powershell
& 'C:\Users\31613\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -B App/DHT11/test_dht11.py
& 'C:\Users\31613\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -B App/DHT11/test_dht11_capture.py
& 'C:\Users\31613\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -B App/DHT11/test_dht11_port.py
cmake --build build/debug -j 8
cmake --build build/release -j 8
```

主机协议测试编译真实 LibDriver，模拟传感器握手与 40 位脉宽，覆盖正负小数温度、
三个读取 API、校验和溢出/错误、模块缺失、卡高/卡低、总线错误和退出路径。
脉宽解码测试遍历字节、计数器回绕和边沿抖动。端口测试使用真实移植层及 HAL/RTOS
寄存器替身，检查计时、中断配置和故障清理；将真实驱动与移植层组合，在任务晚恢复
20 ms 时连续读取 64 帧，验证时间戳采集、正常解码、校验失败和异常脉冲拒帧。
主机测试不能替代 MCU 实际波形和中断负载验证。

实板验收：

1. 烧录后等待约 4 s，检查 USART1 的 `DHT11 ready` 和周期温湿度日志。
2. 逻辑分析仪检查 PG6 空闲高、起始低至少 20 ms、传感器约 80/80 µs 应答，
   以及数据位约 50 µs 低电平、约 26/70 µs 高电平；确认 TIM14 计数单位为 1 µs。
3. 检查温湿度数值合理；断开 DATA 应出现 retry、`valid=false`；接回后自动恢复。
4. 同时运行 TouchGFX、PTP 和音频采集，检查采样成功率、PPS 统计、音频丢块与测距。
   对比两板启用前后的同步/测距表现，并确认正常日志为 `edges=84`。

上游提交、MIT 许可证、本地协议修复和可选整帧回调见 `Drivers/LibDriver/DHT11/README.md`。

## 实板验证记录

2026-10-26，COM8 对应 ST-Link 的 B 节点在 `SLAVE(B) / LOCKED_ESTIMATE` 状态下，
约 30 s 内连续 10 次温湿度采样成功，累计 `failures=0`，每帧均为 `edges=84`。
回传温度为 25.4～25.5 ℃、湿度为 36～37%RH，应答耗时为 3684～3823 µs。
