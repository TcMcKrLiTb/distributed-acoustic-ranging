# PCM 串口诊断

当前 `feature/newRanging` 调试版默认启用 `APP_PCM_DUMP=1`（`pcm_dump.h`），
从 `temp/PCM` 恢复串口诊断模块，保留新 TouchGFX 页面设计。PCM 导出不改变声音检测、互相关门限或网口协议。
每次完成供测距使用的 2048 点单通道采集后，两板分别导出自己的副本；包括低相关度等最终被拒绝的测量。
未触发、未完成录制的声音没有片段可导出。两板事件序号可以不同，之后按 PTP 到达时间配对。

本轮先保留 `LONG` 原测距算法作为实际发布结果的来源，同时在完全相同的配对 PCM 上计算
`FRONT` 前沿短窗作诊断对照。`LONG` 使用索引 128～1919，`FRONT` 使用 128～383；
短窗通过不会替代实际结果，也不会更新两板有效测距计数或显示距离。
第一轮实验中 `LONG` 仍可能全部拒绝，此时需要结合 `FRONT` 对照日志和两板 PCM 判断前沿方案是否适用。
不要仅靠降低质量门限或只看成功结果判断改进。前沿窗口来自旧数据的事后分析，必须用新数据验证。

当前算法试验在此基础上做两项修改：

- 保留原有最少 96 点确认、双阈值和形态门限。弱候选满足形态要求但峰值尚不足 `2 * threshold` 时，
  保留最早的门限交点，最多确认 480 点；标称 48 kHz 下为约 2～10 ms。
  延长期间若连续 48 点低于 `threshold / 2`，立即作废该候选，避免把分开的噪声拼成一个事件。
  强音仍按原来的 96 点确认；到达时间取保留的最早交点，不是确认完成时刻。
- LONG 和 FRONT 的搜索范围同时从 -96..96 扩大为 -127..127；两窗口范围、质量 0.65 与竞争峰差 0.02 不变。
  LONG 仍决定正式结果，FRONT 仍只是诊断对照。搜索范围变大不保证每次都更容易通过：新发现的竞争峰也可能让原结果被拒绝。

修改前的可回退快照为 `3363294`（PCM 诊断与新界面均已保存）。后续比较时记录 ELF 对应版本及日志中的
`confirm=96..480`、`lag=-127:127`，避免混用快照版、旧 Release 或旧日志。

## 实板操作

1. 先执行 `cmake --build build/Debug -j 8`，再给两板烧录同一份新生成的
   `build/Debug/DistributedAcousticRanging.elf`。本轮不要使用旧 `build/Release` 文件。
   上电日志应含 `AUDIO INIT ... confirm=96..480` 和 `RANGE DSP ... lag=-127:127`。
2. 第一轮保持原来的布局：B 在左，A 在右，同向摆放，对应有效麦克风声孔间距 700 mm，
   手机放在 B 外侧延长线上，扬声器距 B 的有效拾音孔约 700 mm，复现之前的低相关度条件。
   记录手机到 B 的距离、音量、朝向、两板离桌高度和室温。测试中固定这些条件。
   当前两节点算法得到的是声程差；声源在这条外侧延长线上时，才用 700 mm 作为距离真值。
