import { netBoost } from '@kit.NetworkBoostKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SocketFlow } from 'libentry.so';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

function sameFlow(a: SocketFlow, b: SocketFlow): boolean {
  return a.localPort === b.localPort && a.remotePort === b.remotePort && a.localAddress === b.localAddress &&
    a.remoteAddress === b.remoteAddress;
}

/**
 * 华为网络加速（Network Boost Kit）：告诉系统“正在实时游戏”，并把串流的两条 TCP 连接标成高优先级。
 * 系统据此可能关闭 Wi-Fi 省电、优先调度这些数据流。只影响本机接收这一侧，管不到 Switch 的发送端。
 * - setSceneDesc 从 API 20 开始提供；setDataFlowDesc 从 API 26 开始提供，低版本设备自动跳过
 * - 数据流用地址五元组标记，不能传 socket 描述符：系统服务会持有传过去的描述符，我们 close() 后连接并不断开，
 *   游戏机要等手机这边的接收缓冲塞满、再超时 10 秒才放弃（Opus 音频码率低，要一分多钟），这期间重连会被拒绝
 * - 连接断开或离开场景时，对标记过的数据流逐条发 LEAVE
 */
export class NetBooster {
  // 给统计信息显示用
  status: string = tr('app_boost_off');

  private sceneActive: boolean = false;
  private flows: SocketFlow[] = [];

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

  // 每秒随统计调用：连接建立或重连后地址会变，新的要标记，消失的要撤销
  updateFlows(current: SocketFlow[]): void {
    if (!this.sceneActive || deviceInfo.sdkApiVersion < 26) {
      return;
    }
    const kept: SocketFlow[] = [];
    for (const old of this.flows) {
      if (current.some((f: SocketFlow) => sameFlow(f, old))) {
        kept.push(old);
      } else {
        this.markFlow(old, netBoost.SceneEvent.SCENE_EVENT_LEAVE);
      }
    }
    for (const flow of current) {
      if (kept.some((f: SocketFlow) => sameFlow(f, flow))) {
        continue;
      }
      if (!this.markFlow(flow, netBoost.SceneEvent.SCENE_EVENT_ENTER)) {
        this.flows = kept;
        return;
      }
      kept.push(flow);
    }
    this.flows = kept;
    if (kept.length > 0) {
      this.status = tr('app_boost_flows', kept.length);
    }
  }

  private markFlow(flow: SocketFlow, event: netBoost.SceneEvent): boolean {
    try {
      const desc: netBoost.DataFlowDesc = {
        dataFlowInfo: {
          protocol: netBoost.ProtocolType.PROTOCOL_TCP,
          local: { address: flow.localAddress, port: flow.localPort },
          remote: { address: flow.remoteAddress, port: flow.remotePort }
        },
        scene: 'realtimeGame',
        sceneEvent: event
      };
      if (event === netBoost.SceneEvent.SCENE_EVENT_ENTER) {
        desc.expectations = {
          downlinkBandwidth: 8000, // Kbps：视频约 4–5 Mbps + 音频 1.5 Mbps，留些余量
          latency: 50,             // ms
          priority: netBoost.PriorityLevel.PRIO_HIGH
        };
      }
      netBoost.setDataFlowDesc(desc);
      return true;
    } catch (e) {
      const err = e as BusinessError;
      if (event === netBoost.SceneEvent.SCENE_EVENT_ENTER) {
        this.status = tr('app_boost_flow_failed', err.code);
      }
      hilog.warn(DOMAIN, TAG, 'netBoost setDataFlowDesc(%{public}d) failed: %{public}d %{public}s', event, err.code,
        err.message);
      return false;
    }
  }

  leave(): void {
    if (!this.sceneActive) {
      return;
    }
    for (const flow of this.flows) {
      this.markFlow(flow, netBoost.SceneEvent.SCENE_EVENT_LEAVE);
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
