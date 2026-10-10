import { AbilityConstant, Configuration, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError, commonEventManager } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { HotspotService } from '../common/HotspotService';
import { followSystemLanguageNow, followsSystemLanguage, initI18n, notifyLanguageChanged } from '../common/I18n';
import { takeShortcut } from '../common/LaunchShortcut';
import { updateSafeArea, updateSystemBarColors } from '../common/Theme';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | null = null;
  // 当前进程的界面语言（配置里的 language），变了才通知首页重建
  private language: string = '';
  private localeSubscriber: commonEventManager.CommonEventSubscriber | null = null;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'Ability onCreate');
    // 多语言：tr() 通过它按当前（或应用偏好）语言取字符串资源
    initI18n(this.context);
    this.language = this.context.config.language ?? '';
    AppStorage.setOrCreate('ui.dark', this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    this.watchSystemLocale();
    // 从桌面快捷方式启动：首页建好后执行
    takeShortcut(want, true);
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // App 已在运行（包括在后台）时点了快捷方式
    takeShortcut(want, false);
  }

  /**
   * 跟随系统时，系统语言在 App 运行中变了也要跟着变：切回“跟随系统”时用 ApplicationContext.setLanguage
   * 指定过当前进程的语言，之后系统语言的变化不一定还会传过来，所以监听系统语言变化事件，重新按系统语言设置一次
   */
  private watchSystemLocale(): void {
    commonEventManager.createSubscriber({ events: [commonEventManager.Support.COMMON_EVENT_LOCALE_CHANGED] })
      .then((subscriber: commonEventManager.CommonEventSubscriber) => {
        this.localeSubscriber = subscriber;
        commonEventManager.subscribe(subscriber, (err: BusinessError) => {
          if (err) {
            hilog.warn(DOMAIN, TAG, 'locale event failed: %{public}s', JSON.stringify(err));
            return;
          }
          const follow = followsSystemLanguage();
          hilog.info(DOMAIN, TAG, 'system locale changed, follow=%{public}s', follow ? 'yes' : 'no');
          if (follow) {
            followSystemLanguageNow();
          }
          notifyLanguageChanged();
        });
      })
      .catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'createSubscriber failed: %{public}s', JSON.stringify(e));
      });
  }

  onConfigurationUpdate(config: Configuration): void {
    // 系统切换深浅色 / 语言、或应用调用 setColorMode / 切换界面语言后都会走到这里
    const dark = config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    AppStorage.setOrCreate('ui.dark', dark);
    if (this.mainWindow !== null) {
      updateSystemBarColors(this.mainWindow, dark);
    }
    // 界面语言变了（切换语言、或跟随系统时系统语言变了）：资源已经换好，通知首页重建让 tr() 重新取词
    if (config.language !== undefined && config.language !== this.language) {
      hilog.info(DOMAIN, TAG, 'language config: %{public}s -> %{public}s', this.language, config.language);
      this.language = config.language;
      notifyLanguageChanged();
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      try {
        const win = windowStage.getMainWindowSync();
        this.mainWindow = win;
        updateSystemBarColors(win, AppStorage.get<boolean>('ui.dark') === true);
        const uiContext = win.getUIContext();
        updateSafeArea(win, uiContext);
        // 旋转、折叠屏开合、进出全屏时安全区会变
        win.on('avoidAreaChange', () => {
          updateSafeArea(win, uiContext);
        });
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'window setup failed: %{public}s', JSON.stringify(e));
      }
    });
  }

  onWindowStageDestroy(): void {
    // 退出应用时关掉专用热点（切到后台不会走到这里）
    HotspotService.get().shutdown();
  }

  onDestroy(): void {
    HotspotService.get().shutdown();
    if (this.localeSubscriber !== null) {
      commonEventManager.unsubscribe(this.localeSubscriber);
      this.localeSubscriber = null;
    }
  }
}
