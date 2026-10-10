// libentry.so 的 ArkTS 类型声明，实现见 ../../napi_init.cpp

export interface DeviceInfo {
  ip: string;
  version: string;
  protocol: string;
  serial: string;
}

export interface SessionStats {
  active: boolean;
  /** 在后台：继续接收，不渲染 */
  renderingPaused: boolean;
  message: string;
  videoConnected: boolean;
  audioConnected: boolean;
  videoPackets: number;
  videoBytes: number;
  audioPackets: number;
  audioBytes: number;
  replayHits: number;
  resyncs: number;
  reconnects: number;
  framesRendered: number;
  videoDropped: number;
  /** 最近一次从后台恢复到画面追上最新帧的耗时（ms），-1 表示尚无 */
  lastCatchUpMs: number;
  /** 平滑模式档位（0 关闭 / 1 游戏 / 2 观看）、当前自适应缓冲时长、迟到帧数 */
  smoothLevel: number;
  /** TCP 快速确认是否开启 */
  netOpt: boolean;
  smoothDelayMs: number;
  /** 网络预警是否生效中（缓冲已提前垫到档位上限） */
  netGuard: boolean;
  videoLate: number;
  /** 当前缓存的 GOP（最近一个关键帧起）包数与字节数 */
  gopPackets: number;
  gopBytes: number;
  audioEnabled: boolean;
  audioFastMode: boolean;
  audioUnderruns: number;
  audioBufferedMs: number;
  /** 音频自适应预缓冲目标（ms） */
  audioTargetMs: number;
  decoderError: string;

  /** 丢帧时定格画面是否开启；当前是否正在定格等关键帧 */
  freezeOnLoss: boolean;
  frozen: boolean;
  /** 视频丢失事件，按原因分：时间戳断档（Switch 端丢帧）/ 错误包 / 重放缓存未命中 / 包头失步 / 包超过解码缓冲 */
  lossGap: number;
  lossError: number;
  lossReplayMiss: number;
  lossResync: number;
  lossOversize: number;
  /** 时间戳断档估计丢掉的帧数 */
  lostFrames: number;
  /** 估计的正常帧间隔（us）、关键帧间隔（ms，滑动平均，0 表示还没测到） */
  frameIntervalUs: number;
  keyframeIntervalMs: number;
  /** 定格次数、累计时长、等关键帧超时放弃的次数、最近一次定格时长（-1 表示无） */
  freezeEvents: number;
  freezeTotalMs: number;
  freezeGiveUps: number;
  lastFreezeMs: number;
  /** 屏幕刷新周期（us）；与上一帧同一刷新周期而顺延上屏的帧数；积压太多跳过上屏的帧数 */
  vsyncPeriodUs: number;
  vsyncDeferred: number;
  vsyncDropped: number;
  /** 上屏卡顿次数（间隔明显大于帧间隔），最近一秒的上屏抖动与最长上屏间隔（ms） */
  stutters: number;
  renderJitterMs: number;
  renderMaxGapMs: number;
  /** 画质增强：设置的档位、实际生效的档位（0 = 未生效）、增强耗时（us）、没能生效或出错的原因 */
  enhanceRequested: number;
  enhanceLevel: number;
  enhanceLatencyUs: number;
  enhanceError: string;

  /** Switch 内存报告（协议 03 视频握手时请求，重连后刷新）：是否有效、libnx 结果码，各内存池总量/已用（字节） */
  switchMemValid: boolean;
  switchMemQueryResult: number;
  systemPoolSize: number;
  systemPoolUsed: number;
  appletPoolSize: number;
  appletPoolUsed: number;
  applicationPoolSize: number;
  applicationPoolUsed: number;
  systemUnsafeSize: number;
  systemUnsafeUsed: number;

