// 网络质量感知（Network Boost Kit 的网络质量评估、网络场景识别）：系统报告弱信号/拥塞/频繁切换，
// 或预测即将进入弱信号区时，打开“网络预警”，平滑缓冲提前垫到当前档位上限；恢复正常后再保持一会儿才解除，
// 缓冲随后按平时的节奏慢慢缩回。只看 Wi-Fi 链路：经路由器连接时串流走这条链路；
// 专用热点（Wi-Fi 直连）和 USB 不在系统统计的链路里，只记录数据、不触发预警。
// 门限是初值，等真机数据校准：所有信息都写进 stats.csv（net_* 列和 “# net …” 注释行）。
import { netQuality } from '@kit.NetworkBoostKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';
// 预测在多少秒内变弱就提前预警
const PREDICT_LEAD_S = 10;
// 风险消失后预警再保持多久
const HOLD_MS = 15000;
// 自定义判决：Wi-Fi 下行带宽低于这个值（bit/s）算弱网（串流约 6 Mbps，留 2.5 倍余量）
const WEAK_BANDWIDTH_BPS = 15 * netQuality.MBPS;

/** 网络场景编码，写进 stats.csv：-1 未知 / 0 正常 / 1 拥塞 / 2 频繁切换 / 3 弱信号 */
function sceneCode(scene: string): number {
  switch (scene) {
    case 'normal':
      return 0;
    case 'congestion':
      return 1;
    case 'frequentHandover':
      return 2;
    case 'weakSignal':
      return 3;
    default:
      return -1;
  }
}

function isWifi(path: netQuality.PathType): boolean {
  return path === netQuality.PathType.WIFI_PRIMARY || path === netQuality.PathType.WIFI_SECONDARY;
}

export class NetQualityWatcher {
  /** 预警是否生效中 */
  guard: boolean = false;
  private active: boolean = false;
  // 串流是否走 Wi-Fi 链路（经路由器）；热点、USB 时只记录
  private applies: boolean = false;
  private scene: string = '';
  private action: string = '';
  // 预测：何时（本地毫秒）开始变弱、持续多久
  private weakFromMs: number = 0;
  private weakUntilMs: number = 0;
  private bandwidthBps: number = 0;
  private rttMs: number = -1;
  private lastRiskMs: number = 0;
  private reason: string = '';
  private pendingNotes: string[] = [];
  private onScene: (list: Array<netQuality.NetworkScene>) => void = () => {
  };
  private onQos: (list: Array<netQuality.NetworkQos>) => void = () => {
  };

