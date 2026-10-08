// NSDVR 实验功能（docs/nsdvr-ext-protocol.md，仅 TCP Bridge）：音频编码与 Opus 参数、自动轮换对比。
// 这些选项只在连接过的服务端表明支持扩展时显示（见到扩展音频标记或诊断包，按设备 IP 记住），
// 或者在关于页连点版本号 7 次打开开发者选项后强制显示；官方 SysDVR 用户看不到任何变化。
import { tr } from './I18n';

// 当前选择的编码：0 PCM（原始）/ 1 PCM 24 kHz / 2 IMA ADPCM / 3 Opus；Opus 参数只对 Opus 有效
PersistentStorage.persistProp('sysdvr.extCodec', 0);
PersistentStorage.persistProp('sysdvr.extOpusKbps', 96);
PersistentStorage.persistProp('sysdvr.extOpusComplexity', 5);
PersistentStorage.persistProp('sysdvr.extOpusFrameMs', 20);
// 表明支持扩展的设备（TCP Bridge 的 IP），逗号分隔
PersistentStorage.persistProp('sysdvr.extHosts', '');
// 开发者选项：没有支持扩展的服务端时也显示实验功能
PersistentStorage.persistProp('sysdvr.devMode', false);
// 自动轮换对比：开关、每组时长（ROTATE_MINUTES 的下标）、组合列表（formatPlan 的格式）
PersistentStorage.persistProp('sysdvr.rotateOn', false);
PersistentStorage.persistProp('sysdvr.rotateMinutesIndex', 1);

export enum ExtCodec {
  PCM = 0,
  PCM24 = 1,
  ADPCM = 2,
  OPUS = 3
}

/** 一组编码参数（轮换对比的一项） */
export interface AudioCombo {
  codec: number;
  kbps: number;
  complexity: number;
  frameMs: number;
}

export const OPUS_KBPS_OPTIONS: number[] = [32, 48, 64, 96, 128, 160, 192];
export const OPUS_COMPLEXITY_OPTIONS: number[] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
export const OPUS_FRAME_OPTIONS: number[] = [10, 20];
export const ROTATE_MINUTES: number[] = [1, 2, 5, 10];

// 默认轮换组合：三种无损/轻量编码 + 几档 Opus（码率、复杂度、帧长各有对照）
export const DEFAULT_ROTATE_PLAN = '0:96:5:20;1:96:5:20;2:96:5:20;3:96:5:20;3:64:0:20;3:128:10:10';
PersistentStorage.persistProp('sysdvr.rotatePlan', DEFAULT_ROTATE_PLAN);

export function codecLabels(): string[] {
  return [tr('ext_codec_pcm'), tr('ext_codec_24k'), 'ADPCM', 'Opus'];
}

export function comboLabel(c: AudioCombo): string {
  if (c.codec === ExtCodec.OPUS) {
    return tr('ext_combo_opus', c.kbps, c.complexity, c.frameMs);
  }
  const labels = codecLabels();
  return c.codec >= 0 && c.codec < labels.length ? labels[c.codec] : '—';
}

/** CSV / 日志里用的简短写法，例如 opus/96/5/20 */
export function comboTag(c: AudioCombo): string {
  const names: string[] = ['pcm', '24k', 'adpcm', 'opus'];
  const name = c.codec >= 0 && c.codec < names.length ? names[c.codec] : '?';
  return c.codec === ExtCodec.OPUS ? `${name}/${c.kbps}/${c.complexity}/${c.frameMs}` : name;
}

export function sameCombo(a: AudioCombo, b: AudioCombo): boolean {
  if (a.codec !== b.codec) {
    return false;
  }
  return a.codec !== ExtCodec.OPUS || (a.kbps === b.kbps && a.complexity === b.complexity && a.frameMs === b.frameMs);
}

function clampCombo(c: AudioCombo): AudioCombo {
  const codec = c.codec >= 0 && c.codec <= 3 ? Math.round(c.codec) : 0;
  const kbps = Math.min(192, Math.max(32, Math.round(c.kbps || 96)));
  const complexity = Math.min(10, Math.max(0, Math.round(c.complexity)));
  const frameMs = c.frameMs === 10 ? 10 : 20;
  const out: AudioCombo = { codec: codec, kbps: kbps, complexity: complexity, frameMs: frameMs };
  return out;
}

/** 组合列表的存储格式："编码:码率:复杂度:帧长;…" */
export function parsePlan(s: string): AudioCombo[] {
  const out: AudioCombo[] = [];
  for (const item of s.split(';')) {
    const f = item.split(':').map((v: string) => Number(v));
    if (f.length === 4 && f.every((v: number) => !isNaN(v))) {
      out.push(clampCombo({ codec: f[0], kbps: f[1], complexity: f[2], frameMs: f[3] }));
    }
  }
  return out;
}

export function formatPlan(list: AudioCombo[]): string {
  return list.map((c: AudioCombo) => `${c.codec}:${c.kbps}:${c.complexity}:${c.frameMs}`).join(';');
}

/** 当前设置（sysdvr.extCodec 等）组成的组合 */
export function currentCombo(): AudioCombo {
  return clampCombo({
    codec: AppStorage.get<number>('sysdvr.extCodec') ?? 0,
    kbps: AppStorage.get<number>('sysdvr.extOpusKbps') ?? 96,
    complexity: AppStorage.get<number>('sysdvr.extOpusComplexity') ?? 5,
    frameMs: AppStorage.get<number>('sysdvr.extOpusFrameMs') ?? 20
  });
}

