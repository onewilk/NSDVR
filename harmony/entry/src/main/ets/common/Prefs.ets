// 设置项与共用常量。设置用 PersistentStorage 持久化，各页面和播放页快捷面板通过 @StorageLink 共用，重启 App 后保留。
import { deviceInfo } from '@kit.BasicServicesKit';
import { LANGUAGE_INDEX_KEY, tr } from './I18n';

// 读出一个旧设置的值，并把它从持久化文件里删掉；没存过时返回 fallback（deleteProp 只能删本次 persistProp 过的键）
function takeLegacyProp(key: string, fallback: number): number {
  PersistentStorage.persistProp(key, fallback);
  const value: number = AppStorage.get<number>(key) ?? fallback;
  PersistentStorage.deleteProp(key);
  return value;
}

PersistentStorage.persistProp('sysdvr.mode', 0);
PersistentStorage.persistProp('sysdvr.enableAudio', true);
// 平滑模式：TCP Bridge、USB 共用一个档位（RTSP 不支持），默认延迟优先。换了新键名：首个版本的 sysdvr.smoothLevel
// 默认关闭，选过延迟优先/流畅优先（> 0）就带过来；为 0 时分不清是默认值还是手动关的，按新默认处理。
// 开发中试过按模式分开保存（.tcp / .usb，没发布过），开发机上沿用 .tcp 的值。
// 旧键读完就删；新键存过之后 persistProp 不再用默认值，所以只会迁移一次
const legacySmoothLevel: number = takeLegacyProp('sysdvr.smoothLevel', 0);
const devSmoothLevel: number = takeLegacyProp('sysdvr.smoothLevel.tcp', -1);
takeLegacyProp('sysdvr.smoothLevel.usb', -1);
const migratedSmoothLevel: number = legacySmoothLevel > 0 ? legacySmoothLevel : devSmoothLevel;
PersistentStorage.persistProp('sysdvr.smoothLevelV2', migratedSmoothLevel >= 0 ? migratedSmoothLevel : 1);
PersistentStorage.persistProp('sysdvr.netOpt', true);
PersistentStorage.persistProp('sysdvr.screenOff', false);
PersistentStorage.persistProp('sysdvr.keepAliveIndex', 2);
// 丢帧时定格画面（等下一个关键帧，不花屏）
PersistentStorage.persistProp('sysdvr.freezeOnLoss', true);
// 画质增强：0 关闭 / 1 标准 / 2 高
PersistentStorage.persistProp('sysdvr.enhanceLevel', 0);
// 统计信息浮层默认关闭（换了新键名：老版本默认开启，升级后也按关闭处理）
PersistentStorage.persistProp('sysdvr.showStatsOverlay', false);
PersistentStorage.persistProp('sysdvr.lastHost', '');
PersistentStorage.persistProp('sysdvr.rtspHost', '');
// Switch 专用热点的名称和密码固定下来，Switch 记住一次后下次自动连上
PersistentStorage.persistProp('sysdvr.hotspotSsid', 'DIRECT-SysDVR');
PersistentStorage.persistProp('sysdvr.hotspotPass', '');
// 打开 App 时自动开启专用热点
PersistentStorage.persistProp('sysdvr.hotspotAuto', false);
// 深色模式：-1 跟随系统 / 0 深色 / 1 浅色（与 ConfigurationConstant.ColorMode 取值一致）
PersistentStorage.persistProp('sysdvr.colorMode', -1);
// 界面语言：LANGUAGE_OPTIONS 的下标（0 跟随系统 / 1 简体中文 / 2 繁體中文 / 3 English）
PersistentStorage.persistProp(LANGUAGE_INDEX_KEY, 0);

export const APP_NAME = 'NSDVR';
export const SYSDVR_GITHUB = 'https://github.com/exelix11/SysDVR';
export const IJKPLAYER_REPO = 'https://gitcode.com/CPF-ApplicationTPC/ohos_ijkplayer';

/** 串流模式，数值与 native 层 connect() 的 mode 一致 */
export enum StreamMode {
  TCP = 0,
  USB = 1,
  RTSP = 2
}

export interface ModeMeta {
  mode: StreamMode;
  title: string;
  subtitle: string;
  desc: string;
  icon: Resource;
}

/** 选项的适用范围说明（在配置页标注） */
export function scopeNotRtsp(): string {
  return tr('app_scope_not_rtsp');
}

