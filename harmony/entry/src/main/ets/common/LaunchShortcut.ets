// 桌面图标长按弹出的快捷方式（resources/base/profile/shortcuts_config.json）：
// “连接上次”直接连最近一次连上的设备，不开专用热点；“热点+连接”先开专用热点，等游戏机连上来再连接。
// EntryAbility 收到启动参数后记进 AppStorage，首页监听到再执行（冷启动时首页还没建好，要等它建好）。
// 注意：EntryAbility.onCreate 时界面还没建立，这里不能引用 Prefs 等模块——它们一加载就调 PersistentStorage，
// 太早调用读不到已保存的设置。最近连接的查询放在 RecentHosts.lastRecentHost()。
import { Want } from '@kit.AbilityKit';

export const SHORTCUT_KEY = 'launch.shortcut';
// 本次启动来自“连接上次”：不按设置自动开启专用热点
export const NO_AUTO_HOTSPOT_KEY = 'launch.noAutoHotspot';
export const SHORTCUT_CONNECT_LAST = 'connect_last';
export const SHORTCUT_HOTSPOT_CONNECT = 'hotspot_connect';

AppStorage.setOrCreate(SHORTCUT_KEY, '');
AppStorage.setOrCreate(NO_AUTO_HOTSPOT_KEY, false);

/** 记下要执行的快捷方式；值带上时间，同一个快捷方式连着用两次也能触发 */
export function takeShortcut(want: Want, coldStart: boolean): void {
  const params = want.parameters;
  const action = params === undefined ? undefined : params['shortcut'];
  if (action !== SHORTCUT_CONNECT_LAST && action !== SHORTCUT_HOTSPOT_CONNECT) {
    return;
  }
  if (coldStart && action === SHORTCUT_CONNECT_LAST) {
    AppStorage.setOrCreate(NO_AUTO_HOTSPOT_KEY, true);
  }
  AppStorage.setOrCreate(SHORTCUT_KEY, `${action}|${Date.now()}`);
}

export function shortcutAction(raw: string): string {
  return raw.split('|')[0];
}
