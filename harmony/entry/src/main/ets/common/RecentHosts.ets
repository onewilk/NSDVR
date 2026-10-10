// 最近连接过的设备（TCP Bridge / RTSP），串流页“最近连接”卡片用，点一下就能重连。
// 只在真正连上之后记录（TCP 收到视频、RTSP 开始播放），连不上的地址不会进来。
import { tr } from './I18n';
import { StreamMode } from './Prefs';

export const RECENT_HOSTS_KEY = 'sysdvr.recentHosts';
// 每种模式最多保留几条
const MAX_PER_MODE = 5;

export interface RecentHost {
  mode: number;
  host: string;
  /** 局域网发现时的名称（如“SysDVR 6.3（协议 03）”）；手动输入的地址为空 */
  name: string;
  /** 最后一次连上的时间（毫秒） */
  at: number;
}

// 存成一行一条“模式|地址|时间|名称”，不用 JSON，免得解析出来的类型不好约束
function clean(s: string): string {
  return s.replace(/[|\n]/g, ' ').trim();
}

export function parseRecentHosts(raw: string): RecentHost[] {
  const out: RecentHost[] = [];
  for (const line of raw.split('\n')) {
    const parts: string[] = line.split('|');
    if (parts.length < 3 || parts[1].length === 0) {
      continue;
    }
    const mode = Number(parts[0]);
    const at = Number(parts[2]);
    if (isNaN(mode) || isNaN(at)) {
      continue;
    }
    out.push({ mode: mode, host: parts[1], at: at, name: parts.slice(3).join(' ') });
  }
  return out;
}

function save(list: RecentHost[]): void {
  const lines: string[] = list.map((h: RecentHost): string => `${h.mode}|${clean(h.host)}|${h.at}|${clean(h.name)}`);
  AppStorage.setOrCreate<string>(RECENT_HOSTS_KEY, lines.join('\n'));
}

function current(): RecentHost[] {
  return parseRecentHosts(AppStorage.get<string>(RECENT_HOSTS_KEY) ?? '');
}

/** 某个模式下的记录，最近的在前 */
export function recentHostsFor(raw: string, mode: StreamMode): RecentHost[] {
  return parseRecentHosts(raw)
    .filter((h: RecentHost): boolean => h.mode === mode)
    .sort((a: RecentHost, b: RecentHost): number => b.at - a.at);
}

/** 连接成功后调用：同一模式下的同一地址只保留最新一条；新记录没有名称时沿用旧名称 */
export function rememberHost(mode: StreamMode, host: string, name: string): void {
  const list = current();
  let oldName = '';
  for (const h of list) {
    if (h.mode === mode && h.host === host) {
      oldName = h.name;
    }
  }
  const rest = list.filter((h: RecentHost): boolean => !(h.mode === mode && h.host === host));
  const sameMode = rest.filter((h: RecentHost): boolean => h.mode === mode).slice(0, MAX_PER_MODE - 1);
  const others = rest.filter((h: RecentHost): boolean => h.mode !== mode);
  const entry: RecentHost = { mode: mode, host: host, name: name.length > 0 ? name : oldName, at: Date.now() };
  save([entry].concat(sameMode).concat(others));
}

/** 最近一次连上的设备（TCP Bridge 和 RTSP 里最新的一条），桌面快捷方式“连接上次”用 */
export function lastRecentHost(): RecentHost | null {
  const list = current().sort((a: RecentHost, b: RecentHost): number => b.at - a.at);
  return list.length > 0 ? list[0] : null;
}

export function forgetHost(mode: number, host: string): void {
  save(current().filter((h: RecentHost): boolean => !(h.mode === mode && h.host === host)));
}

export function clearRecentHosts(mode: number): void {
  save(current().filter((h: RecentHost): boolean => h.mode !== mode));
}

/** “刚刚 / 5 分钟前 / 3 小时前 / 2 天前” */
export function timeAgo(at: number): string {
  const minutes = Math.floor(Math.max(0, Date.now() - at) / 60000);
  if (minutes < 1) {
    return tr('recent_just_now');
  }
  if (minutes < 60) {
    return tr('recent_minutes_ago', minutes);
  }
  const hours = Math.floor(minutes / 60);
  if (hours < 24) {
    return tr('recent_hours_ago', hours);
  }
  return tr('recent_days_ago', Math.floor(hours / 24));
}
