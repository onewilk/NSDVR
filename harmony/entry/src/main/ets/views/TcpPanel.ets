import { promptAction } from '@kit.ArkUI';
import sysdvr from 'libentry.so';
import { DeviceInfo } from 'libentry.so';
import { tr } from '../common/I18n';
import { isValidIPv4, StreamMode } from '../common/Prefs';
import { StreamTarget } from '../common/StreamTarget';
import { Card } from './Widgets';

/** TCP Bridge：自动发现（UDP 19999 广播）+ 手动输入 IP */
@Component
export struct TcpPanel {
  // 只有“当前是 TCP 模式且不在播放”时才监听广播
  @Prop @Watch('onActiveChange') active: boolean = false;
  @StorageLink('sysdvr.lastHost') manualIp: string = '';
  @State devices: DeviceInfo[] = [];
  @State discoveryOk: boolean = true;
  onConnect: (target: StreamTarget) => void = () => {
  };
  private scanTimer: number = -1;

  aboutToAppear(): void {
    this.onActiveChange();
  }

  aboutToDisappear(): void {
    this.stopScan();
  }

  onActiveChange(): void {
    if (this.active) {
      this.startScan();
    } else {
      this.stopScan();
    }
  }

  private startScan(): void {
    if (this.scanTimer >= 0) {
      return;
    }
    this.discoveryOk = sysdvr.startDiscovery();
    this.devices = sysdvr.getDevices();
    this.scanTimer = setInterval(() => {
      this.devices = sysdvr.getDevices();
    }, 1000);
  }

  private stopScan(): void {
    if (this.scanTimer < 0) {
      return;
    }
    clearInterval(this.scanTimer);
    this.scanTimer = -1;
    sysdvr.stopDiscovery();
  }

  private connect(host: string): void {
    const ip = host.trim();
    if (!isValidIPv4(ip)) {
      promptAction.showToast({ message: tr('connect_invalid_ip') });
      return;
    }
    this.manualIp = ip; // 记住，下次不用重新输入
    this.onConnect({ mode: StreamMode.TCP, host: ip, label: ip, usb: null });
  }

  build() {
    Column() {
      Card({
        title: this.discoveryOk ? tr('connect_lan_title') : tr('connect_discovery_unavailable'),
        subtitle: this.discoveryOk ? tr('connect_lan_subtitle') : tr('connect_discovery_port_busy')
      }) {
        if (this.devices.length === 0 && this.discoveryOk) {
          Row({ space: 8 }) {
            LoadingProgress().width(20).height(20).color($r('app.color.accent'))
            Text(tr('connect_searching')).fontSize(13).fontColor($r('app.color.text_tertiary'))
          }
        }
        // 设备多时列表内部滚动，卡片不会把上面的内容挤没
        List({ space: 8 }) {
        ForEach(this.devices, (d: DeviceInfo) => {
          ListItem() {
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(24).fontColor([$r('app.color.accent')])
            Column({ space: 2 }) {
              Text(tr('connect_device_name', d.version, d.protocol))
                .fontSize(15)
                .fontColor($r('app.color.text_primary'))
              Text(`${d.ip} · ${d.serial}`).fontSize(12).fontColor($r('app.color.text_secondary'))
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Button(tr('connect_button')).height(34).fontSize(14).onClick(() => {
              this.connect(d.ip);
            })
          }
          .width('100%')
          .padding(12)
          .borderRadius(12)
          .backgroundColor($r('app.color.chip_bg'))
          }
        }, (d: DeviceInfo) => d.ip)
        }
        .width('100%')
        .constraintSize({ maxHeight: 200 })
        .scrollBar(BarState.Off)
        .visibility(this.devices.length > 0 ? Visibility.Visible : Visibility.None)

        Row({ space: 8 }) {
          TextInput({ placeholder: tr('connect_manual_ip_hint'), text: this.manualIp })
            .layoutWeight(1)
            .height(40)
            .type(InputType.URL)
            .onChange((value: string) => {
              this.manualIp = value;
            })
            .onSubmit(() => {
              this.connect(this.manualIp);
            })
          Button(tr('connect_button')).height(40).onClick(() => {
            this.connect(this.manualIp);
          })
        }
        .width('100%')
      }
    }
  }
}
