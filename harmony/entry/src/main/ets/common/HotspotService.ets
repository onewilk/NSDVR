import { promptAction } from '@kit.ArkUI';
import { tr } from './I18n';
import { NO_AUTO_HOTSPOT_KEY } from './LaunchShortcut';
import { SwitchHotspot } from './SwitchHotspot';

/**
 * Switch 专用热点的全局控制器。热点由系统维护，App 被杀后仍在，所以状态不能放在某个页面组件里：
 * 这里统一负责开关、定时刷新、启动时同步和“打开 App 自动开启”，界面通过 AppStorage 读取状态。
 *
 * AppStorage 键：
 *   hs.on / hs.busy       开关状态、是否正在操作
 *   hs.summary            卡片上的一行摘要（不含网络名和密码）
 *   hs.detail             配置弹窗里的详细信息（网络名、密码、频段等）
 *   hs.warn               手机同时连着别的 Wi-Fi、且与热点不在同一信道时的警告
 *   hs.channel            热点信道（未开启为 0）
 *   hs.staChannel         手机 WLAN 所连网络的信道（没连为 0）
 */
export function randomPassphrase(): string {
  const chars = 'abcdefghjkmnpqrstuvwxyz23456789'; // 去掉容易看错的 0/o、1/l/i
  let s = '';
  for (let i = 0; i < 10; i++) {
    s += chars.charAt(Math.floor(Math.random() * chars.length));
  }
  return s;
}

// Wi-Fi 网络名最长 32 字节（UTF-8，一个中文 3 字节）
function utf8Length(str: string): number {
  let n = 0;
  for (let i = 0; i < str.length; i++) {
    const c = str.charCodeAt(i);
    if (c < 0x80) {
      n += 1;
    } else if (c < 0x800) {
      n += 2;
    } else if (c >= 0xD800 && c <= 0xDBFF) {
      n += 4; // 代理对（如表情符号），跳过低位
      i++;
    } else {
      n += 3;
    }
  }
  return n;
}

export function checkHotspotConfig(ssid: string, pass: string): string {
  if (ssid.length === 0) {
    return tr('hs_err_ssid_empty');
  }
  if (utf8Length(ssid) > 32) {
    return tr('hs_err_ssid_long');
  }
  if (!/^[\x20-\x7E]{8,63}$/.test(pass)) {
    return tr('hs_err_pass');
  }
  return '';
}

AppStorage.setOrCreate('hs.on', false);
AppStorage.setOrCreate('hs.busy', false);
AppStorage.setOrCreate('hs.summary', '');
AppStorage.setOrCreate('hs.detail', '');
AppStorage.setOrCreate('hs.warn', '');
AppStorage.setOrCreate('hs.channel', 0);
AppStorage.setOrCreate('hs.staChannel', 0);
// 专用热点（群组主）的 IP：判断游戏机是否连在热点上；已连上的设备数
AppStorage.setOrCreate('hs.goIp', '');
AppStorage.setOrCreate('hs.clients', 0);

export class HotspotService {
  private static instance: HotspotService | null = null;
  private hotspot: SwitchHotspot = new SwitchHotspot();
  private timer: number = -1;
  private initialized: boolean = false;

  static get(): HotspotService {
    if (HotspotService.instance === null) {
      HotspotService.instance = new HotspotService();
    }
    return HotspotService.instance;
  }

  /** App 启动时调用一次：同步系统里还在运行的热点；没有的话按设置自动开启 */
  async init(): Promise<void> {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    if (!canIUse('SystemCapability.Communication.WiFi.P2P')) {
      return; // 本设备没有 Wi-Fi 直连（部分 PC），专用热点功能整体隐藏
    }
    const alive = await this.hotspot.refresh();
    if (alive) {
      this.hotspot.state.message = tr('hs_still_running');
      this.setOn(true);
      this.publish();
      return;
    }
    // 从“连接上次”快捷方式启动时不自动开启（用户要的就是不开热点直接连）
    if (AppStorage.get<boolean>('sysdvr.hotspotAuto') === true && AppStorage.get<boolean>(NO_AUTO_HOTSPOT_KEY) !== true) {
      // 自动开启的结果要告诉用户：成功时卡片不一定在眼前，失败更要说明原因
      const err = await this.start();
      promptAction.showToast({
        message: err.length > 0 ? tr('hs_auto_failed', err) : tr('hs_auto_started'),
        duration: 3000
      });
    }
  }

  /**
   * App 真正退出（从最近任务划掉、被系统销毁）时关掉专用热点：Wi-Fi 直连组由系统维护，
   * 不关的话 App 没了热点还一直开着，耗电、占着 Wi-Fi，也还能被连上。切到后台不走这里，热点保留。
   */
  shutdown(): void {
    if (!this.isOn()) {
      return;
    }
    this.hotspot.stopNow();
    this.setOn(false);
  }

