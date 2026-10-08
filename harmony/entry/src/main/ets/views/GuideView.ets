import { tr } from '../common/I18n';
import { Breakpoint } from '../common/Prefs';
import { Card, Step, TipBar } from './Widgets';

interface GuideItem {
  title: string;
  body: string;
}

interface GuideSection {
  key: string;
  title: string;
  icon: Resource;
  intro: string;
  steps: string[];
  items: GuideItem[];
  tip: string;
}

function item(title: string, body: string): GuideItem {
  const it: GuideItem = { title: title, body: body };
  return it;
}

/** 使用说明的全部内容；每次构建页面时按当前语言取词 */
function sections(): GuideSection[] {
  return [
    {
      key: 'prepare',
      title: tr('guide_prepare_title'),
      icon: $r('sys.symbol.gamecontroller'),
      intro: tr('guide_prepare_intro'),
      steps: [tr('guide_prepare_step1'), tr('guide_prepare_step2'), tr('guide_prepare_step3'), tr('guide_prepare_step4')],
      items: [],
      tip: tr('guide_prepare_tip')
    },
    {
      key: 'tcp',
      title: tr('guide_tcp_title'),
      icon: $r('sys.symbol.ohos_wifi'),
      intro: tr('guide_tcp_intro'),
      steps: [tr('guide_tcp_step1'), tr('guide_tcp_step2'), tr('guide_tcp_step3')],
      items: [
        item(tr('guide_tcp_item1_title'), tr('guide_tcp_item1_body')),
        item(tr('guide_tcp_item2_title'), tr('guide_tcp_item2_body'))
      ],
      tip: tr('guide_tcp_tip')
    },
    {
      key: 'usb',
      title: tr('guide_usb_title'),
      icon: $r('sys.symbol.charg_cable'),
      intro: tr('guide_usb_intro'),
      steps: [tr('guide_usb_step1'), tr('guide_usb_step2'), tr('guide_usb_step3'), tr('guide_usb_step4')],
      items: [
        item(tr('guide_usb_item1_title'), tr('guide_usb_item1_body')),
        item(tr('guide_usb_item2_title'), tr('guide_usb_item2_body'))
      ],
      tip: tr('guide_usb_tip')
    },
    {
      key: 'rtsp',
      title: tr('guide_rtsp_title'),
      icon: $r('sys.symbol.play'),
      intro: tr('guide_rtsp_intro'),
      steps: [tr('guide_rtsp_step1'), tr('guide_rtsp_step2')],
      items: [
        item(tr('guide_rtsp_item1_title'), tr('guide_rtsp_item1_body')),
        item(tr('guide_rtsp_item2_title'), tr('guide_rtsp_item2_body'))
      ],
      tip: tr('guide_rtsp_tip')
    },
    {
      key: 'options',
      title: tr('guide_opt_title'),
      icon: $r('sys.symbol.slider_horizontal_2'),
      intro: tr('guide_opt_intro'),
      steps: [],
      items: [
        item(tr('guide_opt_audio_title'), tr('guide_opt_audio_body')),
        item(tr('guide_opt_smooth_title'), tr('guide_opt_smooth_body')),
        item(tr('guide_opt_freeze_title'), tr('guide_opt_freeze_body')),
        item(tr('guide_opt_enhance_title'), tr('guide_opt_enhance_body')),
        item(tr('guide_opt_net_title'), tr('guide_opt_net_body')),
        item(tr('guide_opt_hotspot_title'), tr('guide_opt_hotspot_body')),
        item(tr('guide_opt_screen_title'), tr('guide_opt_screen_body')),
        item(tr('guide_opt_keep_title'), tr('guide_opt_keep_body')),
        item(tr('guide_opt_stats_title'), tr('guide_opt_stats_body')),
        item(tr('guide_opt_lock_title'), tr('guide_opt_lock_body')),
        item(tr('guide_opt_dark_title'), tr('guide_opt_dark_body')),
        item(tr('guide_opt_lang_title'), tr('guide_opt_lang_body'))
      ],
      tip: ''
    },
    {
      key: 'faq',
      title: tr('guide_faq_title'),
      icon: $r('sys.symbol.questionmark_circle'),
      intro: '',
      steps: [],
      items: [
        item(tr('guide_faq_q1'), tr('guide_faq_a1')),
        item(tr('guide_faq_q2'), tr('guide_faq_a2')),
        item(tr('guide_faq_q3'), tr('guide_faq_a3')),
        item(tr('guide_faq_q4'), tr('guide_faq_a4')),
        item(tr('guide_faq_q5'), tr('guide_faq_a5'))
      ],
      tip: ''
    }
  ];
}

