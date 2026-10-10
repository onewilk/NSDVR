import { SymbolGlyphModifier } from '@kit.ArkUI';
import { tr } from '../common/I18n';
import { StreamMode } from '../common/Prefs';
import { forgetHost, RecentHost, recentHostsFor, timeAgo } from '../common/RecentHosts';
import { Card } from './Widgets';

/** 串流页“最近连接”：当前模式下最近连上过的设备（最多 5 条），点“连接”直接重连，长按一条弹出“删除”。没有记录时不显示 */
@Component
export struct RecentHostsCard {
  @Prop mode: number = StreamMode.TCP;
  @StorageLink('sysdvr.recentHosts') raw: string = '';
  onPick: (h: RecentHost) => void = () => {
  };

  @Builder
  RowMenu(h: RecentHost) {
    Menu() {
      MenuItem({ symbolStartIcon: new SymbolGlyphModifier($r('sys.symbol.trash')), content: tr('recent_delete') })
        .onClick(() => {
          forgetHost(h.mode, h.host);
        })
    }
  }

  build() {
    if (recentHostsFor(this.raw, this.mode).length > 0) {
      Card({ title: tr('recent_title') }) {
        Column({ space: 8 }) {
          ForEach(recentHostsFor(this.raw, this.mode), (h: RecentHost) => {
            Row({ space: 12 }) {
              SymbolGlyph($r('sys.symbol.clock')).fontSize(22).fontColor([$r('app.color.accent')])
              Column({ space: 2 }) {
                Text(h.name.length > 0 ? h.name : h.host)
                  .fontSize(15)
                  .fontColor($r('app.color.text_primary'))
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                Text(h.name.length > 0 ? `${h.host} · ${timeAgo(h.at)}` : timeAgo(h.at))
                  .fontSize(12)
                  .fontColor($r('app.color.text_secondary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Button(tr('connect_button')).height(34).fontSize(14).onClick(() => {
                this.onPick(h);
              })
            }
            .width('100%')
            .padding(12)
            .borderRadius(12)
            .backgroundColor($r('app.color.chip_bg'))
            .bindContextMenu(this.RowMenu(h), ResponseType.LongPress)
          }, (h: RecentHost) => `${h.mode}-${h.host}-${h.at}`)
        }
        .width('100%')
      }
    }
  }
}
