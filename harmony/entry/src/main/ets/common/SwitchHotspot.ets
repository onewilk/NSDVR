import { wifiManager } from '@kit.ConnectivityKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

export interface HotspotState {
  active: boolean;
  ssid: string;
  passphrase: string;
  frequency: number;
  channel: number;
  clients: number;
  goIp: string;
  // 手机是否同时连着别的 Wi-Fi（会和专用热点抢无线芯片，来回切信道导致掉帧）
  staSsid: string;
  staFrequency: number;
  staChannel: number;
  // 最近一次操作的说明（成功/失败原因/尝试过程）
  message: string;
}

// Switch 在 5 GHz 下通常只认 36–48 信道（非 DFS），按顺序尝试
const SWITCH_FREQS_5G: number[] = [5180, 5200, 5220, 5240];

function freqToChannel(f: number): number {
  if (f >= 2412 && f <= 2484) {
    return f === 2484 ? 14 : Math.round((f - 2407) / 5);
  }
  if (f >= 5000 && f <= 5900) {
    return Math.round((f - 5000) / 5);
  }
  return 0;
}

// 20MHz 信道 f 被周边 5 GHz AP 占用的程度：RSSI 换算成线性功率累加；40/80/160MHz 宽信道按实际覆盖范围算重叠
function channelLoad(f: number, aps: wifiManager.WifiScanInfo[]): number {
  let load = 0;
  for (const ap of aps) {
    if (ap.frequency < 5000) {
      continue;
    }
    const width = ap.channelWidth === 1 ? 40 : (ap.channelWidth === 2 ? 80 : (ap.channelWidth === 3 ? 160 : 20));
    const center = width > 20 && ap.centerFrequency0 > 0 ? ap.centerFrequency0 : ap.frequency;
    if (Math.abs(f - center) >= (width + 20) / 2) {
      continue;
    }
    load += Math.pow(10, ap.rssi / 10);
  }
  return load;
}

function sleep(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void) => {
    setTimeout(resolve, ms);
  });
}

/**
 * Switch 专用热点（实验）：系统热点接口（setHotspotConfig 等）只对系统应用开放，改用 Wi-Fi 直连（P2P）
 * 建组——手机作为组主（GO），本质上就是一个 WPA2 的软 AP，普通设备可以当成 Wi-Fi 连接。
 * 优势是能指定频段和频率（goFreq，API 23+），把网络放在 Switch 能识别的 5 GHz 36–48 信道上。
 */
export class SwitchHotspot {
  state: HotspotState = {
    active: false, ssid: '', passphrase: '', frequency: 0, channel: 0, clients: 0, goIp: '', message: '',
    staSsid: '', staFrequency: 0, staChannel: 0
  };

  /**
   * 候选频率的尝试顺序：
   * 1. 手机连着的 Wi-Fi 就在 36–48 信道时，热点建在同一信道——无线芯片不用在两个信道间来回切换；
   * 2. 否则按系统缓存的扫描结果（只需要已声明的 GET_WIFI_INFO，不主动触发扫描、不要定位权限），
   *    周边 AP 占用轻的信道排前面。
   */
  private orderFreqs(): number[] {
    const sta = this.state.staFrequency;
    let aps: wifiManager.WifiScanInfo[] = [];
    try {
      aps = wifiManager.getScanInfoList();
    } catch (e) {
      aps = [];
    }
    const scored = SWITCH_FREQS_5G.map((f: number, i: number): number[] => [f, channelLoad(f, aps), i]);
    scored.sort((a: number[], b: number[]): number => (a[1] - b[1]) || (a[2] - b[2]));
    const ordered = scored.map((x: number[]): number => x[0]);
    if (SWITCH_FREQS_5G.indexOf(sta) >= 0) {
      ordered.splice(ordered.indexOf(sta), 1);
      ordered.unshift(sta);
    }
    hilog.info(DOMAIN, TAG, 'hotspot freq order %{public}s (sta %{public}d, %{public}d APs)', ordered.join(','), sta,
      aps.length);
    return ordered;
  }

