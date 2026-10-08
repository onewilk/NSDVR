import { bundleManager, common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { productViewManager } from '@kit.StoreKit';
import { APP_NAME, Breakpoint, IJKPLAYER_REPO, SYSDVR_GITHUB } from '../common/Prefs';
import { tr } from '../common/I18n';
import { Card, NavRow } from './Widgets';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

interface LicenseItem {
  name: string;
  license: string;
  note: string;
  // rawfile 里的许可全文，空串表示没有。BSD 等许可要求二进制分发附上版权声明、条款和免责声明
  textFile: string;
}

function licenses(): LicenseItem[] {
  return [
    { name: 'SysDVR', license: 'GPL-2.0', note: tr('about_lic_sysdvr_note'), textFile: '' },
    { name: tr('about_ijk_name'), license: 'LGPL-2.1+', note: tr('about_lic_ijk_note'), textFile: '' },
    { name: 'FFmpeg', license: 'LGPL-2.1+', note: tr('about_lic_ffmpeg_note'), textFile: '' },
    // 原样复制自 third_party/opus/COPYING，升级 libopus 时一起更新
    { name: 'Opus', license: 'BSD-3-Clause', note: tr('about_lic_opus_note'), textFile: 'licenses/opus_COPYING.txt' }
  ];
}

/** 关于：版本、SysDVR 项目链接、开源许可、隐私与免责声明 */
@Component
export struct AboutView {
  @Prop bp: Breakpoint = 'sm';
  @StorageProp('ui.bottomReserve') bottomReserve: number = 0;
  @State version: string = '';
  // 开发者选项：连点版本号 7 次开关（没有支持扩展的服务端时也显示实验功能，方便看界面）
  @StorageLink('sysdvr.devMode') devMode: boolean = false;
  private versionTaps: number = 0;
  private lastTapMs: number = 0;
  private bundleName: string = '';
  private licenses: LicenseItem[] = licenses();
  @State showLicense: boolean = false;
  @State licenseTitle: string = '';
  @State licenseText: string = '';
  onOpenGuide: () => void = () => {
  };

  aboutToAppear(): void {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
      this.version = info.versionName;
      this.bundleName = info.name;
    } catch (e) {
      this.version = '';
    }
  }

  private onVersionTap(): void {
    const now = Date.now();
    // 两次点击间隔超过 1.5 秒就重新计数
    this.versionTaps = now - this.lastTapMs > 1500 ? 1 : this.versionTaps + 1;
    this.lastTapMs = now;
    if (this.versionTaps >= 7) {
      this.versionTaps = 0;
      this.devMode = !this.devMode;
      promptAction.showToast({ message: this.devMode ? tr('ext_dev_on') : tr('ext_dev_off') });
    } else if (this.versionTaps >= 4) {
      promptAction.showToast({ message: tr(this.devMode ? 'ext_dev_taps_off' : 'ext_dev_taps', 7 - this.versionTaps),
        duration: 1000 });
    }
  }

  private open(url: string): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    ctx.openLink(url, { appLinkingOnly: false }).catch((e: Error) => {
      hilog.warn(DOMAIN, TAG, 'openLink failed: %{public}s', JSON.stringify(e));
      promptAction.showToast({ message: tr('about_open_failed', url) });
    });
  }

  /** 打开应用市场里本应用的详情页（评分、写评论） */
  private openStore(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    try {
      productViewManager.loadProduct(ctx, { parameters: { bundleName: this.bundleName } }, {
        onError: (err: BusinessError) => {
          hilog.warn(DOMAIN, TAG, 'loadProduct failed: %{public}d %{public}s', err.code, err.message);
          promptAction.showToast({ message: tr('about_store_unavailable') });
        }
      });
    } catch (e) {
      promptAction.showToast({ message: tr('about_store_failed') });
    }
  }

  private openLicense(l: LicenseItem): void {
    try {
      const bytes = getContext(this).resourceManager.getRawFileContentSync(l.textFile);
      const raw = util.TextDecoder.create('utf-8').decodeToString(bytes);
      // 原文按 80 列硬换行，屏幕上按段落重排（只改空白，文字不变）
      this.licenseText = raw.trim().split(/\n\s*\n/)
        .map((p: string) => p.split('\n').map((l: string) => l.trim()).join(' '))
        .join('\n\n');
      this.licenseTitle = tr('about_lic_sheet_title', l.name);
      this.showLicense = true;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'license text %{public}s: %{public}s', l.textFile, JSON.stringify(e));
      promptAction.showToast({ message: tr('about_lic_load_failed') });
    }
  }

  @Builder
  LicenseSheet() {
    Scroll() {
      Text(this.licenseText)
        .fontSize(12)
        .lineHeight(18)
        .fontColor($r('app.color.text_secondary'))
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
    }
    .width('100%')
    .height('100%')
    .align(Alignment.Top)
    .padding({ left: 20, right: 20, bottom: 24 })
    .scrollBar(BarState.Auto)
  }

  @Builder
  LinkRow(icon: Resource, title: string, url: string) {
    Row({ space: 12 }) {
      SymbolGlyph(icon).fontSize(22).fontColor([$r('app.color.accent')])
      Column({ space: 2 }) {
        Text(title).fontSize(15).fontColor($r('app.color.text_primary'))
        Text(url).fontSize(12).fontColor($r('app.color.text_secondary')).maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      SymbolGlyph($r('sys.symbol.arrow_up_right')).fontSize(18).fontColor([$r('app.color.text_tertiary')])
    }
    .width('100%')
    .padding({ top: 4, bottom: 4 })
    .onClick(() => {
      this.open(url);
    })
  }

  build() {
    Scroll() {
      Column({ space: 16 }) {
        Column({ space: 8 }) {
          Image($r('app.media.icon'))
            .width(88)
            .height(88)
            .borderRadius(20)
          Text(APP_NAME)
            .fontSize(24)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary'))
          Text(this.version.length > 0 ? tr('about_version', this.version) : '')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
            .onClick(() => {
              this.onVersionTap();
            })
          Text(tr('about_desc'))
            .fontSize(14)
            .lineHeight(22)
            .fontColor($r('app.color.text_secondary'))
            .textAlign(TextAlign.Center)
        }
        .width('100%')
        .padding({ top: 24, bottom: 8 })

        Card() {
          NavRow({
            title: tr('about_guide_title'),
            desc: tr('about_guide_desc'),
            icon: $r('sys.symbol.book'),
            onTap: () => {
              this.onOpenGuide();
            }
          })
        }

        Card({ title: tr('about_thanks_title'), subtitle: tr('about_thanks_sub') }) {
          this.LinkRow($r('sys.symbol.link'), tr('about_sysdvr_link'), SYSDVR_GITHUB)
          Text(tr('about_sysdvr_note'))
            .fontSize(12)
            .lineHeight(19)
            .fontColor($r('app.color.text_tertiary'))
          Divider().color($r('app.color.divider'))
          this.LinkRow($r('sys.symbol.play'), tr('about_ijk_name'), IJKPLAYER_REPO)
        }

        Card({ title: tr('about_licenses_title') }) {
          ForEach(this.licenses, (l: LicenseItem) => {
            Row({ space: 8 }) {
              Column({ space: 2 }) {
                Text(l.name).fontSize(14).fontColor($r('app.color.text_primary'))
                Text(l.note).fontSize(12).fontColor($r('app.color.text_secondary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              if (l.textFile.length > 0) {
                Column({ space: 2 }) {
                  Text(l.license).fontSize(12).fontColor($r('app.color.text_secondary'))
                  Text(tr('about_lic_view')).fontSize(12).fontColor($r('app.color.accent'))
                }
                .alignItems(HorizontalAlign.End)

                SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(16).fontColor([$r('app.color.text_tertiary')])
              } else {
                Text(l.license).fontSize(12).fontColor($r('app.color.text_secondary'))
              }
            }
            .width('100%')
            .onClick(() => {
              if (l.textFile.length > 0) {
                this.openLicense(l);
              }
            })
          }, (l: LicenseItem) => l.name)
        }

        Card({ title: tr('about_privacy_title') }) {
          Text(tr('about_privacy_body'))
            .fontSize(13)
            .lineHeight(21)
            .fontColor($r('app.color.text_secondary'))
        }

        Card({ title: tr('about_disclaimer_title') }) {
          Text(tr('about_disclaimer_body'))
            .fontSize(13)
            .lineHeight(21)
            .fontColor($r('app.color.text_secondary'))
        }

        Card() {
          NavRow({
            title: tr('about_feedback_title'),
            desc: tr('about_feedback_desc'),
            icon: $r('sys.symbol.star'),
            external: true,
            onTap: () => {
              this.openStore();
            }
          })
        }
      }
      .width('100%')
      .constraintSize({ maxWidth: 720 })
      .padding({ left: 16, right: 16, top: 16, bottom: 32 + this.bottomReserve })
    }
    .width('100%')
    .height('100%')
    .align(Alignment.Top)
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    .bindSheet($$this.showLicense, this.LicenseSheet(), {
      title: { title: this.licenseTitle },
      height: SheetSize.LARGE,
      dragBar: true,
      backgroundColor: $r('app.color.card_bg')
    })
    // 内容延伸到底部小白条下面，滚动时从悬浮导航栏和小白条下透过去
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
  }
}