// ---------------------------------------------------------------- 支持扩展的设备

function extHostList(): string[] {
  const s = AppStorage.get<string>('sysdvr.extHosts') ?? '';
  return s.length > 0 ? s.split(',') : [];
}

export function isExtHost(host: string): boolean {
  return host.length > 0 && extHostList().includes(host);
}

/** 记住这台设备支持扩展（最多记 16 台，新的在前） */
export function rememberExtHost(host: string): void {
  if (host.length === 0 || isExtHost(host)) {
    return;
  }
  const list = [host].concat(extHostList()).slice(0, 16);
  AppStorage.setOrCreate('sysdvr.extHosts', list.join(','));
}

// ---------------------------------------------------------------- 自动轮换

/** 一组轮换期间的累计，切到下一组时写一行汇总进 stats.csv */
class ComboTally {
  seconds: number = 0;
  audioBytes: number = 0;
  underruns: number = 0;
  decodeErrors: number = 0;
  lostFrames: number = 0;
  lossEvents: number = 0;
  encodeUs: number = 0;
  diagMs: number = 0;
  diagEncodeUs: number = 0;
  grcGaps: number = 0;
  gapsAfterSlow: number = 0;
  blockUs: number = 0;
  sendsOver20ms: number = 0;
}

/** 每秒一份的增量（Index 的 pollStats 算好传进来） */
export interface RotationSample {
  audioBytes: number;
  underruns: number;
  decodeErrors: number;
  lostFrames: number;
  lossEvents: number;
  encodeUs: number;
  diagMs: number;
  diagEncodeUs: number;
  grcGaps: number;
  gapsAfterSlow: number;
  blockUs: number;
  sendsOver20ms: number;
}

/**
 * 按组合列表依次切换、循环。tick() 每秒调用一次，到点时返回下一组（调用方负责发给 native）。
 * 服务端表明支持扩展之后才开始计时，官方 SysDVR 下什么都不做
 */
export class ExtRotation {
  plan: AudioCombo[] = [];
  durationMs: number = 120000;
  index: number = -1;
  private startedAt: number = 0;
  private tally: ComboTally = new ComboTally();

  running(): boolean {
    return this.index >= 0;
  }

  current(): AudioCombo | null {
    return this.index >= 0 && this.index < this.plan.length ? this.plan[this.index] : null;
  }

  /** 开始轮换，返回第一组 */
  start(plan: AudioCombo[], minutes: number, now: number): AudioCombo | null {
    if (plan.length === 0) {
      this.stop();
      return null;
    }
    this.plan = plan;
    this.durationMs = minutes * 60000;
    this.index = 0;
    this.startedAt = now;
    this.tally = new ComboTally();
    return this.plan[0];
  }

  stop(): void {
    this.index = -1;
  }

  remainingMs(now: number): number {
    return Math.max(0, this.durationMs - (now - this.startedAt));
  }

  add(s: RotationSample): void {
    if (!this.running()) {
      return;
    }
    const t = this.tally;
    t.seconds += 1;
    t.audioBytes += s.audioBytes;
    t.underruns += s.underruns;
    t.decodeErrors += s.decodeErrors;
    t.lostFrames += s.lostFrames;
    t.lossEvents += s.lossEvents;
    t.encodeUs += s.encodeUs;
    t.diagMs += s.diagMs;
    t.diagEncodeUs += s.diagEncodeUs;
    t.grcGaps += s.grcGaps;
    t.gapsAfterSlow += s.gapsAfterSlow;
    t.blockUs += s.blockUs;
    t.sendsOver20ms += s.sendsOver20ms;
  }

  /** 当前这一组的汇总（写进 stats.csv 的注释行） */
  summary(): string {
    const c = this.current();
    const t = this.tally;
    if (c === null) {
      return '';
    }
    const secs = Math.max(1, t.seconds);
    const encPct = t.diagMs > 0 ? t.diagEncodeUs / t.diagMs / 10 : t.encodeUs / secs / 10000;
    return `rotation_summary combo=${this.index + 1}/${this.plan.length} ${comboTag(c)} secs=${t.seconds} ` +
      `audio_kbps=${(t.audioBytes * 8 / 1000 / secs).toFixed(1)} underruns=${t.underruns} ` +
      `decode_errors=${t.decodeErrors} loss_events=${t.lossEvents} lost_frames=${t.lostFrames} ` +
      `enc_pct=${encPct.toFixed(2)} grc_gaps=${t.grcGaps} gaps_after_slow=${t.gapsAfterSlow} ` +
      `block_ms_per_s=${(t.blockUs / 1000 / secs).toFixed(1)} sends_over_20ms=${t.sendsOver20ms}`;
  }

  /** 到点了就切到下一组并返回它；否则返回 null */
  tick(now: number): AudioCombo | null {
    if (!this.running() || now - this.startedAt < this.durationMs) {
      return null;
    }
    this.index = (this.index + 1) % this.plan.length;
    this.startedAt = now;
    this.tally = new ComboTally();
    return this.plan[this.index];
  }
}
