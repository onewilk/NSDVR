import { ConfigurationConstant } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

/**
 * 深色模式与系统栏：
 * - 应用级颜色模式用 ApplicationContext.setColorMode（-1 跟随系统 / 0 深色 / 1 浅色），资源自动切到 dark 目录；
 * - 状态栏、导航条文字颜色随当前深浅色调整，让顶部状态栏和底部小白条区域和页面融为一体。
 */
export function applyColorMode(ctx: Context, mode: number): void {
  try {
    ctx.getApplicationContext().setColorMode(mode as ConfigurationConstant.ColorMode);
  } catch (e) {
    hilog.warn(DOMAIN, TAG, 'setColorMode failed: %{public}s', JSON.stringify(e));
  }
}

export function updateSystemBarColors(win: window.Window, dark: boolean): void {
  const content = dark ? '#FFFFFF' : '#000000';
  win.setWindowSystemBarProperties({
    statusBarColor: '#00000000',
    navigationBarColor: '#00000000',
    statusBarContentColor: content,
    navigationBarContentColor: content
  }).catch((e: Error) => {
    hilog.warn(DOMAIN, TAG, 'setWindowSystemBarProperties failed: %{public}s', JSON.stringify(e));
  });
}

/** 取系统栏安全区高度（vp），存进 AppStorage：ui.safeTop / ui.safeBottom */
export function updateSafeArea(win: window.Window, uiContext: UIContext): void {
  try {
    const top = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM).topRect.height;
    const bottom = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR).bottomRect.height;
    AppStorage.setOrCreate('ui.safeTop', uiContext.px2vp(top));
    AppStorage.setOrCreate('ui.safeBottom', uiContext.px2vp(bottom));
  } catch (e) {
    hilog.warn(DOMAIN, TAG, 'getWindowAvoidArea failed: %{public}s', JSON.stringify(e));
  }
}
