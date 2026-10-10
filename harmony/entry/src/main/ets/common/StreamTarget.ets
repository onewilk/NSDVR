import { StreamMode } from './Prefs';
import { UsbLink } from './UsbSwitch';

/** 用户选好的串流目标，交给播放页去连接 */
export interface StreamTarget {
  mode: StreamMode;
  /** TCP Bridge / RTSP：Switch 的 IP */
  host: string;
  /** 状态提示、实况窗里显示的名字 */
  label: string;
  /** USB：已授权并打开的设备 */
  usb: UsbLink | null;
  /** 局域网发现到的设备名（如“SysDVR 6.3（协议 03）”），记进“最近连接”；手动输入时不填 */
  name?: string;
}
