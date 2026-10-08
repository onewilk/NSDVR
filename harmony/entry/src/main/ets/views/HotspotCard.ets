import { promptAction } from '@kit.ArkUI';
import { checkHotspotConfig, HotspotService, randomPassphrase } from '../common/HotspotService';
import { tr } from '../common/I18n';
import { Card, NavRow, SettingRow, TipBar } from './Widgets';

/**
 * Switch 专用热点：卡片上只显示开关和一行状态（不直接露出网络名和密码）；
 * 打开后出现“热点配置”，点开从底部弹出：查看/修改网络名和密码、设置打开 App 时自动开启。
 */
@Component
export struct HotspotCard {
  @StorageProp('hs.on') on: boolean = false;
  @StorageProp('hs.busy') busy: boolean = false;
  @StorageProp('hs.summary') summary: string = '';
  @StorageProp('hs.detail') detail: string = '';
  @StorageProp('hs.warn') warn: string = '';
  @StorageLink('sysdvr.hotspotSsid') ssid: string = 'DIRECT-SysDVR';
  @StorageLink('sysdvr.hotspotPass') pass: string = '';
  @StorageLink('sysdvr.hotspotAuto') auto: boolean = false;
  @State showSheet: boolean = false;
  // 弹窗里编辑中的草稿，点右上角“保存”才生效；直接关掉弹窗则放弃修改
  @State draftSsid: string = '';
  @State draftPass: string = '';
  @State draftError: string = '';
  @State showPass: boolean = false;

  private openSheet(): void {
    this.draftSsid = this.ssid;
    this.draftPass = this.pass;
    this.draftError = '';
    this.showPass = false;
    this.showSheet = true;
  }

  /** 网络名或密码改过（和已保存的不一样）：右上角的关闭按钮变成对勾保存 */
  private dirty(): boolean {
    return this.draftSsid.trim() !== this.ssid || this.draftPass !== this.pass;
  }

  private async save(): Promise<void> {
    const ssid = this.draftSsid.trim();
    const invalid = checkHotspotConfig(ssid, this.draftPass);
    if (invalid.length > 0) {
      this.draftError = invalid;
      return;
    }
    const changed = ssid !== this.ssid || this.draftPass !== this.pass;
    this.ssid = ssid;
    this.pass = this.draftPass;
    this.showSheet = false;
    if (changed && this.on) {
      const err = await HotspotService.get().restart();
      promptAction.showToast({
        message: err.length > 0 ? tr('cfg_hs_rebuild_failed', err) : tr('cfg_hs_rebuilt'),
        duration: 3000
      });
    }
  }