  start(applies: boolean): void {
    this.stop();
    this.applies = applies;
    this.active = true;
    this.onScene = (list: Array<netQuality.NetworkScene>) => {
      this.handleScene(list);
    };
    this.onQos = (list: Array<netQuality.NetworkQos>) => {
      this.handleQos(list);
    };
    try {
      netQuality.on('netSceneChange', this.onScene);
      netQuality.on('netQosChange', this.onQos);
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'netQuality subscribe failed: %{public}d %{public}s', err.code, err.message);
      this.pendingNotes.push(`net unavailable ${err.code}`);
    }
  }

  stop(): void {
    if (!this.active) {
      return;
    }
    this.active = false;
    try {
      netQuality.off('netSceneChange', this.onScene);
      netQuality.off('netQosChange', this.onQos);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'netQuality unsubscribe failed');
    }
    this.guard = false;
    this.scene = '';
    this.action = '';
    this.weakFromMs = 0;
    this.weakUntilMs = 0;
    this.bandwidthBps = 0;
    this.rttMs = -1;
    this.lastRiskMs = 0;
    this.reason = '';
  }

  /** 每秒随统计调用：重新判断预警，返回预警状态是否变了 */
  tick(nowMs: number): boolean {
    if (!this.active) {
      return false;
    }
    let reason = '';
    if (this.scene === 'weakSignal' || this.scene === 'congestion' || this.scene === 'frequentHandover') {
      reason = this.scene;
    } else if (this.weakUntilMs > nowMs && this.weakFromMs - nowMs <= PREDICT_LEAD_S * 1000) {
      reason = 'prediction';
    } else if (this.bandwidthBps > 0 && this.bandwidthBps < WEAK_BANDWIDTH_BPS) {
      reason = 'bandwidth';
    }
    if (reason.length > 0) {
      this.lastRiskMs = nowMs;
      this.reason = reason;
    }
    const guard = this.applies && this.lastRiskMs > 0 && nowMs - this.lastRiskMs < HOLD_MS;
    if (guard === this.guard) {
      return false;
    }
    this.guard = guard;
    this.pendingNotes.push(guard ? `net guard on (${this.reason})` : 'net guard off');
    return true;
  }

  /** 取走待写进 stats.csv 的注释行（场景变化、预测、预警开关） */
  takeNotes(): string[] {
    const notes = this.pendingNotes;
    this.pendingNotes = [];
    return notes;
  }

  /** stats.csv 的 net_* 列：预警、场景、带宽（Mbps）、RTT（ms） */
  csvFields(): number[] {
    return [this.guard ? 1 : 0, sceneCode(this.scene), Math.round(this.bandwidthBps / 1e5) / 10, this.rttMs];
  }

  /** 统计浮层的一行；还没收到任何信息时为空。smoothOn：平滑关闭时预警不起作用 */
  statsText(nowMs: number, smoothOn: boolean): string {
    if (!this.active || (this.scene.length === 0 && this.bandwidthBps <= 0 && this.rttMs < 0)) {
      return '';
    }
    const parts: string[] = [this.sceneLabel()];
    if (this.bandwidthBps > 0) {
      parts.push(tr('net_bandwidth', (this.bandwidthBps / 1e6).toFixed(0)));
    }
    if (this.rttMs >= 0) {
      parts.push(tr('net_rtt', this.rttMs));
    }
    if (this.weakUntilMs > nowMs) {
      parts.push(tr('net_predict', Math.max(0, Math.round((this.weakFromMs - nowMs) / 1000))));
    }
    let text = tr('net_line', parts.join(' · '));
    if (this.guard) {
      text += smoothOn ? tr('net_guard_on') : tr('net_guard_smooth_off');
    } else if (!this.applies) {
      text += tr('net_not_applied');
    }
    return text;
  }

  private sceneLabel(): string {
    switch (this.scene) {
      case 'normal':
        return tr('net_scene_normal');
      case 'congestion':
        return tr('net_scene_congestion');
      case 'frequentHandover':
        return tr('net_scene_handover');
      case 'weakSignal':
        return tr('net_scene_weak');
      default:
        return tr('net_scene_unknown');
    }
  }

  private handleScene(list: Array<netQuality.NetworkScene>): void {
    for (const info of list) {
      if (!isWifi(info.pathType)) {
        continue;
      }
      if (info.scene !== this.scene || info.recommendedAction !== this.action) {
        this.pendingNotes.push(`net scene ${info.scene} action=${info.recommendedAction} path=${info.pathType}`);
      }
      this.scene = info.scene;
      this.action = info.recommendedAction;
      const p = info.weakSignalPrediction;
      if (p !== undefined) {
        if (p.isLastPredictionValid && p.duration > 0) {
          const now = Date.now();
          this.weakFromMs = now + p.startTime * 1000;
          this.weakUntilMs = this.weakFromMs + p.duration * 1000;
          this.pendingNotes.push(`net predict weak in ${p.startTime}s for ${p.duration}s`);
        } else {
          this.weakFromMs = 0;
          this.weakUntilMs = 0;
        }
      }
    }
  }

  private handleQos(list: Array<netQuality.NetworkQos>): void {
    for (const qos of list) {
      if (!isWifi(qos.pathType)) {
        continue;
      }
      this.bandwidthBps = qos.linkDownBandwidth;
      this.rttMs = qos.rttMs;
    }
  }
}
