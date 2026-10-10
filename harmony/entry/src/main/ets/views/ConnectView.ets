import { tr } from '../common/I18n';
import { APP_NAME, Breakpoint, ModeMeta, modes, modeTip, StreamMode } from '../common/Prefs';
import { StreamTarget } from '../common/StreamTarget';
import { RtspPanel } from './RtspPanel';
import { TcpPanel } from './TcpPanel';
import { UsbPanel } from './UsbPanel';
import { Chip, TipBar } from './Widgets';

/**
 * “串流”页：选模式、看提示，列表最后是连接卡片（局域网内的设备 / USB 设备 / RTSP 地址）和最近连接。
 * 手机上模式是纵向列表，宽屏是横排卡片。
 */
@Component
export struct ConnectView {
  @Prop bp: Breakpoint = 'sm';
  @Prop playing: boolean = false;
  @StorageLink('sysdvr.mode') mode: number = StreamMode.TCP;
  @StorageProp('ui.bottomReserve') bottomReserve: number = 0;
  @StorageProp('hs.on') hotspotOn: boolean = false;
  @StorageProp('hs.warn') hotspotWarn: string = '';
  onConnect: (target: StreamTarget) => void = () => {
  };
  onOpenGuide: () => void = () => {
  };

  @Builder
  Header() {
    Row() {
      Column({ space: 4 }) {
        Text(APP_NAME)
          .fontSize(this.bp === 'sm' ? 26 : 30)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
        Text(tr('connect_subtitle'))
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Button({ type: ButtonType.Normal, buttonStyle: ButtonStyleMode.TEXTUAL }) {
        Row({ space: 4 }) {
          SymbolGlyph($r('sys.symbol.questionmark_circle')).fontSize(18).fontColor([$r('app.color.accent')])
          Text(tr('connect_how_to')).fontSize(14).fontColor($r('app.color.accent'))
        }
      }
      .onClick(() => {
        this.onOpenGuide();
      })
    }
    .width('100%')
  }

  // 手机：纵向列表中的一行
  @Builder
  ModeRow(meta: ModeMeta) {
    Row({ space: 12 }) {
      Row() {
        SymbolGlyph(meta.icon)
          .fontSize(22)
          .fontColor([this.mode === meta.mode ? $r('app.color.on_accent') : $r('app.color.accent')])
      }
      .width(40)
      .height(40)
      .justifyContent(FlexAlign.Center)
      .borderRadius(20)
      .backgroundColor(this.mode === meta.mode ? $r('app.color.accent') : $r('app.color.accent_soft'))
      Column({ space: 2 }) {
        Row({ space: 6 }) {
          Text(meta.title)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          if (meta.mode === StreamMode.TCP) {
            Chip({ text: tr('connect_recommended'), accent: true })
          }
        }
        Text(`${meta.subtitle} · ${meta.desc}`)
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      SymbolGlyph(this.mode === meta.mode ? $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.circle'))
        .fontSize(22)
        .fontColor([this.mode === meta.mode ? $r('app.color.accent') : $r('app.color.text_tertiary')])
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .onClick(() => {
      this.mode = meta.mode;
    })
    .accessibilityText(`${meta.title}, ${meta.subtitle}, ${meta.desc}`)
  }

  // 宽屏：横排卡片
  @Builder
  ModeCard(meta: ModeMeta) {
    Column({ space: 6 }) {
      Row() {
        SymbolGlyph(meta.icon)
          .fontSize(24)
          .fontColor([this.mode === meta.mode ? $r('app.color.accent') : $r('app.color.text_secondary')])
        Blank()
        if (meta.mode === StreamMode.TCP) {
          Chip({ text: tr('connect_recommended'), accent: this.mode === meta.mode })
        }
      }
      .width('100%')

      Text(meta.title)
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.mode === meta.mode ? $r('app.color.accent') : $r('app.color.text_primary'))
      Text(`${meta.subtitle} · ${meta.desc}`)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
        .maxLines(2)
    }
    .alignItems(HorizontalAlign.Start)
    .layoutWeight(1)
    .padding(14)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_bg'))
    .border({ width: 1.5, color: this.mode === meta.mode ? $r('app.color.accent') : Color.Transparent })
    .onClick(() => {
      this.mode = meta.mode;
    })
    .accessibilityText(`${meta.title}, ${meta.subtitle}, ${meta.desc}`)
  }

  @Builder
  ModeSelector() {
    if (this.bp === 'sm') {
      Column() {
        ForEach(modes(), (meta: ModeMeta, i: number) => {
          if (i > 0) {
            Divider().color($r('app.color.divider')).margin({ left: 64, right: 12 })
          }
          this.ModeRow(meta)
        }, (meta: ModeMeta) => `row-${meta.mode}`)
      }
      .width('100%')
      .padding({ top: 4, bottom: 4 })
      .borderRadius(16)
      .backgroundColor($r('app.color.card_bg'))
    } else {
      Row({ space: 12 }) {
        ForEach(modes(), (meta: ModeMeta) => {
          this.ModeCard(meta)
        }, (meta: ModeMeta) => `card-${meta.mode}`)
      }
      .width('100%')
    }
  }

  build() {
    Column() {
      Scroll() {
        Column({ space: 16 }) {
          this.Header()
          this.ModeSelector()
          TipBar({ text: modeTip(this.mode) })

          // 手机 WLAN 与专用热点不在同一信道：串流时会周期性掉帧，在串流页也提醒一下
          if (this.mode !== StreamMode.USB && this.hotspotOn && this.hotspotWarn.length > 0) {
            TipBar({ text: this.hotspotWarn, warn: true })
          }

          // 连接卡片在列表最后：三个面板都保持挂载，只切换显示，切模式时授权状态、输入内容不会丢
          TcpPanel({ active: this.mode === StreamMode.TCP && !this.playing, onConnect: this.onConnect })
            .visibility(this.mode === StreamMode.TCP ? Visibility.Visible : Visibility.None)
          UsbPanel({ active: this.mode === StreamMode.USB && !this.playing, onConnect: this.onConnect })
            .visibility(this.mode === StreamMode.USB ? Visibility.Visible : Visibility.None)
          RtspPanel({ onConnect: this.onConnect })
            .visibility(this.mode === StreamMode.RTSP ? Visibility.Visible : Visibility.None)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 16, bottom: 16 + this.bottomReserve })
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
      .scrollBar(BarState.Off)
      .edgeEffect(EdgeEffect.Spring)
      // 内容延伸到底部小白条下面，滚动时从悬浮导航栏和小白条下透过去
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
    }
    .width('100%')
    .height('100%')
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
    .constraintSize({ maxWidth: this.bp === 'lg' ? 1120 : 720 })
  }
}
