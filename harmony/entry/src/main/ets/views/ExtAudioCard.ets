import { promptAction } from '@kit.ArkUI';
import {
  AudioCombo, codecLabels, comboLabel, currentCombo, DEFAULT_ROTATE_PLAN, ExtCodec, formatPlan, OPUS_COMPLEXITY_OPTIONS,
  OPUS_FRAME_OPTIONS, OPUS_KBPS_OPTIONS, parsePlan, ROTATE_MINUTES, sameCombo
} from '../common/ExtAudio';
import { tr } from '../common/I18n';
import { scopeTcpOnly } from '../common/Prefs';
import { Card, SettingRow } from './Widgets';

function selectOptions(labels: string[]): SelectOption[] {
  return labels.map((label: string): SelectOption => {
    const option: SelectOption = { value: label };
    return option;
  });
}

/**
 * 配置页的“实验功能”：音频编码与 Opus 参数、自动轮换对比。只有连接过表明支持扩展的服务端
 * （或打开了开发者选项）才显示，由 ConfigView 决定是否挂出来；播放中也可以在快捷设置里即时修改
 */
@Component
export struct ExtAudioCard {
  @StorageLink('sysdvr.extCodec') codec: number = 0;
  @StorageLink('sysdvr.extOpusKbps') opusKbps: number = 96;
  @StorageLink('sysdvr.extOpusComplexity') opusComplexity: number = 5;
  @StorageLink('sysdvr.extOpusFrameMs') opusFrameMs: number = 20;
  @StorageLink('sysdvr.rotateOn') rotateOn: boolean = false;
  @StorageLink('sysdvr.rotateMinutesIndex') rotateMinutesIndex: number = 1;
  @StorageLink('sysdvr.rotatePlan') rotatePlan: string = DEFAULT_ROTATE_PLAN;
  @StorageLink('sysdvr.devMode') devMode: boolean = false;

  private kbpsLabels(): string[] {
    return OPUS_KBPS_OPTIONS.map((v: number) => tr('ext_kbps_value', v));
  }

  private complexityLabels(): string[] {
    return OPUS_COMPLEXITY_OPTIONS.map((v: number) => `${v}`);
  }

  private frameLabels(): string[] {
    return OPUS_FRAME_OPTIONS.map((v: number) => tr('ext_ms_value', v));
  }

  private minutesLabels(): string[] {
    return ROTATE_MINUTES.map((v: number) => tr('app_minutes', v));
  }

  private plan(): AudioCombo[] {
    return parsePlan(this.rotatePlan);
  }

  private addCurrent(): void {
    const list = this.plan();
    const c = currentCombo();
    if (list.some((x: AudioCombo) => sameCombo(x, c))) {
      promptAction.showToast({ message: tr('ext_rotate_exists') });
      return;
    }
    list.push(c);
    this.rotatePlan = formatPlan(list);
  }

  private removeAt(index: number): void {
    const list = this.plan();
    list.splice(index, 1);
    this.rotatePlan = formatPlan(list);
  }

  build() {
    Card({ title: tr('ext_card_title'), subtitle: tr('ext_card_subtitle') }) {
      SettingRow({
        title: tr('ext_codec'),
        desc: tr('ext_codec_desc'),
        scope: scopeTcpOnly(),
        icon: $r('sys.symbol.waveform')
      }) {
        Select(selectOptions(codecLabels()))
          .selected(this.codec)
          .value(codecLabels()[this.codec])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.codec = index;
          })
      }

