# 基础第 2 问：声音到达时刻检测

默认日志已改为精简模式，不再每秒打印 ARMED/音频状态。本文下面的完整状态示例与诊断脚本
对应 `APP_LOG_VERBOSE=1`；精简格式和切换方法见 [串口日志说明](../LOGGING.md)。

## 本版实现与边界

板载 MP34DT01 数字麦克风 -> WM8994 DMIC2 -> SAI2 Block B -> DMA2 Stream7 ->
AudioArrival 任务 -> 本板 PTP 时间戳 -> USART1。

- 48 kHz 标称采样、16 位、双通道 DMA，固定取 channel 0。不能随信号强弱切换通道，
  也不把两只位置不同的麦克风求平均。两块板使用相同固件、同一通道。
- 启动后先学习约 1 秒背景噪声；直流抑制、噪声自适应门限、2 ms 声音确认、
  约 600 ms 禁止重复触发，并要求约 100 ms 安静后重新布防。
- 检测时间是首次越过门限的采样位置，确认和串口发送发生在之后；这些处理延迟不加到时间戳里。
- 当前没有课程提供的标准音频文件。本版是通用声音起始检测，不识别某个专有波形；
  标准音频、敲击或说话都可能触发。它不是第 4 问的完整抗干扰实现，也未实现第 3 问的测距。
- 提供的测试 WAV 是自测参考，不代替课程正式音频。正式音频较低频、渐起很慢或背景噪声大时，
  需要结合实测调整门限或增加对该音频的匹配检测。
- 时间戳标记 PCM 检测特征。WM8994 数字滤波、声学路径存在固定延迟，本版没有标定它；
  不应把输出数值当作已校准的麦克风膜片物理到达时刻。重复性可先测试，绝对延迟需单独标定。
- 已做主机算法/计时测试和固件编译；±100 us 指标必须用实物验证，不能由软件测试代替。

## 接线

1. 单板自测：通过板载 ST-LINK 的 Mini-USB CN14 连接电脑并供电、烧录。
   正常板载麦克风已经连好，不接外置麦克风，不插蓝色 Line-In。
2. 串口优先使用 ST-LINK Virtual COM Port。设置 **1152000、8N1、无流控**，注意是
   1152000 而不是 115200。它对应本固件 USART1 PA9/TX、PB7/RX。
   如必须使用课程提供的 3.3 V USB-UART：板 PA9 -> 转换器 RX，GND -> GND；只看日志
   不必接 TX。不要把电脑串口的 RS-232 电平直接接 MCU，也不要误接 Arduino D0/D1
   （那是 USART6）。避免另一个发射端与 ST-LINK 同时驱动 PB7。
3. 双板测试：两块板烧录相同 ELF，各自供电，网线分别连接同一交换机；开发时可先直连。
   在两路串口观察 MASTER/SLAVE、LOCKED_ESTIMATE。单板无网线仍可采集，事件标为 LOCAL。
4. 发声使用手机本身的扬声器，固定在麦克风前方约 0.5~1 m，固定距离、方向、音量。
   不要用蓝牙音箱。麦克风在板背面，勿堵住声孔；标记本固件 channel 0 对应的有效声孔，
   可在低音量下分别靠近两个声孔，比较 STATUS 的 peak 变化确认。
5. 如检查原有同步：示波器两通道分别接两板 Arduino D9/PA15，地夹接各自 GND。
   这里看到的 1 Hz、1 ms 高脉冲是 PPS，不是声音检测脉冲；本版没有增加声音 GPIO 标记。

## 烧录与现象

在项目根目录：

```powershell
cmake --build build/debug -j 8
```

使用已有 CLion/ST-LINK 下载配置烧录 `build/debug/DistributedAcousticRanging.elf`。
保留原有 QSPI 图形资源的烧录方法，首次烧录/擦除外部 Flash 后要同时恢复 TouchGFX 资源。

