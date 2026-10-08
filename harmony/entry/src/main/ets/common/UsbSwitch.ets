import { usbManager, commonEventManager, BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo } from '@kit.CoreFileKit';
import sysdvr from 'libentry.so';
import { UsbProbeResult } from 'libentry.so';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

// SysDVR 的 USB 模式借用了 Google Nexus 的 VID/PID（这样 Windows 可以直接用签名过的 WinUSB 驱动）
export const SYSDVR_VID = 0x18D1;
export const SYSDVR_PID = 0x4EE0;
// 任天堂的 VID：Switch 在非 SysDVR 模式下（例如其他自制程序占用 USB）会以这个 VID 出现
const NINTENDO_VID = 0x057E;
const BULK = 2;
const DIR_IN = 0x80;
// usbManager.bulkTransfer 单次（含参数）要求 < 200KB；取 512 的整数倍
const PUMP_CHUNK = 128 * 1024;

/** 从序列号 "SysDVR|6.3|03|XAW10000000000" 解析出的设备信息 */
export interface UsbSwitchInfo {
  version: string;
  protocol: string;
  serial: string;
}

/** 打开后的 USB 连接 */
export interface UsbLink {
  device: usbManager.USBDevice;
  pipe: usbManager.USBDevicePipe;
  iface: usbManager.USBInterface;
  epIn: usbManager.USBEndpoint;
  epOut: usbManager.USBEndpoint;
  fd: number;
  /** true：系统不允许直接 ioctl，改用 usbManager.bulkTransfer 兼容传输 */
  pump: boolean;
  info: UsbSwitchInfo;
  /** 给用户看的传输方式说明 */
  transport: string;
  /** 传输方式的补充说明（灰色小字），没有就是空串 */
  transportNote: string;
}

export interface UsbOpenResult {
  link: UsbLink | null;
  error: string;
}

export function parseSysdvrSerial(serial: string): UsbSwitchInfo | null {
  const parts = serial.trim().split('|');
  if (parts.length < 3 || parts[0] !== 'SysDVR') {
    return null;
  }
  return { version: parts[1], protocol: parts[2], serial: parts.length > 3 ? parts[3] : '' };
}

export function describeUsbInfo(info: UsbSwitchInfo): string {
  return tr('connect_device_name', info.version, info.protocol) + (info.serial.length > 0 ? ' · ' + info.serial : '');
}

function errText(e: Object): string {
  const err = e as BusinessError;
  return err.code !== undefined ? `${err.code} ${err.message}` : JSON.stringify(e);
}

export function hex4(n: number): string {
  return ('0000' + n.toString(16).toUpperCase()).slice(-4);
}

/**
 * USB 诊断日志：写到应用沙箱 files/usb.log（这台正式版手机上看不到 App 的 hilog）。拉取：
 *   hdc file recv -b wilk.sysdvr.next /data/storage/el2/base/haps/entry/files/usb.log ./
 */
export class UsbLog {
  private static dir: string = '';

  static init(filesDir: string): void {
    UsbLog.dir = filesDir;
  }

  static write(line: string): void {
    hilog.info(DOMAIN, TAG, 'usb: %{public}s', line);
    if (UsbLog.dir.length === 0) {
      return;
    }
    const path = `${UsbLog.dir}/usb.log`;
    try {
      let flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.APPEND;
      if (fileIo.accessSync(path) && fileIo.statSync(path).size > 512 * 1024) {
        flags = fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      }
      const f = fileIo.openSync(path, flags);
      fileIo.writeSync(f.fd, `${new Date().toISOString()} ${line}\n`);
      fileIo.closeSync(f);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'write usb.log failed');
    }
  }
}

/** 系统当前看到的 USB 设备（诊断用） */
export interface UsbScan {
  devices: usbManager.USBDevice[];
  /** getDevices 抛异常时的错误信息 */
  error: string;
}

