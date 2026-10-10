import { promptAction } from '@kit.ArkUI';
import sysdvr from 'libentry.so';
import { RtspProbeResult } from 'libentry.so';
import { tr } from '../common/I18n';
import { isValidIPv4, StreamMode } from '../common/Prefs';
import { RtspPlayer } from '../common/RtspPlayer';
import { StreamTarget } from '../common/StreamTarget';
import { RecentHost } from '../common/RecentHosts';
import { RecentHostsCard } from './RecentHostsCard';
import { Card } from './Widgets';

/** RTSP：SysDVR 在 Switch 上开 rtsp://IP:6666/，这里用 ijkplayer 播放；RTSP 模式下 Switch 不广播，只能手动填 IP */
@Component
export struct RtspPanel {
  @StorageLink('sysdvr.rtspHost') host: string = '';
  @StorageLink('sysdvr.lastHost') lastHost: string = '';
  @State probing: boolean = false;
  @State probeText: string = '';
  @State probeOk: boolean = false;
  onConnect: (target: StreamTarget) => void = () => {
  };

  aboutToAppear(): void {
    if (this.host.length === 0) {
      this.host = this.lastHost; // TCP Bridge 下连过的地址通常就是这台 Switch
    }
  }

  private connect(): void {
    const ip = this.host.trim();
    if (!isValidIPv4(ip)) {
      promptAction.showToast({ message: tr('connect_invalid_ip') });
      return;
    }
    this.host = ip;
    this.onConnect({ mode: StreamMode.RTSP, host: ip, label: RtspPlayer.urlOf(ip), usb: null });
  }

  /** 检测连接：按 SysDVR 服务端的流程走一遍再正常断开，判断是网络、模式、游戏还是播放器的问题 */
  private async probe(): Promise<void> {
    const ip = this.host.trim();
    if (!isValidIPv4(ip) || this.probing) {
      return;
    }
    this.probing = true;
    this.probeText = tr('rtsp_probe_running');
    const r: RtspProbeResult = await sysdvr.rtspProbe(ip, 3000);
    this.probeOk = r.error.length === 0;
    this.probeText = `${this.probeOk ? tr('rtsp_probe_ok') : r.error}\n${r.log.trim()}`;
    this.probing = false;
  }

  build() {
    Column({ space: 16 }) {
      Card({ title: tr('rtsp_address_title'), subtitle: tr('rtsp_address_subtitle') }) {
        Row({ space: 8 }) {
          TextInput({ placeholder: tr('rtsp_ip_hint'), text: this.host })
            .layoutWeight(1)
            .height(40)
            .type(InputType.URL)
            .onChange((value: string) => {
              this.host = value;
            })
            .onSubmit(() => {
              this.connect();
            })
          Button(tr('connect_button')).height(40).onClick(() => {
            this.connect();
          })
        }
        .width('100%')

        Row() {
          Text(tr('rtsp_will_open', RtspPlayer.urlOf(this.host.trim().length > 0 ? this.host.trim() : '<IP>')))
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .layoutWeight(1)
          if (this.probing) {
            LoadingProgress().width(24).height(24).color($r('app.color.accent'))
          } else {
            Button(tr('rtsp_probe_button'), { buttonStyle: ButtonStyleMode.TEXTUAL })
              .fontSize(13)
              .onClick(() => {
                this.probe();
              })
          }
        }
        .width('100%')

        if (this.probeText.length > 0) {
          Text(this.probeText)
            .fontSize(12)
            .lineHeight(19)
            .fontColor(this.probeOk ? $r('app.color.text_secondary') : $r('app.color.warn'))
            .width('100%')
            .padding(10)
            .backgroundColor($r('app.color.chip_bg'))
            .borderRadius(8)
            .copyOption(CopyOptions.LocalDevice)
        }
      }

      RecentHostsCard({
        mode: StreamMode.RTSP,
        onPick: (h: RecentHost) => {
          this.host = h.host;
          this.connect();
        }
      })
    }
  }
}