3. **断开/关闭 MobaXterm 的 COM3 和 COM8 会话**，释放串口。串口不能被两个程序同时打开。
4. 两板同步锁定并完成背景学习后，先单独采集 60 秒安静背景，不播放声音：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\App\Diagnostics\capture_pcm.ps1 -DurationSeconds 60
   ```

   观察是否出现意外的 `AUDIO EVENT`、`AUDIO WARN`、采样恢复或失锁；
   没有声音事件时没有 PCM 文件是正常情况。保存此次目录，不与播放测试混在一起。

5. 安静采集结束后，在项目根目录的 PowerShell 中重新运行默认采集：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\App\Diagnostics\capture_pcm.ps1
   ```

   默认同时读取 COM3（A）和 COM8（B），1152000、8N1、无流控，持续 90 秒。
   看到 `READY: COM3, COM8 ...` 后，确认两板仍同步锁定，再完整播放满幅测试 WAV，
   共 24 次短音、间隔 2 秒。不要等待 60 秒后才播放；安静检查已在上一步完成。
   保持手机位置和音量不动，一次测试只使用一个固定摆放条件，不在播放过程中调音量或移动设备。
   需要换位置时另开一次采集，避免混合两种条件。

   可选：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\App\Diagnostics\capture_pcm.ps1 -DurationSeconds 120 -OutputDirectory .\build\pcm-test-far
   ```

6. 脚本显示声音/测距/对照日志和 `PCM saved: ... CRC OK`，不在终端刷出每行波形数据。
   到时自动退出并打印每个串口的成功片段数与无效/不完整数。
   测试输出位于 `build/pcm-capture/<时间>/`，或指定目录：

   - `COM3.log`、`COM8.log`：未加时间前缀的完整原始串口流，包含 PCM 传输记录。
   - 每个完整片段一个 `.pcm`：4096 字节，2048 点有符号 16 位小端单通道数据。
   - 同名 `.wav`：相同采样点，标称 48 kHz，约 42.7 ms，供试听与工具打开。
   - 同名 `.json`：串口、板号、导出编号、事件序号、epoch、flags、PTP 片段起始时间、
     事件时间、拟合采样周期及 CRC32。纳秒/皮秒时间以十进制字符串保存，避免浮点精度损失。

   示例：`COM3_0001_A_seq1.wav` / `.pcm` / `.json`。主机计数不会因开发板重启归零，避免覆盖旧片段。
   做时间差复算时，**使用 JSON 的 `period_ps` 和 `start_ns`，不能只使用 WAV 的 48000 标称采样率**。
   采样内容是固定 `MIC_CHANNEL` 的原始 PCM，范围包含触发前约 5.3 ms，不是整段 80 ms 测试音。

7. 保留安静与播放两次输出目录，提供双板完整日志及其 PCM/JSON，尤其保留 `LONG`/`FRONT` 对照行。
   只传测距拒绝文字、屏幕截图或 WAV 会缺少时间参数。预期播放测试有 A/B 各 24 个声音事件和完整片段；
   少于或多于 24 个也保留原始数据，不手动删去失败事件。
   第一轮逐项统计双板事件数、完整 PCM 数、实际测距成功/拒绝数和前沿诊断通过数，分别分析误差与拒绝原因。

本轮优先完成下面两组独立测试，每组都按上述流程分开采集静音 60 秒、完整播放 90 秒：

| 测试组 | 布置 | 观察目标 |
| --- | --- | --- |
| 原朝向 B70 | 复现原困难条件：基线 700 mm，手机位于 B 外侧约 700 mm，两板保持原朝向 | 两板是否都得到 24 个事件；`confirm` 是否增长；最早到达时间、片段起点与 LONG/FRONT 候选如何变化 |
| 旋转 B70 | 按之前旋转实验，将两板整体旋转 180°；重新核准有效声孔基线 700 mm，手机仍在 B 外侧约 700 mm | 比较朝向改变后的触发、相关竞争峰与正式结果，不能只比较成功数量 |

每组使用不同输出目录，记录板号与位置、声孔间距、旋转方式、手机扬声器位置和音量。
保持与各自旧实验相同的条件；若旋转后必须挪动板子以恢复声孔基线，也记录位置变化。
先分别比较每组修改前后的新实测结果，再比较两种朝向，不把两组混为同一统计条件。

上述复测取得数据后，再分目录测试手机位于 B 外侧约 300 mm、A 外侧约 700 mm、A 外侧约 300 mm。
每次仍保持 700 mm 基线、相同音量、同一完整测试文件，避免一次同时改变多个条件。
通过这些条件后再扩展到不同基线和音量。校准偏置应在选峰方法稳定后进行。

## LONG / FRONT 同数据对照

启动时输出当前窗口与正式算法来源，窗口 end 不包含在区间内：

```text
RANGE DSP primary=LONG compare=FRONT long=128:1920 front=128:384 lag=-127:127
```

每对完成 DSP 的片段输出两行，以下数值仅为格式示例：

```text
RANGE CMP a=1 b=1 win=LONG error=3 q=437 q2=419 gap=18 lag=12 frac=-7144 dt_ns=-2015992 mm=692
RANGE CMP a=1 b=1 win=FRONT error=0 q=962 q2=930 gap=32 lag=11 frac=11800 dt_ns=-2036858 mm=699 dsp_ms=20
```

- `a/b` 是参与此次 DSP 的 A/B 本地声音事件序号；两板序号不要求相等。
- `error=0` 表示该算法通过全部现有检查，非零使用原 RangeError 定义；
  `LONG` 决定正式 `RANGE RESULT` 或 `RANGE REJECT`，`FRONT` 通过也仅为诊断。
- `q` 是最佳归一化相关分数，`q2` 是距最佳整数位置超过 12 点处的最大竞争分数；
  `q/q2` 将负分数显示为 0；`gap` 使用未截断的两者差值，均按千分值表示。
  门限判断使用内部浮点值，因此不要用舍入后的字段重新判定边界。
- `lag` 是最佳整数位置；`frac` 是 Q16 格式的抛物线小数采样修正，例如 32768 表示半个采样间隔。
- `dt_ns` 是候选原始 `B-A` 时间差，结合片段起始时间与相关峰修正得出；
  `mm` 是候选时间差使用当前声速和偏置换算的距离。两者在算法拒绝时也可能有值，**不是正式测距结果**。
  `dt_ns` 为正对应 A 侧、负对应 B 侧（当前零偏置）。
- 未完成完整相关搜索时 `q/q2/gap/lag/frac` 为 `NA`，无法计算候选时间差时 `dt_ns/mm` 为 `NA`。
- `FRONT` 行末的 `dsp_ms` 是两次算法计算的总经过时间，包括任务被抢占的时间；用于观察增加诊断后的负载。

对照行只在主板完成配对和 DSP 后产生。若某板未触发、片段被提前丢弃或传输超时，就没有这对算法对照。
此时仍需检查两板 `AUDIO EVENT` 和 PCM 文件，区分触发、传输与 DSP 失败，不能仅按对照行数量统计总发声。

主屏新增的 `Q L... F...`、`M L... F...` 分别为本次 LONG/FRONT 的质量和峰间差千分值。
原统计行 `LAST Q` 保留最近成功正式结果的质量，两种含义不要混用。A 板诊断页的 RESULTS 选项可查看
两窗口质量、拒绝原因及候选距离；B 板显示 `DSP ON A`，因为本轮对照在 A 执行，没有通过网口新增诊断同步协议。
SYNC/AUDIO 选项显示真实同步与最近声音事件。失锁或开始新的采集事务会清空此前的候选快照。
其中 AUDIO 提示行的 `CONFIRM N SAMPLES` 为该事件实际确认耗时，单位是采样点；无可显示事件时为 `NO EVENT`。
设置、标定与波形页面的业务交互尚未接入；本轮使用串口导出的完整片段进行波形分析。

## 串口帧和异常

```text
PCM BEGIN v=1 id=1 board=A seq=1 epoch=0 count=2048 fs=48000 start_ns=... onset_ns=... period_ps=... flags=1
PCM DATA id=1 offset=0 hex=...
...
PCM END id=1 crc32=12345678
```

每行 DATA 包含最多 64 个采样点，`offset` 的单位为采样点。CRC32 使用标准 IEEE/zlib 算法，
覆盖全部 4096 个 PCM 小端字节；BEGIN/END 和普通日志不参与校验。允许普通日志穿插在各行之间。
脚本只保存计数、顺序和 CRC 全部正确的完整片段，损坏、不完整和板端中止的帧不生成 WAV。

导出使用独立的两个静态缓冲区，不抢占测距消费者的片段。复制发生在 AudioTask，UART 分块发送发生在
默认后台任务，所有 UART 输出与原日志在该任务中串行执行；没有在音频 ISR 或同步任务中发送波形。
约 9 KB ASCII/片段，在 1152000 波特率下的纯线速时间约 80 ms，实际完成时间受任务调度影响。
队列满时只丢诊断副本，输出 `PCM WARN dropped=N`；UART 发送失败时输出 `PCM ABORT`。
如果出现这些提示，保存日志并检查串口采集是否正常、发声是否过密。

也可继续用 MobaXterm 保存原始文本，再在 PowerShell 中离线解码（日志不能加每行时间前缀）：

```powershell
& .\App\Diagnostics\capture_pcm.ps1 -LogFiles @('D:\Desktop\A.log','D:\Desktop\B.log') -OutputDirectory .\build\pcm-offline
```

结束诊断后将 `APP_PCM_DUMP` 改为 `0` 并重新构建，即可关闭此导出。
波形页面如需显示真实 PCM，应使用独立快照；不要从 GUI 调用消耗式
`AudioArrival_TakeCapture()`，以免抢走测距模块需要的片段。

## 主机验证

`test_pcm_dump.py` 编译真实的 C 编码模块（HAL/RTOS 用主机桩替代），把输出交给 Windows PowerShell 解码。
检查双缓冲满时的丢弃、生产者不发送 UART、每轮最多一块、16 位极值及字节序、64 位时间精度、
独立 zlib CRC 校验、日志穿插、`RANGE DSP/CMP` 对照行在控制台可见、坏校验、丢块、截断和 UART 中止恢复。
需要 Python 3、原生 GCC、PowerShell，
不依赖 Python 第三方包或真实串口。所有测试文件放在 `build/pcm-validation/`。

```powershell
python .\App\Diagnostics\test_pcm_dump.py --cc gcc
```

测距核心和状态机的独立验证：

```powershell
python .\App\Ranging\test_range_diagnostics.py --cc gcc
python .\App\Ranging\test_ranging_flow.py --cc gcc
```

算法修正新增的确认、时间回填、录制与五组录音回归：

```powershell
python .\App\AudioArrival\test_arrival_detector.py --cc gcc
python .\App\AudioArrival\test_arrival_capture.py --cc gcc
python .\App\Ranging\test_algorithm_replay.py --cc gcc
python .\App\Ranging\test_onset_replay.py --cc gcc
```

`test_onset_replay.py` 仅在 LONG 全部实际读取的样本均来自旧录音时进行条件性对照；
缺失的完整片段前后缀、flags和连续性无法靠它重验，因此输出不能当作新固件的实板成功率。
在当前原朝向 B70 录音中，条件性 LONG 对照可通过17对、约691～692 mm，仍存在低相关和歧义拒绝；
未改增益、声速、偏置或质量门限，也没有使用 FRONT 自动兜底。新固件必须重新实测。

前者检查窗口、候选有效性、方向与偏置，并使用固定旧版作同搜索范围下的行为对照；
后者驱动实际 META/DATA/ACK 和显示时序，检查 FRONT 通过不增加正式成功计数、失锁作废及日志格式。
本地保留的10月23日原始 PCM 可进一步重放真实数据；这些实测片段位于 build，未纳入源码版本。

固定旧片段的搜索范围回归可运行：

```powershell
python .\App\Ranging\test_algorithm_replay.py --cc gcc --baseline-ref 3363294
```

当前归档的五组主要数据是一组 10 月 23 日录音，加上四组 10 月 25 日录音：

| 目录 | 旧实验条件 |
| --- | --- |
| `20261023_214834_384` | 早期 B 侧 700 mm 音源位置 |
| `20261025_154131_136` | 原朝向 B 侧 700 mm 困难条件 |
| `20261025_155918_062` | A 侧 700 mm 音源位置 |
| `20261025_161041_655` | B 侧 300 mm 音源位置，包含播放后额外事件 |
| `20261025_172902_517` | 两板整体旋转 180°后的 B 侧 700 mm 音源位置 |

这里的音源位置距离指手机到近端板，基线仍按每组实验记录解释。这些目录位于 `build/pcm-capture/`。
重放使用当时已经捕获的原始片段及时间元数据，只能检验扩大搜索范围等算法对**固定片段**的行为。
它不能证明延长确认后，真实事件会在何时完成、最早交点会如何变化，或新采集窗口会有多少改进。
若尝试按新触发点重新对齐旧片段，缺少的前缀或后缀必须标为缺失；**不能补零并将其当作真实采样**，
也不能据此声称完成了实板验证。四组或五组旧数据的回放通过，仍须用上述两组新实测确认整体效果。

编译和主机验证不能代替实板检查。采集时仍应观察是否新增 `AUDIO WARN`、断链或失锁，
确认额外的串口负载未破坏原来的实时任务。