export function describeUsbDevice(d: usbManager.USBDevice): string {
  const names = [d.manufacturerName, d.productName].filter((s: string) => s !== undefined && s.length > 0).join(' ');
  return tr('usb_scan_device', `${hex4(d.vendorId)}:${hex4(d.productId)}`, d.clazz, d.name) +
    (names.length > 0 ? ' · ' + names : '');
}

/** 根据扫描结果给出“为什么没检测到”的判断和下一步 */
export function explainUsbScan(scan: UsbScan): string {
  if (scan.error.length > 0) {
    return tr('usb_hint_list_failed', scan.error);
  }
  if (scan.devices.length === 0) {
    // 实测（Mate 60 Pro + C-C 线）：端口协商成 UFP/数据从设备，Switch 当主机；
    // 之后即使供电方向变成手机给 Switch 供电，数据角色仍是从设备，所以只能靠 OTG 转接头在插入时定好角色
    // PC 的 USB 口默认就是主机口，不存在手机/平板 C-C 线直连时被协商成从设备的问题
    return deviceInfo.deviceType === '2in1' ? tr('usb_hint_no_devices_pc') : tr('usb_hint_no_devices');
  }
  if (scan.devices.some((d: usbManager.USBDevice) => d.vendorId === NINTENDO_VID)) {
    return tr('usb_hint_nintendo');
  }
  return tr('usb_hint_no_sysdvr');
}

export class UsbSwitch {
  static scan(): UsbScan {
    try {
      return { devices: usbManager.getDevices(), error: '' };
    } catch (e) {
      return { devices: [], error: errText(e) };
    }
  }

  /** 当前插着的 SysDVR 设备（Switch 上的 SysDVR 设为 USB 模式、用数据线连到本机） */
  static find(): usbManager.USBDevice | undefined {
    try {
      return usbManager.getDevices().find((d: usbManager.USBDevice) =>
      d.vendorId === SYSDVR_VID && d.productId === SYSDVR_PID);
    } catch (e) {
      // 不支持 USB 主机的设备（例如部分 PC 形态）会抛 801
      return undefined;
    }
  }

  static hasRight(device: usbManager.USBDevice): boolean {
    try {
      return usbManager.hasRight(device.name);
    } catch (e) {
      return false;
    }
  }

