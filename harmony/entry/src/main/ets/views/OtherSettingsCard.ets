import { ConfigurationConstant } from '@kit.AbilityKit';
import { applyAppLanguage, notifyLanguageChanged, tr } from '../common/I18n';
import { applyColorMode } from '../common/Theme';
import { Card, SettingRow } from './Widgets';

// 语言名称用各自语言书写，不随界面语言翻译（方便看不懂当前语言的用户找到自己的语言）
const LANGUAGE_NAMES: string[] = ['', '简体中文', '繁體中文', 'English'];

/** 其他设置：语言、深色模式（跟随系统 / 手动开关） */
@Component
export struct OtherSettingsCard {
  // -1 跟随系统 / 0 深色 / 1 浅色
  @StorageLink('sysdvr.colorMode') colorMode: number = -1;
  // 当前界面是否深色（跟随系统时就是系统的深浅色；由 EntryAbility 在配置变化时更新）
  @StorageProp('ui.dark') dark: boolean = false;
  // LANGUAGE_OPTIONS 的下标：0 跟随系统 / 1 简体中文 / 2 繁體中文 / 3 English
  // 键名同 I18n.LANGUAGE_INDEX_KEY
  @StorageLink('sysdvr.languageIndex') languageIndex: number = 0;

  private set(mode: number): void {
    this.colorMode = mode;
    applyColorMode(getContext(this), mode);
  }

  private languageLabels(): string[] {
    return LANGUAGE_NAMES.map((name: string, i: number) => i === 0 ? tr('app_lang_system') : name);
  }

  private setLanguage(index: number): void {
    if (index === this.languageIndex) {
      return;
    }
    if (!applyAppLanguage(index)) {
      return;
    }
    this.languageIndex = index;
    // 首页按新语言重建，让所有 tr() 重新取词，并回到配置页（热点等全局状态不受影响）
    notifyLanguageChanged();
  }

  build() {
    Card({ title: tr('app_other_title') }) {
      SettingRow({
        title: tr('app_language'),
        desc: tr('app_language_desc'),
        icon: $r('sys.symbol.globe_and_keyboard')
      }) {
        Select(this.languageLabels().map((label: string): SelectOption => {
          const option: SelectOption = { value: label };
          return option;
        }))
          .selected(this.languageIndex)
          .value(this.languageLabels()[this.languageIndex])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.setLanguage(index);
          })
      }

      SettingRow({
        title: tr('app_dark_follow'),
        desc: tr('app_dark_follow_desc'),
        icon: $r('sys.symbol.paintpalette')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET })
          .onChange((isOn: boolean) => {
            // 从“跟随系统”切到手动时，先保持当前看到的效果
            this.set(isOn ? ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET :
              (this.dark ? ConfigurationConstant.ColorMode.COLOR_MODE_DARK :
              ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT));
          })
      }

      SettingRow({
        title: tr('app_dark'),
        desc: this.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET ? tr('app_dark_desc_follow') :
          tr('app_dark_desc_app'),
        icon: $r('sys.symbol.moon')
      }) {
        Toggle({
          type: ToggleType.Switch,
          isOn: this.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET ? this.dark :
            this.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK
        })
          .enabled(this.colorMode !== ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET)
          .onChange((isOn: boolean) => {
            if (this.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET) {
              return;
            }
            this.set(isOn ? ConfigurationConstant.ColorMode.COLOR_MODE_DARK :
              ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT);
          })
      }
    }
  }
}