/** 使用说明：每种模式的步骤、各选项说明和常见问题；宽屏时左侧目录 */
@Component
export struct GuideView {
  @Prop bp: Breakpoint = 'sm';
  @State current: number = 0;
  @StorageProp('ui.safeBottom') safeBottom: number = 0;
  private scroller: Scroller = new Scroller();
  private sections: GuideSection[] = sections();

  @Builder
  SectionCard(s: GuideSection) {
    Card({ title: s.title }) {
      if (s.intro.length > 0) {
        Text(s.intro)
          .fontSize(14)
          .lineHeight(22)
          .fontColor($r('app.color.text_secondary'))
      }
      ForEach(s.steps, (step: string, i: number) => {
        Step({ index: i + 1, text: step })
      }, (step: string, i: number) => `${s.key}-step-${i}`)
      ForEach(s.items, (item: GuideItem) => {
        Column({ space: 4 }) {
          Text(item.title)
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          Text(item.body)
            .fontSize(13)
            .lineHeight(21)
            .fontColor($r('app.color.text_secondary'))
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
      }, (item: GuideItem) => `${s.key}-${item.title}`)
      if (s.tip.length > 0) {
        TipBar({ text: s.tip })
      }
    }
  }

  @Builder
  Toc() {
    Column({ space: 4 }) {
      ForEach(this.sections, (s: GuideSection, i: number) => {
        Row({ space: 10 }) {
          SymbolGlyph(s.icon)
            .fontSize(18)
            .fontColor([this.current === i ? $r('app.color.accent') : $r('app.color.text_secondary')])
          Text(s.title)
            .fontSize(14)
            .fontColor(this.current === i ? $r('app.color.accent') : $r('app.color.text_primary'))
            .maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 10, bottom: 10 })
        .borderRadius(10)
        .backgroundColor(this.current === i ? $r('app.color.accent_soft') : Color.Transparent)
        .onClick(() => {
          this.current = i;
          this.scroller.scrollToIndex(i + 1, true); // 第 0 项是页头
        })
      }, (s: GuideSection) => s.key)
    }
    .width(280)
    .padding(8)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_bg'))
  }

  build() {
    Row({ space: 16 }) {
      if (this.bp === 'lg') {
        this.Toc()
      }
      List({ space: 16, scroller: this.scroller }) {
        ListItem() {
          Column({ space: 4 }) {
            Text(tr('guide_subtitle'))
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')
        }

        ForEach(this.sections, (s: GuideSection) => {
          ListItem() {
            this.SectionCard(s)
          }
        }, (s: GuideSection) => s.key)
      }
      .layoutWeight(1)
      .height('100%')
      .scrollBar(BarState.Off)
      .edgeEffect(EdgeEffect.Spring)
      .contentEndOffset(32 + this.safeBottom)
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
      .onScrollIndex((start: number) => {
        // 第 0 项是页头
        this.current = Math.max(0, Math.min(this.sections.length - 1, start - 1));
      })
    }
    .alignItems(VerticalAlign.Top)
    .width('100%')
    .height('100%')
    .constraintSize({ maxWidth: this.bp === 'lg' ? 1120 : 720 })
    .padding({ left: 16, right: 16, top: 16 })
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
  }
}