  /**
   * NSDVR 实验扩展（docs/nsdvr-ext-protocol.md，仅 TCP Bridge）。官方 SysDVR 下 extSupported 为 false，其余保持默认值。
   * extSupported：本次会话见到过扩展音频标记或诊断包。
   * audioCodec：最近一个音频包的编码，-1 官方 PCM（无标记）/ 0 PCM48 / 1 PCM 24 kHz / 2 ADPCM / 3 Opus；
   * audioKbpsConfig：服务端配置的码率；opusComplexity / opusFrameMs：仅 Opus（其他编码为 -1 / 0）。
   * audioEncodeUs：累计的服务端编码耗时（各包 ExtAudioHeader.encodeUs 之和，us）；audioDecodeErrors：客户端解码错误数。
   * extAudioPending：请求的编码还没在包标记上生效；extReq*：最近一次请求的编码与参数
   */
  extSupported: boolean;
  audioCodec: number;
  audioKbpsConfig: number;
  opusComplexity: number;
  opusFrameMs: number;
  audioEncodeUs: number;
  audioDecodeErrors: number;
  extAudioPending: boolean;
  extReqCodec: number;
  extReqKbps: number;
  extReqComplexity: number;
  extReqFrameMs: number;
  /**
   * 诊断包（服务端约每秒一个）。diagPackets 为收到的个数；不带 Total 的是最近一个窗口：窗口时长（ms）、发送帧数、
   * grc 断档、视频发送阻塞合计/最长（us）、单次超过 20 ms 的次数、断档前一次发送慢的次数、音频包数、音频编码耗时合计（us）、
   * 音频发送阻塞合计（us）、3 号核心空闲与 SysDVR CPU（千分比，0xFFFFFFFF 表示拿不到）、TOS 结果（bit0 视频、bit1 音频）。
   * 带 Total 的是会话累计（最长阻塞取最大值），给 CSV 算每秒增量
   */
  diagPackets: number;
  diagIntervalMs: number;
  diagFramesSent: number;
  diagGrcGaps: number;
  diagBlockTotalUs: number;
  diagBlockMaxUs: number;
  diagSendsOver20ms: number;
  diagGapsAfterSlowSend: number;
  diagAudioPackets: number;
  diagAudioEncodeUs: number;
  diagAudioBlockUs: number;
  diagCore3IdlePermille: number;
  diagCpuPermille: number;
  diagTosFlags: number;
  diagTotalIntervalMs: number;
  diagTotalFramesSent: number;
  diagTotalGrcGaps: number;
  diagTotalBlockUs: number;
  diagTotalBlockMaxUs: number;
  diagTotalSendsOver20ms: number;
  diagTotalGapsAfterSlowSend: number;
  diagTotalAudioPackets: number;
  diagTotalAudioEncodeUs: number;
  diagTotalAudioBlockUs: number;
}

/** 开始监听 UDP 19999 上的 SysDVR 广播；端口被占用时返回 false */
export const startDiscovery: () => boolean;
export const stopDiscovery: () => void;
/** 最近 10 秒内广播过的设备 */
export const getDevices: () => DeviceInfo[];
export interface ConnectConfig {
  /** 0 TCP Bridge / 1 USB（RTSP 由 ijkplayer 播放，不走这里） */
  mode: number;
  /** XComponent 的 surfaceId */
  surfaceId: string;
  audio: boolean;
  smoothLevel: number;
  netOpt: boolean;
  /** 串流时关闭 Switch 屏幕（协议 03） */
  screenOff: boolean;
  /** 丢帧时定格画面，等下一个关键帧（默认开） */
  freezeOnLoss?: boolean;
  /** 画质增强：0 关闭 / 1 标准 / 2 高（默认 0） */
  enhanceLevel?: number;
  /** 实验扩展（仅 TCP Bridge，官方 SysDVR 忽略）：是否请求扩展；初始音频编码（0 PCM / 1 24 kHz / 2 ADPCM / 3 Opus）与 Opus 参数 */
  nextExt?: boolean;
  extCodec?: number;
  extOpusKbps?: number;
  extOpusComplexity?: number;
  extOpusFrameMs?: number;
  /** TCP Bridge：Switch 的 IP */
  host?: string;
  /** USB：usbManager.getFileDescriptor() 返回的描述符和接口、端点地址 */
  usbFd?: number;
  usbInterface?: number;
  usbEpIn?: number;
  usbEpOut?: number;
  /** USB：true 表示用 ArkTS bulkTransfer 兼容传输，此时必须提供 onUsbWrite */
  usbPump?: boolean;
  onUsbWrite?: () => void;
}