  /** 申请授权（系统弹窗）→ 打开设备 → 找到 bulk 端点 → 探测能否直接 ioctl，不能就准备兼容传输 */
  static async open(device: usbManager.USBDevice): Promise<UsbOpenResult> {
    try {
      UsbLog.write(`open ${describeUsbDevice(device)} configs=${device.configs.length}`);
      if (!usbManager.hasRight(device.name)) {
        const granted = await usbManager.requestRight(device.name);
        UsbLog.write(`requestRight → ${granted}`);
        if (!granted) {
          return { link: null, error: tr('usb_err_no_right') };
        }
      }
      const iface = UsbSwitch.pickInterface(device);
      if (iface === undefined) {
        UsbLog.write('no interface');
        return { link: null, error: tr('usb_err_no_iface') };
      }
      UsbLog.write(`iface id=${iface.id} class=${iface.clazz} endpoints=` +
        iface.endpoints.map((e: usbManager.USBEndpoint) =>
        `0x${e.address.toString(16)}/type${e.type}/dir${e.direction}/mps${e.maxPacketSize}`).join(','));
      const epIn = iface.endpoints.find((e: usbManager.USBEndpoint) => e.type === BULK && e.direction === DIR_IN);
      const epOut = iface.endpoints.find((e: usbManager.USBEndpoint) => e.type === BULK && e.direction !== DIR_IN);
      if (epIn === undefined || epOut === undefined) {
        return { link: null, error: tr('usb_err_no_bulk') };
      }
      const pipe = usbManager.connectDevice(device);
      if (pipe === undefined) {
        UsbLog.write('connectDevice → undefined');
        return { link: null, error: tr('usb_err_open_service') };
      }
      const fd = usbManager.getFileDescriptor(pipe);
      UsbLog.write(`connectDevice ok, getFileDescriptor → ${fd}`);
      hilog.info(DOMAIN, TAG, 'usb opened: fd=%{public}d iface=%{public}d in=%{public}x out=%{public}x', fd, iface.id,
        epIn.address, epOut.address);

      // 1) 直接对 usbfs 描述符发 ioctl：零拷贝、没有跨进程开销
      const noFd: UsbProbeResult = { ok: false, serial: '', error: `fd=${fd}`, bulkRefused: false };
      let probe: UsbProbeResult = fd >= 0 ? sysdvr.usbProbe(fd, iface.id, epIn.address) : noFd;
      let pump = false;
      let transport = tr('usb_transport_direct');
      let note = '';
      UsbLog.write(`usbfs probe → ok=${probe.ok} serial="${probe.serial}" error="${probe.error}"`);
      if (!probe.ok) {
        // 2) 先让 USB 服务 claim 接口，再试一次（有的系统要求先经服务声明接口）
        const claim = usbManager.claimInterface(pipe, iface, true);
        // bulk 被系统拒绝（EACCES）时重试也没用；而且 fd 可能和服务共用，重试失败时的 release 会把服务刚声明的接口也放掉
        // 序列号读到了（控制传输可用）却仍失败，说明卡在最后一步的 bulk 试读——不依赖错误文字，多语言下也成立
        // native 探测直接告诉我们 bulk 是否被系统拒绝（EACCES）；不依赖错误文字，换语言也不受影响
        const bulkRefused = probe.bulkRefused;
        const retry = fd >= 0 && claim === 0 && !bulkRefused ? sysdvr.usbProbe(fd, iface.id, epIn.address) : probe;
        UsbLog.write(`claimInterface → ${claim}; retry probe ok=${retry.ok} error="${retry.error}"`);
        if (claim !== 0) {
          UsbLog.write('warning: USB 服务声明接口失败，兼容传输可能无法收发');
        }
        if (retry.ok) {
          probe = retry;
          transport = tr('usb_transport_direct_claimed');
        } else {
          // 3) 兼容传输：ArkTS 调 bulkTransfer 收发，序列号用控制传输读
          pump = true;
          // 界面上只说结论；具体原因（如 bulk 被系统拒绝 EACCES）已写进 usb.log
          transport = tr('usb_transport_compat');
          note = tr('usb_transport_compat_note');
          // usbfs 控制传输能用时序列号已经读到了，只是 bulk 不行
          const serial = probe.serial.length > 0 ? probe.serial : await UsbSwitch.readSerialViaService(pipe);
          UsbLog.write(`fallback to bulkTransfer pump, serial via service="${serial}"`);
          probe = { ok: serial.length > 0, serial: serial, error: '', bulkRefused: true };
        }
      }
      const info = parseSysdvrSerial(probe.serial) ?? { version: '?', protocol: '?', serial: probe.serial };
      const link: UsbLink = {
        device: device, pipe: pipe, iface: iface, epIn: epIn, epOut: epOut, fd: fd, pump: pump, info: info,
        transport: transport,
        transportNote: note
      };
      return { link: link, error: '' };
    } catch (e) {
      UsbLog.write(`open failed: ${errText(e)}`);
      return { link: null, error: tr('usb_err_open', errText(e)) };
    }
  }

