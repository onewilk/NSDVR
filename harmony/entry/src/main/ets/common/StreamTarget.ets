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
}
