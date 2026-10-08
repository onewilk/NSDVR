import { common } from '@kit.AbilityKit';
import { resourceManager, i18n } from '@kit.LocalizationKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

/**
 * 多语言：界面文字都放在 resources/<语言>/element/strings_*.json 里，代码里用 tr('键名', 参数...) 取。
 * - base = English（兜底），zh_CN = 简体中文，zh_TW / zh_HK = 繁體中文（zh_HK 由 tools/i18n_check.py --sync 从 zh_TW 复制）
 * - 占位符用 %s（字符串）、%d（整数）；小数先在代码里格式化成字符串再传 %s
 * - 语言切换：配置 → 其他设置。写入应用偏好语言（系统会记住，冷启动时按它加载资源），
 *   同时让当前进程立即换成新语言，再通知首页重建，让 tr() 重新取值（见 applyAppLanguage / notifyLanguageChanged）
 */

let manager: resourceManager.ResourceManager | null = null;
let appContext: common.ApplicationContext | null = null;

/** 界面语言选项；值同时是 setAppPreferredLanguage 的参数 */
export const LANGUAGE_OPTIONS: string[] = ['default', 'zh-Hans-CN', 'zh-Hant-TW', 'en-Latn-US'];

/** 界面语言设置（LANGUAGE_OPTIONS 的下标）在 AppStorage / PersistentStorage 里的键 */
export const LANGUAGE_INDEX_KEY = 'sysdvr.languageIndex';

/** 界面语言变了（手动切换、或跟随系统时系统语言变了）就加一，首页据此重建 */
export const LANGUAGE_EPOCH_KEY = 'ui.langEpoch';

export function initI18n(ctx: common.UIAbilityContext): void {
  manager = ctx.resourceManager;
  appContext = ctx.getApplicationContext();
}

export function tr(name: string, ...args: (string | number)[]): string {
  if (manager === null) {
    return name;
  }
  try {
    return args.length > 0 ? manager.getStringByNameSync(name, ...args) : manager.getStringByNameSync(name);
  } catch (e) {
    hilog.warn(0x5344, 'SysDVR', 'missing string: %{public}s', name);
    return name;
  }
}

/** 当前的界面语言设置：0 跟随系统 / 1 简体中文 / 2 繁體中文 / 3 English */
export function languageSetting(): number {
  const index = AppStorage.get<number>(LANGUAGE_INDEX_KEY) ?? 0;
  return index > 0 && index < LANGUAGE_OPTIONS.length ? index : 0;
}

/** 是否设置为跟随系统（设置还没从持久化文件读出来时按“不确定”处理，返回 false） */
export function followsSystemLanguage(): boolean {
  return AppStorage.has(LANGUAGE_INDEX_KEY) && languageSetting() === 0;
}

function systemLanguage(): string {
  try {
    return i18n.System.getSystemLanguage();
  } catch (e) {
    return 'zh-Hans';
  }
}

/** 当前生效的界面语言：0 简体 / 1 繁体 / 2 英文（给 native 层的状态文字用，与 tr() 取到的资源一致） */
export function uiLanguageIndex(): number {
  // 按自己记的设置算：跟随系统时直接看系统语言（与 followSystemLanguageNow 设给当前进程的一致）
  const index = languageSetting();
  const tag = index > 0 ? LANGUAGE_OPTIONS[index] : systemLanguage();
  if (!tag.startsWith('zh')) {
    // 其他语言没有翻译，资源回落到 base（英文）
    return 2;
  }
  return /Hant|-TW|-HK|-MO/.test(tag) ? 1 : 0;
}

/**
 * 资源当前实际用的是哪种语言：取一条三种语言写法都不同的文字，首页用它判断要不要重建。
 * （切换应用语言后 ResourceManager.getConfigurationSync().locale 不会跟着变，不能用来判断）
 */
export function languageSignature(): string {
  return tr('app_lang_system');
}

/**
 * 切换界面语言（LANGUAGE_OPTIONS 的下标）。
 * setAppPreferredLanguage 写入的偏好语言会被系统记住，冷启动时按它加载资源；选具体语言时当前进程也会立即切换。
 * 但设成 'default'（跟随系统）只在冷启动时生效，当前进程会停在之前选的语言（华为审核报告的问题），
 * 所以跟随系统时再用 ApplicationContext.setLanguage 把当前进程切到系统语言。
 */
export function applyAppLanguage(index: number): boolean {
  const tag = index > 0 && index < LANGUAGE_OPTIONS.length ? LANGUAGE_OPTIONS[index] : LANGUAGE_OPTIONS[0];
  try {
    i18n.System.setAppPreferredLanguage(tag);
  } catch (e) {
    hilog.warn(0x5344, 'SysDVR', 'setAppPreferredLanguage failed: %{public}s', JSON.stringify(e));
    return false;
  }
  if (tag === LANGUAGE_OPTIONS[0]) {
    followSystemLanguageNow();
  }
  return true;
}

/** 跟随系统时，让当前进程立即使用系统语言（系统语言变化时也调用，见 EntryAbility） */
export function followSystemLanguageNow(): void {
  if (appContext === null) {
    return;
  }
  const lang = systemLanguage();
  try {
    appContext.setLanguage(lang);
  } catch (e) {
    hilog.warn(0x5344, 'SysDVR', 'setLanguage(%{public}s) failed: %{public}s', lang, JSON.stringify(e));
  }
}

/** 通知首页按新语言重建（首页会比对资源语言，没变就不重建） */
export function notifyLanguageChanged(): void {
  AppStorage.setOrCreate(LANGUAGE_EPOCH_KEY, (AppStorage.get<number>(LANGUAGE_EPOCH_KEY) ?? 0) + 1);
}
