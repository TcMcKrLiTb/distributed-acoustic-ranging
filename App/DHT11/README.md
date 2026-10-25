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

## 启动和读取

`App_Init()` 调用 `DHT11_Init()`，创建静态低优先级任务（768 words 栈）。
上电等待至少 2 s，调用 LibDriver 初始化握手；成功后再等待至少 2 s 才读取第一帧。
后续每次事务结束后等待至少 2 s，失败也按此间隔重试，不阻塞启动或 defaultTask。
初始化握手本身会触发一次传感器应答，因此不能紧跟一次读请求。

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
`initialized` 只表示初始化握手曾成功，不代表当前接线正常。
`attempts` 包含初始化探测，`samples` 仅统计成功采样，`failures` 统计失败事务。
`sampled_ms` 是本机 HAL tick；`response_us` 不含 20 ms 起始低电平。

`last_error`：`0` 成功，`1` 读/总线/校验失败，`2/3` 为上游参数或句柄错误，
`4` 初始化无应答，`5` 端口初始化或时序异常，`6` 尚未就绪。
`timing_error` 区分采样时序中断与普通无应答/校验错误。

串口输出复用现有 `SyncLog` 队列，例如：

```text
DHT11 ready error=0 timing=0 response_us=102
DHT11 temp_x10=253 humidity=58% response_us=3900 samples=1 failures=0
```

日志使用整数格式，不依赖 newlib-nano 浮点 printf。
日志队列满时可丢弃，状态接口仍保留最新结果。
该移植提供传感器采样和数据接口；现有 GUI、手动环境温度和双板声速配置逻辑保持原有行为。

## 与音频、PTP 共存

20 ms 起始低电平使用 `vTaskDelay()`，期间允许其他任务运行。
从释放总线到握手/40 位读取完成，仅暂停任务调度，**保留全部硬件中断**：
音频 DMA、PPS、HAL TIM6 和 FreeRTOS tick 仍可响应。
通常这段持续约 3～5 ms，其他任务在事务结束后继续运行。
这会增加一次任务调度延迟，仍需实板验证 PTP、音频处理和测距表现。

上游在握手和数据阶段之间调用一次 `enable_irq`。移植层跨越此间隙持有调度锁，
在包装层 `DHT11_PortFinish()` 中统一释放，所有成功/失败路径都执行清理并释放 DATA。
因此不要从其他地方直接使用 LibDriver API/平台回调，否则可能遗留调度锁。
硬件 PRIMASK/BASEPRI 从不在时序回调中修改。

微秒延时处理 16 位回绕，并增加一个计数 tick，保证最短延时。
中断造成回调间空隙超过 15 µs、延时超出容差或应答超出 6 ms 时，放弃当前帧，
防止受打断的位流被当作有效读数；下一周期重试。
若 `timing=1` 持续出现，应测量现有 ISR 执行时间以及 PG6 波形。

## 验证

```powershell
& 'C:\Users\31613\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -B App/DHT11/test_dht11.py
& 'C:\Users\31613\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -B App/DHT11/test_dht11_port.py
cmake --build build/debug -j 8
cmake --build build/release -j 8
```

主机协议测试编译真实 LibDriver，模拟传感器握手与 40 位脉宽，覆盖正负小数温度、
三个读取 API、校验和溢出/错误、模块缺失、卡高/卡低、总线错误和退出路径。
端口测试使用真实移植层及 HAL/RTOS 寄存器替身，检查计时、调度锁和故障清理；
还将真实驱动与移植层组合，验证实际延时逻辑下的正常解码、校验失败及中断延迟拒帧。
主机测试不能替代 MCU 实际波形和中断负载验证。

实板验收：

1. 烧录后等待约 4 s，检查 USART1 的 `DHT11 ready` 和周期温湿度日志。
2. 逻辑分析仪检查 PG6 空闲高、起始低至少 20 ms、传感器约 80/80 µs 应答，
   以及数据位约 50 µs 低电平、约 26/70 µs 高电平；确认 TIM14 计数单位为 1 µs。
3. 检查温湿度数值合理；断开 DATA 应出现 retry、`valid=false`；接回后自动恢复。
4. 同时运行 TouchGFX、PTP 和音频采集，检查采样成功率、PPS 统计、音频丢块与测距。
   对比两板启用前后的同步/测距表现，确认调度延迟可接受。

上游提交、MIT 许可证和本地两项协议修复见 `Drivers/LibDriver/DHT11/README.md`。