也可使用配套下载脚本，保证主程序和 QSPI 图片来自同一个最新 ELF：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash_f746.ps1 -SerialNumber 066BFF3834344E5043234409 -FlashType N25Q128A
```

序列号应改为待烧录板子的 ST-LINK 序列号，FlashType 按板载 Flash 选择 `N25Q128A` 或
`W25Q128JVEIQ`。脚本先构建、导出两份镜像、备份内部 Flash 和外部首个 64 KB，再分别下载、
校验并复位；不执行整片擦除或修改选项字节。此命令会真正改写所选板子的固件。

串口会在原有同步日志之间出现类似如下内容（数字仅为格式示例）：

```text
AUDIO INIT: onboard DMIC2 channel=0, 16bit stereo DMA, nominal_fs=48000, warmup=1s
AUDIO STATUS state=WARMUP blocks=... rate=... peak=... noise=... clip=0 drop=0 timing=0 hwerr=0 clock_skip=0 period_ps=20837209
AUDIO STATUS state=ARMED blocks=... rate=187 peak=... noise=... clip=0 drop=0 timing=0 hwerr=0 clock_skip=0 period_ps=20837209
AUDIO EVENT seq=1 arrival=12.345678901 epoch=0 sample=... peak=... noise=... threshold=128 sync=LOCAL clipped=0 interval=NA
```

- `arrival=秒.纳秒` 是本次启动后的本板 PTP 时间，不是 UTC；显示 9 位小数不代表纳秒测量精度。
  48 kHz 下每个采样点约 20.84 us，门限插值也不能消除声学、编解码器或播放设备的误差。
- 从第二次有效检测起，`interval=秒.纳秒` 自动输出本次与上次检测时间戳之差；例如
  `interval=2.000060000` 表示相隔 2.000060000 秒，无需手算。第一条事件、时钟 epoch
  改变或时间戳不递增时输出 `interval=NA`。它是两次被检测事件的时间差，不能证明两次
  实际发声的独立真值；出现漏检、额外触发或削顶时也不能直接用于验收。
- `LOCAL`：本地时钟可读，但尚未确认双板锁定；`LOCKED`：事件发生时软件同步状态已锁定。
  单板第 2 问自测可先使用 LOCAL；跨板比较应等待 LOCKED。
- 安静时 `blocks` 连续增加，`rate` 稳定后通常约 187~188 块/秒；有声时 `peak` 增大。
  每次短音应只有一条 EVENT，之后经过 HOLDOFF 再回 ARMED。单音持续不断时不应反复刷 EVENT。
- `clip` 是该状态报告周期内接近满量程的原始采样数；`clipped=1` 的事件不用于精度评估。
  降低手机音量或适当拉远；不要用过大音量掩盖收音配置问题。
- `drop`、`timing`、`hwerr` 应保持不增长。`clock_skip` 可在初始同步相位调整时增加；
  此时待检测事件被丢弃，时钟稳定后会重新学习噪声、恢复检测。
- `AUDIO RECOVER` 表示 DMA 错误/停止或有效采样长时间中断，任务尝试重新启动 DMA。
  若持续出现、blocks 不增长或 peak 长期为 0，应先解决采集链路，不能进行精度验收。
- 屏幕仍保留原来的 LED 按钮；第 2 问通过串口观察，没有增加距离显示。

## 固定音频的重复性测试

生成自测文件（仅使用 Python 标准库）：

```powershell
python scripts/audio/make_test_audio.py
```

输出 `build/audio-test/arrival_chirp_2s.wav`：开头静音 5 秒，随后 24 个 80 ms、2~6 kHz
短扫频，每 2 秒一次。把文件拷到手机，**完整播放一次**。不要用播放器把单个短音循环播放，
循环间隙和反复启动音频设备会引入额外时间抖动。关闭均衡器、音效、响度均衡，测试时保持安静。

1. 固定手机与板，等 ARMED；双板验收时还应先等同步稳定。
2. 先保存一段静音日志，确认无明显误触发、采样正常。
3. 清空日志并开启保存，完整播放上述文件，预计得到 **24 条 EVENT**。
4. 将一块板的日志保存为 `build/audio-test/board_a.log`，再运行：

```powershell
python scripts/audio/check_arrivals.py build/audio-test/board_a.log --expected-count 24
# 只检查双板已锁定的事件：
python scripts/audio/check_arrivals.py build/audio-test/board_a.log --expected-count 24 --locked
```

脚本要求同一时钟 epoch 至少 11 条事件（10 个间隔），报告间隔中位数、相邻间隔相对中位数
的最大偏差和标准差；默认目标为最大偏差 <=100 us。还检查事件序号、削顶和健康计数。
它不根据串口每行的接收时刻计时。

**两个到达时间戳本身相隔约 2 秒，不可能只差 100 us。** 在固定周期播放条件下，观察的是
重复测量的抖动，而不是把绝对时间戳直接相减后要求 <=100 us。任务书对第 2 问这句的表述
存在歧义，课程正式验收采用的参考/处理方式应以老师口径为准。
该脚本围绕实测中位周期统计抖动，不验证绝对 2 秒周期误差，也不代替课程完整验收。
手机采样时钟、系统音效、扬声器瞬态也会影响结果，测试失败不能只归因于 MCU。

### 如何得到实际发声间隔的参考值

需要区分三个量：

1. 文件中的标称间隔：本测试文件为 96000 帧 / 48000 Hz = 2 秒。
2. 手机实际产生的声音间隔：由手机的实际播放时钟、播放链路和扬声器决定，未经独立测量未知。
3. 本板检测间隔：串口 `interval`，由本板两次采样特征的到达时间戳相减得到。

文件采样间隔准确，不等于手机输出的物理时间间隔也同样准确。[Android 官方音频文档]
(https://developer.android.com/ndk/guides/audio/audio-latency#make-no-assumptions-about-audio-performance)
明确说明实际采样率可能偏离标称采样率。例如假设播放时钟有 50 ppm 偏差，2 秒就会
对应 100 us 的时间偏差；这是误差预算示例，不是对当前手机的实测结论。

如果要独立验证 ±100 us，可使用参考麦克风的模拟音频输出接示波器，固定参考麦克风、
板载麦克风和手机的位置，在一次连续记录中找出相邻两个相同声音特征的时间差，得到
`T_ref`，再与本板对应两条事件的 `interval` 比较。参考测量的采样分辨率、时间基准误差
和波形定位抖动都应显著小于 100 us；不能只根据示波器显示的小数位数判断精度。
参考麦克风模块型号/引脚尚未确认，本文不假定其供电或输出接口；数字麦克风的 SD
引脚不能按模拟 AO 信号连接使用。

现有检测时间戳及其中位间隔不能反过来充当自身的独立参考。中位间隔脚本适合自测抖动，
正式验收还需以课程明确的参考测量方法为准。

移动手机、改音量、更换播放设备都会改变传播延迟或门限触发位置。先完成固定位置测试，
再测试其他条件。正式验收还必须换用课程标准音频记录结果。

## 实现与集成说明

- `arrival_detector.c`：独立于 STM32 的起始检测器。
- `arrival_time.c`：DMA 进度到时间的换算，可独立主机测试。
- `audio_arrival.c`：BSP、DMA IRQ、静态块队列、任务、串口和最新事件快照。
- `AudioArrival_GetLatest()` 提供给将来的 Model/测距任务使用，时间连续性用
  `clock_generation` 区分。旧快照可能属于以前的 epoch，使用者需要核对。
- DMA 总缓冲 512 帧，半缓冲 256 帧（约 5.33 ms）。IRQ 仅采集时钟、复制单通道数据入队；
  检测和日志在任务中完成。队列满、缓冲覆盖或不连续数据均丢弃，重新布防。
- 时间锚点按 IRQ 发生时 NDTR 的进度回推半缓冲边界，再按具体采样索引回算；
  使用实际 SAI 时钟/分频计算采样周期，避免把约 47991 Hz 简单当成精确 48000 Hz。
  SAI FIFO、帧内位置仍带来有界偏差，没有把此方案描述为硬件捕获每个音频采样边沿。
- PTP 相位调整前置 `phase_update` 并更新 epoch，IRQ 在调整期间拒绝采样时间戳。
  串口队列改为同步/音频多任务生产、defaultTask 单消费者，避免多个任务直接抢 UART。
- `.audio_dma` 独立占用 SRAM1 `0x2004B000..0x2004BFFF`，MPU region 3 配为不可缓存、不可执行。
  普通 RAM 从 304KB 改为 300KB，ETHRAM 仍为 `0x2004C000..0x2004FFFF`；保留全局 I/D Cache。
- `App_Init()` 在调度器启动前初始化音频（与触摸共享 I2C3，运行期间不反复写 codec），
  创建 1536 words 静态栈的任务，优先级与现有 GUI 相同、低于 SyncTask。
- SAI 使用独立 PLLI2S，不修改 LCD 使用的 PLLSAI。引入的 HAL SAI 文件与本项目其余 HAL
  均为版本 1.3.3；来源见 `Drivers/AudioHAL/README.md`。
- CubeMX 再生成后检查 `HAL_SAI_MODULE_ENABLED`、main 中的 MPU region 3、App_Init 调用、
  链接脚本和根 CMake。如果以后在 CubeMX 启用 SAI，要避免重复编译 HAL SAI 或重复实现 IRQ。

主机检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File validation/audio/run_tests.ps1
python validation/audio/test_check_arrivals.py
```

包含背景噪声/直流/孤立尖峰、不重复触发、重复短音、确认延迟、跨块索引、DMA 中断延迟回算、
秒边界和错误边界拒绝测试。测试里的 <100 us 是合成信号结果，不是实板测量。