  async start(groupName: string, passphrase: string, use5g: boolean): Promise<void> {
    try {
      if (!wifiManager.isWifiActive()) {
        this.state.message = tr('hs_err_wlan_off');
        return;
      }
      // isHotspotActive 从 API 15 开始提供
      if (deviceInfo.sdkApiVersion >= 15 && wifiManager.isHotspotActive()) {
        this.state.message = tr('hs_err_system_hotspot');
        return;
      }
    } catch (e) {
      const err = e as BusinessError;
      this.state.message = tr('hs_err_wlan_state', err.code);
      return;
    }
    await this.stop();

    const canPickFreq = deviceInfo.sdkApiVersion >= 23;
    await this.refreshSta();
    const freqs: number[] = use5g ? (canPickFreq ? this.orderFreqs() : [0]) : [0];
    const tried: string[] = [];
    for (const freq of freqs) {
      const err = await this.createOnce(groupName, passphrase, use5g, freq);
      if (err.length > 0) {
        tried.push(tr('hs_try_item', freq > 0 ? tr('hs_channel_n', freqToChannel(freq)) : tr('hs_channel_auto'), err));
        continue;
      }
      const ch = this.state.channel;
      const ok = !use5g || (ch >= 36 && ch <= 48);
      if (ok) {
        this.state.message = tr('hs_started', use5g ? '5 GHz' : '2.4 GHz', ch) +
          (tried.length > 0 ? tr('hs_tried_before', tried.join(tr('hs_sep'))) : '');
        return;
      }
      // 系统没按要求的频率建组（例如为了和已连接的 Wi-Fi 同信道），换下一个频率再试
      tried.push(tr('hs_channel_mismatch', freqToChannel(freq), ch));
      await this.stop();
    }
    this.state.message = tr('hs_err_no_channel', tried.join(tr('hs_sep')));
  }

  private async createOnce(groupName: string, passphrase: string, use5g: boolean, freq: number): Promise<string> {
    try {
      const local = await wifiManager.getP2pLocalDevice();
      const config: wifiManager.WifiP2PConfig = {
        deviceAddress: local.deviceAddress,
        netId: -1, // 临时组；名称和密码每次都显式指定，Switch 记住后下次可自动连上
        passphrase: passphrase,
        groupName: groupName,
        goBand: use5g ? wifiManager.GroupOwnerBand.GO_BAND_5GHZ : wifiManager.GroupOwnerBand.GO_BAND_2GHZ
      };
      if (freq > 0) {
        config.goFreq = freq;
      }
      wifiManager.createGroup(config);
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'createGroup failed: %{public}d %{public}s', err.code, err.message);
      return tr('hs_err_create', err.code);
    }
    // 建组是异步的，轮询直到拿到组信息（最多约 8 秒）
    for (let i = 0; i < 16; i++) {
      await sleep(500);
      if (await this.refresh()) {
        return '';
      }
    }
    return tr('hs_err_timeout');
  }

  // 读取当前组信息；返回是否存在活动组
  async refresh(): Promise<boolean> {
    try {
      const g = await wifiManager.getCurrentGroup();
      if (!g || !g.groupName) {
        this.state.active = false;
        return false;
      }
      this.state.active = true;
      this.state.ssid = g.groupName;
      this.state.passphrase = g.passphrase;
      this.state.frequency = g.frequency;
      this.state.channel = freqToChannel(g.frequency);
      this.state.clients = g.clientDevices ? g.clientDevices.length : 0;
      this.state.goIp = g.goIpAddress;
      await this.refreshSta();
      return true;
    } catch (e) {
      this.state.active = false;
      return false;
    }
  }

  async refreshSta(): Promise<void> {
    this.state.staSsid = '';
    this.state.staFrequency = 0;
    this.state.staChannel = 0;
    try {
      if (!wifiManager.isConnected()) {
        return;
      }
      const info = await wifiManager.getLinkedInfo();
      this.state.staSsid = info.ssid;
      this.state.staFrequency = info.frequency;
      this.state.staChannel = freqToChannel(info.frequency);
    } catch (e) {
      // 读不到就不提示
    }
  }

  async stop(): Promise<void> {
    this.stopNow();
    await sleep(300);
  }

  /** 同步关闭：App 退出时用，不能再等异步回调 */
  stopNow(): void {
    try {
      wifiManager.removeGroup();
    } catch (e) {
      // 没有组时会报错，忽略
    }
    this.state.active = false;
    this.state.clients = 0;
  }
}