export function scopeTcpOnly(): string {
  return tr('app_scope_tcp_only');
}

/** 三种模式的名称和说明（名称是协议名，不翻译） */
export function modes(): ModeMeta[] {
  return [
    {
      mode: StreamMode.TCP, title: 'TCP Bridge', subtitle: tr('app_mode_tcp_sub'), desc: tr('app_mode_tcp_desc'),
      icon: $r('sys.symbol.ohos_wifi')
    },
    {
      mode: StreamMode.USB, title: 'USB', subtitle: tr('app_mode_usb_sub'), desc: tr('app_mode_usb_desc'),
      icon: $r('sys.symbol.charg_cable')
    },
    {
      mode: StreamMode.RTSP, title: 'RTSP', subtitle: tr('app_mode_rtsp_sub'), desc: tr('app_mode_rtsp_desc'),
      icon: $r('sys.symbol.play')
    }
  ];
}

/** 各模式在串流页上方显示的提示 */
export function modeTip(mode: StreamMode): string {
  if (mode === StreamMode.USB) {
    return deviceInfo.deviceType === '2in1' ? tr('app_tip_usb_pc') : tr('app_tip_usb');
  }
  if (mode === StreamMode.RTSP) {
    return tr('app_tip_rtsp');
  }
  return hotspotSupported() ? tr('app_tip_tcp') : tr('app_tip_tcp_nohotspot');
}

// 平滑模式档位：延迟优先（缓冲 ≤80ms，适合玩游戏）、流畅优先（缓冲 ≤300ms，适合观看）
export function smoothLabels(): string[] {
  return [tr('app_off'), tr('app_smooth_latency'), tr('app_smooth_fluent')];
}

// 画质增强档位：标准速度快、延迟低；高效果更好，处理更慢
export function enhanceLabels(): string[] {
  return [tr('app_off'), tr('app_enhance_standard'), tr('app_enhance_high')];
}

// 切到后台后保持连接的时长；到时自动断开，实况窗进度条显示已用比例
export function keepAliveLabels(): string[] {
  return [tr('app_off'), tr('app_minutes', 5), tr('app_minutes', 30), tr('app_hours', 2)];
}

export const KEEP_ALIVE_MINUTES: number[] = [0, 5, 30, 120];

/** 各模式支持哪些选项：RTSP 由 ijkplayer 播放，不走我们的解码管线 */
export function supportsSmooth(mode: StreamMode): boolean {
  return mode !== StreamMode.RTSP;
}

export function supportsNetOpt(mode: StreamMode): boolean {
  return mode === StreamMode.TCP;
}

export function supportsScreenOff(mode: StreamMode): boolean {
  return mode !== StreamMode.RTSP;
}

export function supportsKeepAlive(mode: StreamMode): boolean {
  return mode !== StreamMode.RTSP;
}

// 丢帧定格、画质增强都在我们自己的解码链路里做；RTSP 由 ijkplayer 播放，不支持
export function supportsFreeze(mode: StreamMode): boolean {
  return mode !== StreamMode.RTSP;
}

export function supportsEnhance(mode: StreamMode): boolean {
  return mode !== StreamMode.RTSP;
}

/** 设备能力：部分 PC、平板没有 Wi-Fi 直连或 USB 主机能力，对应功能要隐藏或提示 */
export function hotspotSupported(): boolean {
  return canIUse('SystemCapability.Communication.WiFi.P2P');
}

export function usbHostSupported(): boolean {
  return canIUse('SystemCapability.USB.USBManager');
}

/** 窗口宽度断点（vp）：sm 手机竖屏；md 折叠屏展开、平板竖屏；lg 平板横屏、PC */
export type Breakpoint = 'sm' | 'md' | 'lg';

export function breakpointOf(widthVp: number): Breakpoint {
  if (widthVp < 600) {
    return 'sm';
  }
  return widthVp < 840 ? 'md' : 'lg';
}

export function isValidIPv4(ip: string): boolean {
  const parts = ip.split('.');
  if (parts.length !== 4) {
    return false;
  }
  for (const p of parts) {
    if (!/^\d{1,3}$/.test(p) || Number(p) > 255) {
      return false;
    }
  }
  return true;
}

export function formatDuration(ms: number): string {
  const total = Math.max(0, Math.round(ms / 1000));
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${s < 10 ? '0' : ''}${s}`;
}