  isOn(): boolean {
    return AppStorage.get<boolean>('hs.on') === true;
  }

  async start(): Promise<string> {
    if (AppStorage.get<boolean>('hs.busy') === true || this.isOn()) {
      return '';
    }
    let ssid = (AppStorage.get<string>('sysdvr.hotspotSsid') ?? 'DIRECT-SysDVR').trim();
    let pass = AppStorage.get<string>('sysdvr.hotspotPass') ?? '';
    if (ssid.length === 0) {
      ssid = 'DIRECT-SysDVR';
    }
    if (pass.length === 0) {
      pass = randomPassphrase();
    }
    AppStorage.set('sysdvr.hotspotSsid', ssid);
    AppStorage.set('sysdvr.hotspotPass', pass);
    const invalid = checkHotspotConfig(ssid, pass);
    if (invalid.length > 0) {
      AppStorage.set('hs.summary', invalid);
      return invalid;
    }
    AppStorage.set('hs.busy', true);
    AppStorage.set('hs.summary', tr('hs_creating'));
    await this.hotspot.start(ssid, pass, true);
    if (!this.hotspot.state.active) {
      // 5 GHz 建不起来（设备不支持或信道受限），退回 2.4 GHz，至少仍是手机与 Switch 直连
      const reason5g = this.hotspot.state.message;
      await this.hotspot.start(ssid, pass, false);
      this.hotspot.state.message = tr('hs_fallback_24', reason5g) +
        (this.hotspot.state.active ? '' : this.hotspot.state.message);
    }
    this.setOn(this.hotspot.state.active);
    AppStorage.set('hs.busy', false);
    this.publish();
    return this.hotspot.state.active ? '' : this.hotspot.state.message;
  }

  async stop(): Promise<void> {
    if (AppStorage.get<boolean>('hs.busy') === true) {
      return;
    }
    AppStorage.set('hs.busy', true);
    await this.hotspot.stop();
    this.hotspot.state.message = tr('hs_closed');
    this.setOn(false);
    AppStorage.set('hs.busy', false);
    this.publish();
  }

  /** 改了网络名或密码：开着的话按新配置重建（Switch 需要重新连接） */
  async restart(): Promise<string> {
    if (!this.isOn()) {
      return '';
    }
    await this.stop();
    return await this.start();
  }

  private setOn(on: boolean): void {
    AppStorage.set('hs.on', on);
    if (on && this.timer < 0) {
      this.timer = setInterval(() => {
        this.refresh();
      }, 2000);
    } else if (!on && this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private async refresh(): Promise<void> {
    if (AppStorage.get<boolean>('hs.busy') === true) {
      return;
    }
    const alive = await this.hotspot.refresh();
    if (!alive && this.isOn()) {
      // 被系统或其他应用收回了（例如开了系统热点、关了 WLAN）
      this.hotspot.state.message = tr('hs_reclaimed');
      this.setOn(false);
    }
    this.publish();
  }

  private publish(): void {
    const st = this.hotspot.state;
    AppStorage.set('hs.channel', st.active ? st.channel : 0);
    AppStorage.set('hs.staChannel', st.staSsid.length > 0 ? st.staChannel : 0);
    AppStorage.set('hs.goIp', st.active ? st.goIp : '');
    AppStorage.set('hs.clients', st.active ? st.clients : 0);
    if (!st.active) {
      AppStorage.set('hs.summary', st.message);
      AppStorage.set('hs.detail', '');
      AppStorage.set('hs.warn', '');
      return;
    }
    const band = st.frequency >= 5000 ? '5 GHz' : '2.4 GHz';
    AppStorage.set('hs.summary', tr('hs_summary_on', band, st.channel, st.clients));
    AppStorage.set('hs.detail', tr('hs_detail_band', band, st.channel, st.frequency) + '\n' +
      tr('hs_detail_clients', st.clients) + (st.goIp ? tr('hs_detail_ip', st.goIp) : '') + '\n' +
      tr('hs_detail_switch_steps'));
    if (st.staSsid.length > 0 && st.staChannel !== st.channel) {
      // 同一信道时芯片不用来回切换，不提示；不同信道（含 2.4 GHz）才会周期性掉帧
      const staBand = st.staFrequency >= 5000 ? '5 GHz' : '2.4 GHz';
      AppStorage.set('hs.warn', tr('hs_warn_sta', st.staSsid, staBand, st.staChannel, st.channel));
    } else {
      AppStorage.set('hs.warn', '');
    }
  }
}
