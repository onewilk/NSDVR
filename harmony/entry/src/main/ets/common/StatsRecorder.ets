import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

/**
 * 每秒把统计写进应用沙箱的 files/stats.csv，便于连着电脑用 hdc 拉出来对比不同设置的效果：
 *   hdc file recv -b wilk.sysdvr.next /data/storage/el2/base/files/stats.csv ./
 * 每次连接追加一段，以 “# session” 行分隔；文件超过 4MB 时清空重写。
 * 自动轮换对比时，每次切换组合写一行 “# rotation_switch …”，离开一组时写一行 “# rotation_summary …”（按组合汇总）。
 */
export class StatsRecorder {
  static readonly HEADER = 't_s,bg,recv_fps,show_fps,video_kbps,audio_kbps,dropped,smooth_level,smooth_ms,late,' +
    'audio_on,audio_buf_ms,audio_target_ms,underruns,net_opt,reconnects,resyncs,' +
    // 丢帧（每秒增量）：时间戳断档次数、估计丢帧数、错误包、重放缓存未命中、包头失步、超大包
    'loss_gap,lost_frames,loss_error,loss_replay_miss,loss_resync,loss_oversize,' +
    // 定格：开关、此刻是否定格、次数/时长/超时放弃（增量）；帧间隔、关键帧间隔
    'freeze_on,frozen,freezes,freeze_ms,freeze_giveups,frame_ms,keyframe_ms,' +
    // 上屏：刷新周期、顺延/跳过/卡顿（增量）、最近一秒抖动与最长间隔
    'vsync_ms,vsync_deferred,vsync_dropped,stutters,jitter_ms,max_gap_ms,' +
    // 画质增强实际档位与耗时；专用热点信道、手机 WLAN 信道
    'enhance_level,enhance_ms,hs_channel,sta_channel,' +
    // 实验扩展（官方服务端下 ext=0、其余为默认值）：服务端是否支持；音频编码（-1 官方 PCM / 0 PCM / 1 24 kHz / 2 ADPCM /
    // 3 Opus）、配置码率、Opus 复杂度和帧长；服务端编码耗时（按包累加，us，增量）、客户端解码错误（增量）
    'ext,audio_codec,audio_kbps_cfg,opus_complexity,opus_frame_ms,enc_us,audio_dec_err,' +
    // 诊断包（增量）：窗口时长、发送帧数、grc 断档、视频发送阻塞合计、单次超 20 ms 次数、断档前发送慢的次数、音频包数、
    // 音频编码耗时、音频发送阻塞合计；最近一个窗口的单次阻塞峰值、3 号核心空闲、SysDVR CPU（%，-1 拿不到）、TOS 结果
    'diag_interval_ms,diag_frames,grc_gaps,block_total_ms,sends_over_20ms,gaps_after_slow,diag_audio_pkts,' +
    'diag_enc_us,audio_block_ms,block_max_ms,core3_idle_pct,sysdvr_cpu_pct,tos_flags,' +
    // 自动轮换：当前组合编号（从 1 起，0 = 没在轮换）
    'rotation_combo';

  private file: fileIo.File | null = null;
  private startMs: number = 0;

  start(filesDir: string, note: string): void {
    this.stop();
    const path = `${filesDir}/stats.csv`;
    try {
      let flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.APPEND;
      if (fileIo.accessSync(path) && fileIo.statSync(path).size > 4 * 1024 * 1024) {
        flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      }
      this.file = fileIo.openSync(path, flags);
      this.startMs = Date.now();
      fileIo.writeSync(this.file.fd, `# session ${new Date().toISOString()} ${note}\n${StatsRecorder.HEADER}\n`);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'open stats.csv failed: %{public}s', JSON.stringify(e));
      this.file = null;
    }
  }

  /** 追加一行 “# …” 注释（例如 Switch 内存报告），不影响 CSV 列 */
  note(line: string): void {
    const f = this.file;
    if (f === null) {
      return;
    }
    try {
      fileIo.writeSync(f.fd, `# ${line}\n`);
    } catch (e) {
      this.stop();
    }
  }

  write(fields: number[]): void {
    const f = this.file;
    if (f === null) {
      return;
    }
    const t = ((Date.now() - this.startMs) / 1000).toFixed(0);
    try {
      fileIo.writeSync(f.fd, `${t},${fields.join(',')}\n`);
    } catch (e) {
      this.stop();
    }
  }

  stop(): void {
    const f = this.file;
    if (f !== null) {
      try {
        fileIo.closeSync(f);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'close stats.csv failed');
      }
      this.file = null;
    }
  }
}
