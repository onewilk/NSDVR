import { netBoost } from '@kit.NetworkBoostKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

/**
 * 华为网络加速（Network Boost Kit）：告诉系统“正在实时游戏”，并把串流的两条 TCP 连接标成高优先级。
 * 系统据此可能关闭 Wi-Fi 省电、优先调度这些数据流。只影响本机接收这一侧，管不到 Switch 的发送端。
 * - setSceneDesc 从 API 20 开始提供；setDataFlowDesc 从 API 26 开始提供，低版本设备自动跳过
 */
export class NetBooster {
  // 给统计信息显示用
  status: string = tr('app_boost_off');

  private sceneActive: boolean = false;
  private flows: number[] = [];

  enter(): void {
    if (this.sceneActive) {
      return;
    }
    if (deviceInfo.sdkApiVersion < 20) {
      this.status = tr('app_boost_old');
      return;
    }
    try {
      netBoost.setSceneDesc({ scene: 'realtimeGame', sceneEvent: netBoost.SceneEvent.SCENE_EVENT_ENTER });
      this.sceneActive = true;
      this.status = tr('app_boost_scene');
    } catch (e) {
      const err = e as BusinessError;
      this.status = tr('app_boost_unavailable', err.code);
      hilog.warn(DOMAIN, TAG, 'netBoost setSceneDesc failed: %{public}d %{public}s', err.code, err.message);
    }
  }

  // 每秒随统计调用：连接建立或重连后 socket 会变，新的要重新标记
  updateFlows(fds: number[]): void {
    if (!this.sceneActive || deviceInfo.sdkApiVersion < 26) {
      return;
    }
    const fresh = fds.filter((fd: number) => this.flows.indexOf(fd) < 0);
    for (const fd of fresh) {
      try {
        netBoost.setDataFlowDesc({
          dataFlowInfo: fd,
          scene: 'realtimeGame',
          sceneEvent: netBoost.SceneEvent.SCENE_EVENT_ENTER,
          expectations: {
            downlinkBandwidth: 8000, // Kbps：视频约 4–5 Mbps + 音频 1.5 Mbps，留些余量
            latency: 50,             // ms
            priority: netBoost.PriorityLevel.PRIO_HIGH
          }
        });
      } catch (e) {
        const err = e as BusinessError;
        this.status = tr('app_boost_flow_failed', err.code);
        hilog.warn(DOMAIN, TAG, 'netBoost setDataFlowDesc failed: %{public}d %{public}s', err.code, err.message);
        return;
      }
    }
    this.flows = fds;
    if (fds.length > 0) {
      this.status = tr('app_boost_flows', fds.length);
    }
  }

  leave(): void {
    if (!this.sceneActive) {
      return;
    }
    try {
      netBoost.setSceneDesc({ scene: 'realtimeGame', sceneEvent: netBoost.SceneEvent.SCENE_EVENT_LEAVE });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'netBoost leave failed');
    }
    this.sceneActive = false;
    this.flows = [];
    this.status = tr('app_boost_off');
  }
}