  static close(link: UsbLink): void {
    try {
      if (link.pump) {
        usbManager.releaseInterface(link.pipe, link.iface);
      }
      usbManager.closePipe(link.pipe);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'usb close failed: %{public}s', errText(e));
    }
  }

  private static pickInterface(device: usbManager.USBDevice): usbManager.USBInterface | undefined {
    const config = device.configs.length > 0 ? device.configs[0] : undefined;
    if (config === undefined) {
      return undefined;
    }
    // SysDVR 只有一个厂商自定义接口（class 0xFF）
    return config.interfaces.find((i: usbManager.USBInterface) => i.clazz === 0xFF) ?? config.interfaces[0];
  }

  /** 经 USB 服务读序列号字符串描述符（兼容传输时用） */
  private static async readSerialViaService(pipe: usbManager.USBDevicePipe): Promise<string> {
    try {
      const raw = usbManager.getRawDescriptor(pipe);
      const index = raw.length >= 18 ? raw[16] : 0;
      if (index === 0) {
        return '';
      }
      const buf = new Uint8Array(255);
      const n = await usbManager.usbControlTransfer(pipe, {
        bmRequestType: 0x80, bRequest: 0x06, wValue: 0x0300 | index, wIndex: 0x0409, wLength: 255, data: buf
      }, 1000);
      let s = '';
      const len = Math.min(n, buf[0]);
      for (let i = 2; i + 1 < len; i += 2) {
        s += buf[i + 1] === 0 ? String.fromCharCode(buf[i]) : '?';
      }
      return s;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'read serial via service failed: %{public}s', errText(e));
      return '';
    }
  }
}

/**
 * 兼容传输：ArkTS 循环读 IN 端点，把数据交给 native 的 UsbStreamClient 解析；
 * native 要写 OUT 端点时（只有握手请求）通过 onWrite 回调通知这里来取数据发送。
 */
export class UsbPump {
  private link: UsbLink | null = null;
  private running: boolean = false;
  private buffer: Uint8Array = new Uint8Array(PUMP_CHUNK);

  start(link: UsbLink): void {
    this.link = link;
    this.running = true;
    this.readLoop();
  }

  stop(): void {
    this.running = false;
    this.link = null;
  }

  /** 传给 sysdvr.connect 的 onUsbWrite */
  onWrite = (): void => {
    const link = this.link;
    const data = sysdvr.usbTakeWrite();
    if (link === null || data === undefined) {
      return;
    }
    usbManager.bulkTransfer(link.pipe, link.epOut, new Uint8Array(data), 1500).then((n: number) => {
      sysdvr.usbWriteDone(n);
    }).catch(() => {
      sysdvr.usbWriteDone(-1);
    });
  };

  private async readLoop(): Promise<void> {
    let failures = 0;
    while (this.running) {
      const link = this.link;
      if (link === null) {
        break;
      }
      let n = -1;
      try {
        n = await usbManager.bulkTransfer(link.pipe, link.epIn, this.buffer, 500);
      } catch (e) {
        n = -1;
      }
      if (!this.running) {
        break;
      }
      if (n > 0) {
        failures = 0;
        sysdvr.usbFeed(this.buffer.buffer, n);
      } else if (n < 0) {
        // 超时也返回 -1，连续多次失败再看设备还在不在
        failures++;
        if (failures % 10 === 0 && UsbSwitch.find() === undefined) {
          sysdvr.usbFeedError();
        }
      }
    }
  }
}

/** 监听 USB 插拔（系统公共事件） */
export class UsbWatcher {
  private subscriber: commonEventManager.CommonEventSubscriber | null = null;

  async start(onChange: () => void): Promise<void> {
    if (this.subscriber !== null) {
      return;
    }
    try {
      this.subscriber = await commonEventManager.createSubscriber({
        events: [commonEventManager.Support.COMMON_EVENT_USB_DEVICE_ATTACHED,
          commonEventManager.Support.COMMON_EVENT_USB_DEVICE_DETACHED]
      });
      commonEventManager.subscribe(this.subscriber, () => {
        onChange();
      });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'subscribe usb events failed: %{public}s', errText(e));
    }
  }

  stop(): void {
    const s = this.subscriber;
    if (s !== null) {
      commonEventManager.unsubscribe(s);
      this.subscriber = null;
    }
  }
}
