import { usbManager } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { tr } from '../common/I18n';
import { StreamMode, usbHostSupported } from '../common/Prefs';
import { StreamTarget } from '../common/StreamTarget';
import {
  describeUsbDevice, describeUsbInfo, explainUsbScan, hex4, UsbLog, UsbScan, UsbSwitch, UsbWatcher
} from '../common/UsbSwitch';
import { Card, Chip } from './Widgets';

// USB 根集线器（Linux Foundation，VID 1D6B）是系统自带的，不算外接设备
const ROOT_HUB_VID = 0x1D6B;

/** USB：检测插着的 SysDVR 设备 → 授权 → 读取设备信息 → 连接。根节点就是设备卡片（放在串流页底部） */
@Component
export struct UsbPanel {
  @Prop @Watch('onActiveChange') active: boolean = false;
  @State device: usbManager.USBDevice | undefined = undefined;
  @State detail: string = '';
  @State busy: boolean = false;
  // 未检测到时的诊断：系统看到的外接设备和原因
  @State scanText: string = '';
  @State hintText: string = '';
  @State showDiag: boolean = false;
  onConnect: (target: StreamTarget) => void = () => {
  };
  private timer: number = -1;
  private watcher: UsbWatcher = new UsbWatcher();
  private lastScanKey: string = '';

  aboutToAppear(): void {
    UsbLog.init((getContext(this) as common.UIAbilityContext).filesDir);
    this.onActiveChange();
  }

  aboutToDisappear(): void {
    this.stopWatch();
  }

  onActiveChange(): void {
    if (this.active && usbHostSupported()) {
      this.refresh();
      // 插拔事件为主，定时查询兜底（部分设备收不到公共事件）
      this.watcher.start(() => {
        UsbLog.write('usb attach/detach event');
        this.refresh();
      });
      if (this.timer < 0) {
        this.timer = setInterval(() => {
          this.refresh();
        }, 2000);
      }
    } else {
      this.stopWatch();
    }
  }