      if (this.codec === ExtCodec.OPUS) {
        SettingRow({
          title: tr('ext_opus_kbps'),
          desc: tr('ext_opus_kbps_desc'),
          icon: $r('sys.symbol.slider_horizontal_2')
        }) {
          Select(selectOptions(this.kbpsLabels()))
            .selected(Math.max(0, OPUS_KBPS_OPTIONS.indexOf(this.opusKbps)))
            .value(tr('ext_kbps_value', this.opusKbps))
            .font({ size: 14 })
            .onSelect((index: number) => {
              this.opusKbps = OPUS_KBPS_OPTIONS[index];
            })
        }

        SettingRow({
          title: tr('ext_opus_complexity'),
          desc: tr('ext_opus_complexity_desc'),
          icon: $r('sys.symbol.wand_and_stars')
        }) {
          Select(selectOptions(this.complexityLabels()))
            .selected(this.opusComplexity)
            .value(`${this.opusComplexity}`)
            .font({ size: 14 })
            .onSelect((index: number) => {
              this.opusComplexity = OPUS_COMPLEXITY_OPTIONS[index];
            })
        }

        SettingRow({
          title: tr('ext_opus_frame'),
          desc: tr('ext_opus_frame_desc'),
          icon: $r('sys.symbol.timer')
        }) {
          Select(selectOptions(this.frameLabels()))
            .selected(Math.max(0, OPUS_FRAME_OPTIONS.indexOf(this.opusFrameMs)))
            .value(tr('ext_ms_value', this.opusFrameMs))
            .font({ size: 14 })
            .onSelect((index: number) => {
              this.opusFrameMs = OPUS_FRAME_OPTIONS[index];
            })
        }
      }

      Divider().color($r('app.color.divider'))

      SettingRow({
        title: tr('ext_rotate'),
        desc: tr('ext_rotate_desc'),
        icon: $r('sys.symbol.repeat')
      }) {
        Toggle({ type: ToggleType.Switch, isOn: this.rotateOn })
          .onChange((isOn: boolean) => {
            if (isOn && this.plan().length < 2) {
              promptAction.showToast({ message: tr('ext_rotate_need_two') });
              this.rotateOn = false;
              return;
            }
            this.rotateOn = isOn;
          })
      }

      SettingRow({
        title: tr('ext_rotate_duration'),
        desc: '',
        icon: $r('sys.symbol.stopwatch')
      }) {
        Select(selectOptions(this.minutesLabels()))
          .selected(this.rotateMinutesIndex)
          .value(this.minutesLabels()[this.rotateMinutesIndex])
          .font({ size: 14 })
          .onSelect((index: number) => {
            this.rotateMinutesIndex = index;
          })
      }

      Text(tr('ext_rotate_plan', this.plan().length))
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))
      if (this.plan().length === 0) {
        Text(tr('ext_rotate_empty')).fontSize(13).fontColor($r('app.color.text_tertiary'))
      }
      ForEach(this.plan(), (c: AudioCombo, index: number) => {
        Row({ space: 8 }) {
          Text(`${index + 1}`)
            .fontSize(12)
            .fontColor($r('app.color.on_accent'))
            .backgroundColor($r('app.color.accent'))
            .width(20)
            .height(20)
            .borderRadius(10)
            .textAlign(TextAlign.Center)
          Text(comboLabel(c))
            .fontSize(14)
            .fontColor($r('app.color.text_primary'))
            .layoutWeight(1)
          SymbolGlyph($r('sys.symbol.trash'))
            .fontSize(18)
            .fontColor([$r('app.color.text_secondary')])
            .padding(6)
            .onClick(() => {
              this.removeAt(index);
            })
            .accessibilityText(tr('ext_rotate_remove'))
        }
        .width('100%')
      }, (c: AudioCombo, index: number) => `${index}:${formatPlan([c])}`)

      Row({ space: 8 }) {
        Button(tr('ext_rotate_add'))
          .fontSize(13)
          .height(32)
          .backgroundColor($r('app.color.accent_soft'))
          .fontColor($r('app.color.accent'))
          .onClick(() => {
            this.addCurrent();
          })
        Button(tr('ext_rotate_reset'))
          .fontSize(13)
          .height(32)
          .backgroundColor($r('app.color.chip_bg'))
          .fontColor($r('app.color.text_secondary'))
          .onClick(() => {
            this.rotatePlan = DEFAULT_ROTATE_PLAN;
          })
      }

      if (this.devMode) {
        Divider().color($r('app.color.divider'))
        SettingRow({
          title: tr('ext_dev_row'),
          desc: tr('ext_dev_row_desc'),
          icon: $r('sys.symbol.gearshape')
        }) {
          Toggle({ type: ToggleType.Switch, isOn: this.devMode })
            .onChange((isOn: boolean) => {
              this.devMode = isOn;
            })
        }
      }
    }
  }
}