  // 自定义标题栏：没改动时右上角是关闭；改了网络名或密码就变成醒目的“保存”按钮（不会被软键盘挡住）
  @Builder
  SheetTitle() {
    Row() {
      Text(tr('cfg_hs_sheet_title'))
        .fontSize(20)
        .fontWeight(FontWeight.Bold)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
      if (this.dirty()) {
        Button(tr('cfg_hs_save'), { type: ButtonType.Capsule })
          .height(36)
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .padding({ left: 18, right: 18 })
          .backgroundColor($r('app.color.accent'))
          .onClick(() => {
            this.save();
          })
      } else {
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.xmark')).fontSize(20).fontColor([$r('app.color.text_secondary')])
        }
        .width(40)
        .height(40)
        .backgroundColor($r('app.color.chip_bg'))
        .accessibilityText(tr('cfg_hs_close'))
        .onClick(() => {
          this.showSheet = false;
        })
      }
    }
    .width('100%')
    .height(56)
  }

  @Builder
  SheetContent() {
    Scroll() {
      Column({ space: 16 }) {
        if (this.detail.length > 0) {
          Column({ space: 6 }) {
            Text(tr('cfg_hs_current')).fontSize(13).fontColor($r('app.color.text_secondary'))
            Text(`${tr('cfg_hs_ssid_line', this.ssid)}\n${tr('cfg_hs_pass_line', this.pass)}\n${this.detail}`)
              .fontSize(13)
              .lineHeight(21)
              .fontColor($r('app.color.text_primary'))
              .copyOption(CopyOptions.LocalDevice)
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')
          .padding(12)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_bg'))
        }

        Column({ space: 10 }) {
          Text(tr('cfg_hs_custom_title')).fontSize(15).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
          // 关掉系统自动填充：否则密码保险箱会把“网络名 + 密码”当成登录表单弹出填充和人脸验证
          TextInput({ text: this.draftSsid, placeholder: tr('cfg_hs_ssid_hint') })
            .height(44)
            .enableAutoFill(false)
            .onChange((v: string) => {
              this.draftSsid = v;
              this.draftError = '';
            })
          Row({ space: 8 }) {
            TextInput({ text: this.draftPass, placeholder: tr('cfg_hs_pass_hint') })
              .type(this.showPass ? InputType.Normal : InputType.Password)
              .showPasswordIcon(true)
              .maxLength(63)
              .height(44)
              .layoutWeight(1)
              .enableAutoFill(false)
              .onChange((v: string) => {
                this.draftPass = v;
                this.draftError = '';
              })
            Button(tr('cfg_hs_random')).height(40).fontSize(13).onClick(() => {
              this.draftPass = randomPassphrase();
              this.showPass = true;
            })
          }
          .width('100%')
          if (this.draftError.length > 0) {
            Text(this.draftError).fontSize(12).fontColor($r('app.color.warn'))
          } else if (this.dirty()) {
            Text(tr('cfg_hs_unsaved')).fontSize(12).fontColor($r('app.color.accent'))
          } else {
            Text(tr('cfg_hs_edit_note')).fontSize(12).fontColor($r('app.color.text_tertiary'))
          }
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')

        SettingRow({
          title: tr('cfg_hs_auto_title'),
          desc: tr('cfg_hs_auto_desc'),
          icon: $r('sys.symbol.power')
        }) {
          Toggle({ type: ToggleType.Switch, isOn: this.auto })
            .onChange((isOn: boolean) => {
              this.auto = isOn;
            })
        }

      }
      .padding({ left: 16, right: 16, top: 8, bottom: 24 })
    }
    .scrollBar(BarState.Off)
  }

  build() {
    // 外面包一层 Column 再挂 bindSheet（自定义组件上不一定支持弹窗类通用属性）
    Column() {
    Card() {
      SettingRow({
        title: tr('cfg_hs_title'),
        desc: this.on || this.busy ? this.summary :
          tr('cfg_hs_desc'),
        icon: $r('sys.symbol.hotspot')
      }) {
        if (this.busy) {
          LoadingProgress().width(28).height(28).color($r('app.color.accent'))
        } else {
          Toggle({ type: ToggleType.Switch, isOn: this.on })
            .onChange((isOn: boolean) => {
              if (isOn) {
                HotspotService.get().start().then((err: string) => {
                  if (err.length > 0) {
                    promptAction.showToast({ message: err, duration: 3000 });
                  }
                });
              } else {
                HotspotService.get().stop();
              }
            })
        }
      }

      if (this.on) {
        Divider().color($r('app.color.divider'))
        NavRow({
          title: tr('cfg_hs_settings'),
          desc: tr('cfg_hs_settings_desc'),
          icon: $r('sys.symbol.gearshape'),
          onTap: () => {
            this.openSheet();
          }
        })
      } else if (!this.busy && this.summary.length > 0 && this.summary !== tr('hs_closed')) {
        Text(this.summary).fontSize(12).fontColor($r('app.color.warn'))
      }

      if (this.warn.length > 0) {
        TipBar({ text: this.warn, warn: true })
      }
    }
    }
    .width('100%')
    .bindSheet($$this.showSheet, this.SheetContent(), {
      title: this.SheetTitle,
      height: SheetSize.MEDIUM,
      dragBar: true,
      showClose: false,
      backgroundColor: $r('app.color.card_bg')
    })
  }
}
