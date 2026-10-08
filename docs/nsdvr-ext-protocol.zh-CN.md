# NSDVR 实验扩展协议（v1）

[English](nsdvr-ext-protocol.md) | **简体中文**

在 SysDVR 协议 03（TCP Bridge）上做的**向后兼容**扩展，给实验版 SysDVR（[NSDVR-server](https://github.com/onewilk/NSDVR-server) 的 `nsdvr` 分支）和 NSDVR 客户端共用。目的：

1. 音频压缩：PCM24、IMA ADPCM、Opus，客户端可**随时切换**编码和 Opus 参数，不用重连；
2. 服务端诊断：每秒回报发送阻塞、grc 断档、编码耗时、CPU，用来判断丢帧原因；
3. IP_TOS（WMM 视频优先级）标记。

范围：**只用于 TCP Bridge**。USB 和 RTSP 不发扩展标志，行为不变。熄屏逻辑（上游 issue #402）不在本扩展里改，等上游修。

所有多字节整数都是**小端**。

## 1. 握手请求（客户端 → 服务端，16 字节，两路各发一次）

在官方 `ProtoHandshakeRequest` 上扩展：

- `FeatureFlags` bit 2（`0x04`）= `ExtraFeatureFlags_NsdvrExt`：置位时，下面的 `Reserved[0..5]` 按本表解释；不置位时服务端行为与官方完全一致。
- 官方 SysDVR 忽略未知的 FeatureFlags 位和 Reserved 字节（已在上游 `ProtoHandshakeVersion` 中确认：它不检查这些字段）。

| 字节 | 用于 | 含义 |
|---|---|---|
| `Reserved[0]` | 音频路 | 初始音频编码：0 = PCM48（原样），1 = PCM24，2 = ADPCM，3 = OPUS；其他值按 0 处理 |
| `Reserved[1]` | 音频路 | Opus 码率，单位 2 kbps（32 = 64 kbps，48 = 96 kbps）；0 表示默认 96 kbps；服务端限制在 16–256 kbps |
| `Reserved[2]` | 音频路 | 低 4 位：Opus 复杂度 0–10（>10 按 10）；高 4 位：帧长，0 = 20 ms，1 = 10 ms（其他按 20 ms） |
| `Reserved[3]` | 两路 | bit0 = 在本连接上发送诊断包（只对视频路有效）；bit1 = 对本连接的 socket 设置 IP_TOS |
| `Reserved[4..5]` | — | 保留，填 0 |

握手响应不变（72 字节）。

## 2. 服务端能力识别

- 扩展版服务端在客户端请求了扩展标志时，**每个音频包**的 `PacketHeader.ReplaySlot` 写成 `0xE0 | codec`（`0xE0` = PCM48，`0xE1` = PCM24，`0xE2` = ADPCM，`0xE3` = OPUS）。官方服务端对音频一律写 `0xFF`。
- 视频路请求了诊断时，扩展版服务端每秒发一个诊断包（第 4 节）。
- 客户端看到 `0xE0–0xE3` 标记或诊断包，才认为服务端支持扩展、才显示实验选项；否则按官方 PCM 处理。

## 3. 音频包（服务端 → 客户端，9922）

`PacketHeader` 字段含义不变：`Timestamp` 是本包第一个采样的时间（微秒），`MetaData` 为音频类型，`ReplaySlot` 为上面的编码标记。

- **PCM48**（`0xE0`）：负载与官方相同，s16le 立体声交错 48 kHz。
- **压缩编码**（`0xE1–0xE3`）：负载 = 12 字节 `ExtAudioHeader` + 数据体。

`ExtAudioHeader`：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8 | codec（1/2/3） |
| 2 | u8 | 低 4 位 Opus 复杂度，高 4 位帧长代码（非 Opus 填 0） |
| 3 | u8 | frameCount（Opus 帧数；其他编码填 1） |
| 4 | u16 | bitrateKbps（配置值：PCM24 = 768，ADPCM = 384，Opus 为设置的码率） |
| 6 | u16 | samplesPerChannel：本包每声道采样数（按该编码的采样率：PCM24 为 24 kHz，其余 48 kHz） |
| 8 | u32 | encodeUs：服务端编码本包用的时间（微秒，饱和到 0xFFFFFFFF） |

数据体：

- **PCM24**：s16le 立体声交错，24 kHz，`samplesPerChannel` 帧。服务端降采样用 63 抽头半带低通（截止 fs/4，滤波器状态跨包保留），参考 `tools/audio_codec_eval.py` 的 `HALFBAND_63`。客户端升采样回 48 kHz（127 抽头，截止约 11.5 kHz）。
- **ADPCM**：标准 IMA ADPCM（步长表、索引表同 `tools/audio_codec_eval.py`）。先是 2 × 4 字节的声道状态（L 再 R；每个：i16 predictor、u8 stepIndex、u8 保留），表示**本包第一个采样之前**的编码器状态，便于丢包后重新同步；然后 `samplesPerChannel` 字节，每字节一个采样时刻：低 4 位 L、高 4 位 R。
- **OPUS**：`frameCount` 个 `(u16 len, len 字节 Opus 包)`。每帧长度由帧长代码决定（20 ms = 960 采样/声道，10 ms = 480）。服务端用小缓冲把 grc 每次 1024 采样的块重新切成 Opus 帧；包头时间戳 = 第一帧第一个采样的时间。立体声、48 kHz、恒定码率。

补充（服务端实现后确认，2026-09-30）：

- grc 采集失败时的**错误包保持官方格式**：没有 ExtAudioHeader，`ReplaySlot = 0xFF`。客户端不能因为收到一个 `0xFF` 的错误包就认为服务端不支持扩展。
- Opus 只用 CELT（`OPUS_APPLICATION_RESTRICTED_CELT`）、恒定码率：每帧正好 `kbps × 帧采样数 / 384` 字节；编码器延迟 120 采样（2.5 ms）。
- 切换编码类型时，服务端丢弃 Opus 未凑满一帧的尾巴（< 20 ms）；只改码率、复杂度、帧长时保留尾巴，也不重建编码器。
- PCM24 包的时间戳是这批采集第一个输入采样的时间，降采样滤波器约 0.65 ms 的群延迟不补偿。

## 4. 控制消息（客户端 → 服务端，9922 上行，8 字节）

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u32 | magic = `0x58564453`（字节序列 `S D V X`） |
| 4 | u8 | codec（0–3） |
| 5 | u8 | Opus 码率，单位 2 kbps |
| 6 | u8 | 低 4 位复杂度，高 4 位帧长代码 |
| 7 | u8 | 保留 = 0 |

- 服务端音频线程每轮用非阻塞方式读一次；magic 不对就丢弃已读字节，等待下一个对齐的 magic。
- 所有值先校验、夹到合法范围；非法组合一律忽略，**绝不能让异常输入导致 sysmodule 崩溃**。
- 在下一个包的边界生效；切换编码时重置编码器状态。没有应答：客户端从包的编码标记和 `ExtAudioHeader` 确认生效。

## 5. 诊断包（服务端 → 客户端，9911，约每秒一个）

`PacketHeader.MetaData = 0x07`（类型位两位都置位 = 3，再加 Data 位），`ReplaySlot = 0xFF`，`Timestamp` 填最近一帧视频的时间戳。客户端分发器必须**先判断 `(MetaData & 3) == 3`**，否则会被当成视频或音频包。类型 3 官方 sysmodule 未使用；官方 C# 客户端会把它当音频，所以服务端只在客户端请求诊断时才发。

负载 `ExtDiag v1`：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8[3] | 保留 |
| 4 | u32 | intervalMs：本统计窗口实际时长 |
| 8 | u32 | videoFramesSent |
| 12 | u32 | videoGrcGaps：服务端按 grc 时间戳发现的断档次数（间隔 > 1.5 倍正常帧间隔） |
| 16 | u32 | videoSendBlockTotalUs：视频发送阻塞时间合计 |
| 20 | u32 | videoSendBlockMaxUs：单次最长 |
| 24 | u32 | videoSendsOver20ms：单次发送超过 20 ms 的次数 |
| 28 | u32 | gapsAfterSlowSend：断档发生前一次发送超过 20 ms 的次数（直接回答“丢帧是不是发送卡住导致的”） |
| 32 | u32 | audioPackets |
| 36 | u32 | audioEncodeTotalUs |
| 40 | u32 | audioSendBlockTotalUs |
| 44 | u32 | core3IdlePermille：3 号核心空闲千分比（拿不到填 0xFFFFFFFF） |
| 48 | u32 | sysdvrCpuPermille：SysDVR 各线程占用合计千分比（按线程 tick，拿不到填 0xFFFFFFFF） |
| 52 | u32 | tosFlags：bit0 视频 socket 设置成功，bit1 音频 socket 设置成功 |

以后扩展 `ExtDiag` 只能在末尾追加字段、不改已有字段的偏移；客户端接受 version ≥ 1，只读自己认识的前 56 字节。

## 6. IP_TOS

请求了 `Reserved[3]` bit1 时，服务端对该 socket 调用 `setsockopt(IPPROTO_IP, IP_TOS, 0xA0)`（CS5，对应 WMM 视频队列 AC_VI），结果记在诊断包 `tosFlags`。失败不影响串流。

## 7. 兼容性

| 组合 | 行为 |
|---|---|
| 新客户端 + 官方 SysDVR | 音频标记是 `0xFF`、没有诊断包 → 按 PCM 处理，实验选项隐藏 |
| 旧客户端 + 扩展版 SysDVR | 没有扩展标志 → 与官方完全一致 |
| 新客户端 + 扩展版 SysDVR | 按本文档工作 |

## 8. 资源约束（服务端）

- 不用堆（保持 `USE_HEAP 0`），全部静态缓冲；线程栈不够时优先给 libopus 用静态伪栈（`NONTHREADSAFE_PSEUDOSTACK` 或 `VAR_ARRAYS` 以外的方式），并统计最坏情况。
- 交付时报告与官方构建相比 `.text`、`.data/.bss` 的增量，和预计的内存池增加量（Switch 上系统内存池余量有限，测试机只剩约 4 MB）。实测结果：进程内存 +164 KB，明细见服务端仓库的 `host/README.zh-CN.md`。

## 9. 共享产物

- 测试向量：`tools/ext_vectors/`（每种编码约 1 秒：输入 PCM、编码后的音频包、PCM24/ADPCM 的期望解码结果；Opus 附参考解码结果，比较时允许误差）。
- Mac 上的模拟服务器：服务端仓库里的 `host/build/sysdvr_hostmock`，用法见该仓库的 `host/README.zh-CN.md`（默认按本仓库同级目录 `../NSDVR-server` 查找）。它用与 sysmodule 相同的协议和编码 C 代码，推送 H.264 文件和 48 kHz 立体声 WAV，支持扩展协议、控制消息、诊断包，以及人为制造发送卡顿的参数。
