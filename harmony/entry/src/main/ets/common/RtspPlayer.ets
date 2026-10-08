import { IjkMediaPlayer, OnErrorListener, OnInfoListener, OnPreparedListener, VideoCodecMode } from '@ohos/ijkplayer';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo } from '@kit.CoreFileKit';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';
// SysDVR 的 RTSP 服务端一次只接一个客户端，断开后要等约 1 秒才重新监听；重连别太急
const RECONNECT_DELAY_MS = 3000;
// ijkplayer 的 setOption 分类
const CAT_FORMAT = '1';
const CAT_CODEC = '2';
const CAT_PLAYER = '4';

function toNumber(v: number | string | undefined | null): number {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

export interface RtspStats {
  playing: boolean;
  decodeFps: number;
  outputFps: number;
  /** 当前接收速率 */
  kbps: number;
  videoCachedMs: number;
  audioCachedMs: number;
  dropFrameRate: number;
  /** 1 软解 / 2 硬解 */
  decoder: number;
  reconnects: number;
  message: string;
}

/**
 * RTSP 模式：SysDVR 在 Switch 上跑一个 RTSP 服务（端口 6666，H.264 + 48kHz 立体声 L16），
 * 这里用 ijkplayer（FFmpeg）拉流播放，参数按“局域网实时游戏”调成低延迟：
 * RTP over TCP、不预缓冲、落后就丢帧、硬件解码直接渲染到 XComponent。
 */
export class RtspPlayer {
  onStatus: (message: string) => void = () => {
  };
  /** 事件日志写到 files/rtsp.log，排查连接问题用（hdc file recv 拉取） */
  logDir: string = '';

  private player: IjkMediaPlayer | null = null;
  private context: object | null = null;
  private xcomponentId: string = '';
  private url: string = '';
  private muted: boolean = false;
  private playing: boolean = false;
  private active: boolean = false;
  private reconnects: number = 0;
  private reconnectTimer: number = -1;
  private message: string = '';
  private failures: number = 0;
  private statsTick: number = 0;
  private openedAt: number = 0;
  private logFile: fileIo.File | null = null;

  static urlOf(host: string): string {
    return `rtsp://${host}:6666/`;
  }

  /** context 来自 XComponent（libraryname: 'ijkplayer_napi'）的 onLoad 回调 */
  start(context: object, xcomponentId: string, host: string, audio: boolean): void {
    this.context = context;
    this.xcomponentId = xcomponentId;
    this.url = RtspPlayer.urlOf(host);
    this.muted = !audio;
    this.active = true;
    this.reconnects = 0;
    this.failures = 0;
    this.openLog();
    this.log(`start ${this.url} audio=${audio}`);
    this.open();
  }

  stop(): void {
    this.active = false;
    if (this.reconnectTimer >= 0) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = -1;
    }
    this.close();
    this.log('stop');
    this.closeLog();
  }

  /** 切后台时停止拉流（surface 不可用），回前台后重新连接 */
  suspend(): void {
    if (this.reconnectTimer >= 0) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = -1;
    }
    this.close();
    this.setMessage(tr('rtsp_paused_background'));
  }

  resume(): void {
    if (this.active && this.player === null) {
      this.open();
    }
  }

  setAudioEnabled(on: boolean): void {
    this.muted = !on;
    const p = this.player;
    if (p !== null) {
      const v = on ? '1.0' : '0.0';
      p.setVolume(v, v);
    }
  }

  getStats(): RtspStats {
    const p = this.player;
    if (p === null) {
      return {
        playing: false, decodeFps: 0, outputFps: 0, kbps: 0, videoCachedMs: 0, audioCachedMs: 0, dropFrameRate: 0,
        decoder: 0, reconnects: this.reconnects, message: this.message
      };
    }
    // 这些接口声明返回 number，native 层实际返回的是字符串，统一转换
    const stats: RtspStats = {
      playing: this.playing,
      decodeFps: toNumber(p.getVideoDecodeFramesPerSecond()),
      outputFps: toNumber(p.getVideoOutputFramesPerSecond()),
      kbps: Math.round(toNumber(p.getTcpSpeed()) * 8 / 1000),
      videoCachedMs: toNumber(p.getVideoCachedDuration()),
      audioCachedMs: toNumber(p.getAudioCachedDuration()),
      dropFrameRate: toNumber(p.getDropFrameRate()),
      decoder: toNumber(p.getVideoDecoder()),
      reconnects: this.reconnects,
      message: this.message
    };
    // 每 5 次（约 5 秒）记一行，便于对照卡顿时刻
    this.statsTick++;
    if (this.statsTick % 5 === 0) {
      this.log(`stats playing=${stats.playing} dec=${stats.decodeFps.toFixed(1)} out=${stats.outputFps.toFixed(1)} ` +
        `kbps=${stats.kbps} vcache=${stats.videoCachedMs} acache=${stats.audioCachedMs} drop=${stats.dropFrameRate} ` +
        `decoder=${stats.decoder}`);
    }
    return stats;
  }

  private open(): void {
    const ctx = this.context;
    if (ctx === null) {
      return;
    }
    const p = new IjkMediaPlayer();
    p.setContext(ctx, this.xcomponentId);
    // SURFACE 模式：硬件解码器直接输出到 XComponent，少一次拷贝和格式转换
    p.native_setup(VideoCodecMode.SURFACE);

    // ---- 协议层：RTP over TCP（Wi-Fi 丢包时比 UDP 稳），不预缓冲，SDP 里已带 SPS/PPS，探测可以很短
    p.setOption(CAT_FORMAT, 'rtsp_transport', 'tcp');
    p.setOption(CAT_FORMAT, 'fflags', 'nobuffer');
    p.setOptionLong(CAT_FORMAT, 'analyzeduration', '500000');
    p.setOptionLong(CAT_FORMAT, 'probesize', '262144');
    p.setOptionLong(CAT_FORMAT, 'flush_packets', '1');
    // socket 读写超时：Switch 在系统界面、游戏加载时会几秒不出画面，超时太短会中途断开，
    // 而这个服务端对“没收到 TEARDOWN 的断开”恢复得很慢，宁可多等
    p.setOptionLong(CAT_FORMAT, 'timeout', '20000000');
    // ---- 播放层：不做包缓冲、准备好就播、视频落后就丢帧追上
    p.setOptionLong(CAT_PLAYER, 'packet-buffering', '0');
    p.setOptionLong(CAT_PLAYER, 'start-on-prepared', '1');
    p.setOptionLong(CAT_PLAYER, 'framedrop', '5');
    p.setOptionLong(CAT_PLAYER, 'min-frames', '2');
    p.setOptionLong(CAT_PLAYER, 'max_cached_duration', '300');
    p.setOptionLong(CAT_PLAYER, 'infbuf', '0');
    p.setOptionLong(CAT_PLAYER, 'mediacodec-all-videos', '1');
    // ---- 解码层：保留环路滤波（画质优先，720p 硬解没有压力）
    p.setOptionLong(CAT_CODEC, 'skip_loop_filter', '0');

    const prepared: OnPreparedListener = {
      onPrepared: () => {
        this.playing = true;
        this.failures = 0;
        this.log(`prepared after ${Date.now() - this.openedAt} ms`);
        this.setMessage(tr('rtsp_playing'));
        if (this.muted) {
          p.setVolume('0.0', '0.0');
        }
      }
    };
    const error: OnErrorListener = {
      onError: (what: number, extra: number) => {
        hilog.warn(DOMAIN, TAG, 'rtsp error what=%{public}d extra=%{public}d', what, extra);
        this.log(`error what=${what} extra=${extra} after ${Date.now() - this.openedAt} ms playing=${this.playing}`);
        this.failures++;
        this.scheduleReconnect(this.playing ? tr('rtsp_interrupted', `${what}/${extra}`) : tr('rtsp_failed', `${what}/${extra}`));
      }
    };
    const info: OnInfoListener = {
      onInfo: (what: number, extra: number) => {
        hilog.info(DOMAIN, TAG, 'rtsp info what=%{public}d extra=%{public}d', what, extra);
        this.log(`info what=${what} extra=${extra}`);
      }
    };
    p.setOnPreparedListener(prepared);
    p.setOnErrorListener(error);
    p.setOnInfoListener(info);
    p.setMessageListener();
    p.setDataSource(this.url);
    p.setScreenOnWhilePlaying(true);
    this.player = p;
    this.playing = false;
    this.openedAt = Date.now();
    this.log('open');
    this.setMessage(this.failures >= 2 ?
      tr('rtsp_retrying', this.failures) : tr('rtsp_connecting', this.url));
    p.prepareAsync();
    p.start();
  }

  private close(): void {
    const p = this.player;
    this.player = null;
    this.playing = false;
    if (p !== null) {
      try {
        p.stop();
        p.release();
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'rtsp release failed: %{public}s', JSON.stringify(e));
      }
    }
  }

  private scheduleReconnect(reason: string): void {
    this.close();
    if (!this.active || this.reconnectTimer >= 0) {
      return;
    }
    this.setMessage(tr('rtsp_reconnect_in', reason, RECONNECT_DELAY_MS / 1000));
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = -1;
      if (this.active) {
        this.reconnects++;
        this.open();
      }
    }, RECONNECT_DELAY_MS);
  }

  private openLog(): void {
    this.closeLog();
    if (this.logDir.length === 0) {
      return;
    }
    try {
      const path = `${this.logDir}/rtsp.log`;
      let flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.APPEND;
      if (fileIo.accessSync(path) && fileIo.statSync(path).size > 1024 * 1024) {
        flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      }
      this.logFile = fileIo.openSync(path, flags);
    } catch (e) {
      this.logFile = null;
    }
  }

  private closeLog(): void {
    const f = this.logFile;
    this.logFile = null;
    if (f !== null) {
      try {
        fileIo.closeSync(f);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'close rtsp.log failed');
      }
    }
  }

  log(line: string): void {
    const f = this.logFile;
    if (f === null) {
      return;
    }
    try {
      fileIo.writeSync(f.fd, `${new Date().toISOString()} ${line}\n`);
    } catch (e) {
      this.closeLog();
    }
  }

  private setMessage(msg: string): void {
    this.message = msg;
    this.onStatus(msg);
  }
}