export interface UsbProbeResult {
  ok: boolean;
  /** 序列号字符串，如 "SysDVR|6.3|03|XAW10000000000" */
  serial: string;
  /** 失败原因（按 setLanguage 设置的语言），只用于显示 */
  error: string;
  /** 系统拒绝直接对 bulk 端点读写（如 EACCES），应改用兼容传输。判断请用这个字段，不要匹配 error 文字 */
  bulkRefused: boolean;
}

/** 连接并开始播放。返回空串表示成功 */
export const connect: (config: ConnectConfig) => string;
/** 在 fd 上 claim 接口并读序列号：成功说明可以直接对 usbfs 发 ioctl */
export const usbProbe: (fd: number, interfaceId: number, epIn?: number) => UsbProbeResult;
/** 兼容传输：把 bulkTransfer 读到的数据交给 native */
export const usbFeed: (data: ArrayBuffer, length: number) => void;
/** 兼容传输：设备断开 */
export const usbFeedError: () => void;
/** 兼容传输：取出待写入 OUT 端点的数据（onUsbWrite 回调里调用） */
export const usbTakeWrite: () => ArrayBuffer | undefined;
/** 兼容传输：回报写入结果（字节数，<0 失败） */
export const usbWriteDone: (result: number) => void;
export const disconnect: () => void;
/** 切后台：保持连接继续接收，释放解码器与音频 */
export const pauseVideo: () => void;
/** 回前台：重建解码器并重放缓存的 GOP，直接显示最新画面。返回空串表示成功 */
export const resumeVideo: (surfaceId: string) => string;
/** 播放中开关音频：TCP Bridge 下断开音频连接让 Switch 停发以省带宽；USB 下本地静音。返回空串表示成功 */
export const setAudioEnabled: (enabled: boolean) => string;
/** 播放中切换平滑档位：0 关闭 / 1 游戏（≤80ms）/ 2 观看（≤300ms） */
export const setSmoothLevel: (level: number) => void;
/** 播放中开关 TCP 快速确认 */
export const setNetOptimization: (enabled: boolean) => void;
/** 播放中开关“丢帧时定格画面” */
export const setFreezeOnLoss: (on: boolean) => void;
/** 网络预警：系统报告弱信号/拥塞或预测即将变差时打开，平滑缓冲提前垫到当前档位上限；平滑关闭时不生效 */
export const setNetworkGuard: (on: boolean) => void;
/** 播放时请求的屏幕刷新率（Hz），0 = 撤销请求、交还系统决定。只是投票，实际刷新率由系统决定 */
export const setRefreshRateHint: (fps: number) => void;
/** 播放中切换画质增强档位（0 关闭 / 1 标准 / 2 高）。返回空串表示成功，否则为原因（已退回直出） */
export const setEnhanceLevel: (level: number) => string;
/**
 * 实验扩展：运行中切换音频编码与 Opus 参数（音频连接上发控制消息，不重连；生效后 getStats 的 audioCodec 等会变）。
 * 返回空串表示已发出（音频关着时已记下，下次打开生效），否则为原因（例如当前服务端不支持）
 */
export const setExtAudio: (codec: number, kbps: number, complexity: number, frameMs: number) => string;
/** 当前已连接的 socket 描述符 */
export const getSocketFds: () => number[];
export const getStats: () => SessionStats;

export interface RtspProbeResult {
  /** TCP 能连上 6666 */
  reachable: boolean;
  /** 拿到了 SDP */
  described: boolean;
  /** PLAY 成功 */
  playing: boolean;
  videoPackets: number;
  audioPackets: number;
  bytes: number;
  /** 失败原因，空串表示正常 */
  error: string;
  /** 逐步记录 */
  log: string;
}

/** RTSP 连接检测：走一遍 OPTIONS/DESCRIBE/SETUP/PLAY，采样 sampleMs 毫秒后发 TEARDOWN 正常结束 */
export const rtspProbe: (host: string, sampleMs: number) => Promise<RtspProbeResult>;

/** 设置 native 层状态和错误文字的语言：0 简体中文 / 1 繁體中文 / 2 English（默认 0）。启动时和切换语言后调用 */
export const setLanguage: (lang: number) => void;
