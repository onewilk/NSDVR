import { tr } from '../common/I18n';
import { Breakpoint, hotspotSupported } from '../common/Prefs';
import { ExtAudioCard } from './ExtAudioCard';
import { HotspotCard } from './HotspotCard';
import { OptionsCard } from './OptionsCard';
import { OtherSettingsCard } from './OtherSettingsCard';
import { PageTitle } from './Widgets';

/** “配置”页：专用热点、串流选项、其他设置。所有屏幕都是单栏列表，每张卡片占满可用宽度 */
@Component
export struct ConfigView {
  @Prop bp: Breakpoint = 'sm';
  // 底部悬浮导航栏占用的高度，内容要留出这段空间
  @StorageProp('ui.bottomReserve') bottomReserve: number = 0;
  // 实验功能：连接过表明支持扩展的服务端（记在 sysdvr.extHosts），或打开了开发者选项才显示
  @StorageProp('sysdvr.extHosts') extHosts: string = '';
  @StorageProp('sysdvr.devMode') devMode: boolean = false;

  build() {
    Scroll() {
      Column({ space: 16 }) {
        PageTitle({ title: tr('app_cfg_title'), subtitle: tr('app_cfg_subtitle'), large: this.bp !== 'sm' })

        if (hotspotSupported()) {
          HotspotCard()
            .width('100%')
        }

        OptionsCard()
          .width('100%')

        if (this.extHosts.length > 0 || this.devMode) {
          ExtAudioCard()
        }

        // 其他设置放在最下面
        OtherSettingsCard()
      }
      .width('100%')
      .constraintSize({ maxWidth: this.bp === 'lg' ? 1120 : 720 })
      .padding({ left: 16, right: 16, top: 16, bottom: 16 + this.bottomReserve })
    }
    .width('100%')
    .height('100%')
    .align(Alignment.Top)
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    // 内容延伸到底部小白条下面，滚动时从悬浮导航栏和小白条下透过去
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
  }
}