  private stopWatch(): void {
    this.watcher.stop();
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private refresh(): void {
    const scan = UsbSwitch.scan();
    const external: UsbScan = {
      devices: scan.devices.filter((d: usbManager.USBDevice) => d.vendorId !== ROOT_HUB_VID),
      error: scan.error
    };
    // 设备列表变化时记一次日志，便于事后分析
    const key = `${scan.error}|` + scan.devices.map((d: usbManager.USBDevice) => describeUsbDevice(d)).join(';');
    if (key !== this.lastScanKey) {
      this.lastScanKey = key;
      UsbLog.write(`scan: ${scan.error.length > 0 ? 'error ' + scan.error : scan.devices.length + ' device(s)'} ` +
        scan.devices.map((d: usbManager.USBDevice) => `[${describeUsbDevice(d)}]`).join(' '));
    }
    const d = scan.devices.find((x: usbManager.USBDevice) =>
    x.vendorId === 0x18D1 && x.productId === 0x4EE0);
    if (d?.name !== this.device?.name) {
      this.detail = '';
    }
    this.device = d;
    if (d === undefined) {
      this.scanText = external.devices.length === 0 ? tr('usb_external_none') :
        tr('usb_external_list') + '\n' +
          external.devices.map((x: usbManager.USBDevice) => describeUsbDevice(x)).join('\n');
      this.hintText = explainUsbScan(external);
    }
  }

  /** 读取设备信息（授权后打开、读序列号、判断传输方式，然后关闭） */
  private async inspect(): Promise<void> {
    const d = this.device;
    if (d === undefined || this.busy) {
      return;
    }
    this.busy = true;
    const r = await UsbSwitch.open(d);
    if (r.link === null) {
      this.detail = r.error;
    } else {
      const l = r.link;
      this.detail = `${describeUsbInfo(l.info)}\n` +
        tr('usb_endpoints', l.iface.id, l.epIn.address.toString(16), l.epOut.address.toString(16),
          l.epIn.maxPacketSize) + '\n' + tr('usb_transport', l.transport) +
        (l.transportNote.length > 0 ? `\n${l.transportNote}` : '');
      UsbLog.write(`inspect: ${this.detail.replace(/\n/g, ' | ')}`);
      UsbSwitch.close(l);
    }
    this.busy = false;
  }

  private async connect(): Promise<void> {
    const d = this.device;
    if (d === undefined || this.busy) {
      return;
    }
    this.busy = true;
    const r = await UsbSwitch.open(d);
    this.busy = false;
    if (r.link === null) {
      this.detail = r.error;
      promptAction.showToast({ message: r.error });
      return;
    }
    const l = r.link;
    this.detail = `${describeUsbInfo(l.info)}\n` + tr('usb_transport', l.transport) +
      (l.transportNote.length > 0 ? `\n${l.transportNote}` : '');
    UsbLog.write(`connect: ${this.detail.replace(/\n/g, ' | ')}`);
    this.onConnect({ mode: StreamMode.USB, host: '', label: `USB · SysDVR ${l.info.version}`, usb: l });
  }

  build() {
    Card({ title: tr('usb_card_title'), subtitle: tr('usb_card_subtitle') }) {
      if (!usbHostSupported()) {
        // 部分 PC、平板没有 USB 主机能力
        Text(tr('usb_unsupported'))
          .fontSize(13)
          .lineHeight(20)
          .fontColor($r('app.color.warn'))
          .width('100%')
      } else if (this.device === undefined) {
        Row({ space: 8 }) {
          LoadingProgress().width(20).height(20).color($r('app.color.accent'))
          Text(tr('usb_not_detected'))
            .fontSize(13)
            .fontColor($r('app.color.text_tertiary'))
            .layoutWeight(1)
        }
        .width('100%')

        if (this.hintText.length > 0) {
          Text(this.hintText)
            .fontSize(12)
            .lineHeight(19)
            .fontColor($r('app.color.warn'))
            .width('100%')
            .padding(10)
            .backgroundColor($r('app.color.warn_bg'))
            .borderRadius(8)
        }

        Button(this.showDiag ? tr('usb_diag_hide') : tr('usb_diag_show'), { buttonStyle: ButtonStyleMode.TEXTUAL })
          .fontSize(13)
          .onClick(() => {
            this.showDiag = !this.showDiag;
          })
        if (this.showDiag) {
          Text(`${this.scanText}\n${tr('usb_diag_log')}`)
            .fontSize(12)
            .lineHeight(19)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
            .padding(10)
            .backgroundColor($r('app.color.chip_bg'))
            .borderRadius(8)
            .copyOption(CopyOptions.LocalDevice)
        }
      } else {
        Row({ space: 12 }) {
          SymbolGlyph($r('sys.symbol.charg_cable')).fontSize(24).fontColor([$r('app.color.accent')])
          Column({ space: 2 }) {
            Row({ space: 6 }) {
              Text(tr('usb_device_title')).fontSize(15).fontColor($r('app.color.text_primary'))
              Chip({ text: UsbSwitch.hasRight(this.device) ? tr('usb_authorized') : tr('usb_unauthorized') })
            }
            Text(tr('usb_device_ids', hex4(this.device.vendorId), hex4(this.device.productId), this.device.name))
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          if (this.busy) {
            LoadingProgress().width(28).height(28).color($r('app.color.accent'))
          } else {
            Button(tr('connect_button')).height(34).fontSize(14).onClick(() => {
              this.connect();
            })
          }
        }
        .width('100%')

        if (this.detail.length > 0) {
          Text(this.detail)
            .fontSize(12)
            .lineHeight(19)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
            .padding(10)
            .backgroundColor($r('app.color.chip_bg'))
            .borderRadius(8)
            .copyOption(CopyOptions.LocalDevice)
        }

        Button(tr('usb_read_info'), { buttonStyle: ButtonStyleMode.TEXTUAL })
          .fontSize(13)
          .enabled(!this.busy)
          .onClick(() => {
            this.inspect();
          })
      }
    }
  }
}
