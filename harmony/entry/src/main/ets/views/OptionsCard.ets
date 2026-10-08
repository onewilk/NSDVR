import { tr } from '../common/I18n';
import { enhanceLabels, keepAliveLabels, scopeNotRtsp, scopeTcpOnly, smoothLabels } from '../common/Prefs';
import { Card, SettingRow } from './Widgets';

function selectOptions(labels: string[]): SelectOption[] {
  return labels.map((label: string): SelectOption => {
    const option: SelectOption = { value: label };
    return option;
  });
}

/** 串流选项：所有选项都可以设置，标签注明适用范围；播放中也可以在播放页“设置”里即时修改 */
@Component
export struct OptionsCard {
  @StorageLink('sysdvr.enableAudio') enableAudio: boolean = true;
  @StorageLink('sysdvr.smoothLevelV2') smoothLevel: number = 1;
  @StorageLink('sysdvr.netOpt') netOpt: boolean = true;
  @StorageLink('sysdvr.screenOff') screenOff: boolean = false;
  @StorageLink('sysdvr.keepAliveIndex') keepAliveIndex: number = 2;
  @StorageLink('sysdvr.showStatsOverlay') showStats: boolean = false;
  @StorageLink('sysdvr.freezeOnLoss') freezeOnLoss: boolean = true;
  @StorageLink('sysdvr.enhanceLevel') enhanceLevel: number = 0;

  build() {
    Card({ title: tr('cfg_opt_title'), subtitle: tr('cfg_opt_subtitle') }) {
      SettingRow({
        title: tr('cfg_opt_audio'),
        desc: tr('cfg_opt_audio_desc'),
        icon: $r('sys.symbol.speaker_fill')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.enableAudio })
          .onChange((isOn: boolean) => {
            this.enableAudio = isOn;
          })
      }

      SettingRow({
        title: tr('cfg_opt_smooth'),
        desc: tr('cfg_opt_smooth_desc'),
        scope: scopeNotRtsp(),
        icon: $r('sys.symbol.slider_horizontal_2')
      }) {
        Select(selectOptions(smoothLabels()))
          .selected(this.smoothLevel)
          .value(smoothLabels()[this.smoothLevel])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.smoothLevel = index;
          })
      }

      SettingRow({
        title: tr('cfg_opt_freeze'),
        desc: tr('cfg_opt_freeze_desc'),
        scope: scopeNotRtsp(),
        icon: $r('sys.symbol.pause_circle')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.freezeOnLoss })
          .onChange((isOn: boolean) => {
            this.freezeOnLoss = isOn;
          })
      }

      SettingRow({
        title: tr('cfg_opt_enhance'),
        desc: tr('cfg_opt_enhance_desc'),
        scope: scopeNotRtsp(),
        icon: $r('sys.symbol.wand_and_stars')
      }) {
        Select(selectOptions(enhanceLabels()))
          .selected(this.enhanceLevel)
          .value(enhanceLabels()[this.enhanceLevel])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.enhanceLevel = index;
          })
      }

      SettingRow({
        title: tr('cfg_opt_netopt'),
        desc: tr('cfg_opt_netopt_desc'),
        scope: scopeTcpOnly(),
        icon: $r('sys.symbol.network_acceleration')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.netOpt })
          .onChange((isOn: boolean) => {
            this.netOpt = isOn;
          })
      }

      SettingRow({
        title: tr('cfg_opt_screen_off'),
        desc: tr('cfg_opt_screen_off_desc'),
        scope: scopeNotRtsp(),
        icon: $r('sys.symbol.screen_off_display')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.screenOff })
          .onChange((isOn: boolean) => {
            this.screenOff = isOn;
          })
      }

      SettingRow({
        title: tr('cfg_opt_keep_alive'),
        desc: tr('cfg_opt_keep_alive_desc'),
        scope: scopeNotRtsp(),
        icon: $r('sys.symbol.stopwatch')
      }) {
        Select(selectOptions(keepAliveLabels()))
          .selected(this.keepAliveIndex)
          .value(keepAliveLabels()[this.keepAliveIndex])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.keepAliveIndex = index;
          })
      }

      SettingRow({
        title: tr('cfg_opt_stats'),
        desc: tr('cfg_opt_stats_desc'),
        icon: $r('sys.symbol.list_bullet')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.showStats })
          .onChange((isOn: boolean) => {
            this.showStats = isOn;
          })
      }
    }
  }
}
