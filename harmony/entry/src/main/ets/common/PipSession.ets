// 画中画（小窗）：TCP Bridge / USB 播放时把画面放进系统小窗继续播放（RTSP 由 ijkplayer 管理 surface，不支持）。
// 用 XComponent 方案：系统把播放器的 surface 挪进小窗，解码器照常往同一个 surface 上画，不用重连、不重建解码器。
// 播放页只有一个页面（播放层盖在 Navigation 上面，不在路由栈里），所以不传 navigationId。
import { PiPWindow } from '@kit.ArkUI';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';
// 画面固定 1280×720（px），小窗按这个比例显示
const CONTENT_WIDTH = 1280;
const CONTENT_HEIGHT = 720;

export class PipSession {
  private controller: PiPWindow.PiPController | null = null;
  // 每次 open/close 加一：create 还没返回会话就结束了，返回后把这个控制器关掉
  private generation: number = 0;
  private autoStart: boolean = false;
  private audioOn: boolean = true;
  // 小窗从开始弹出到关闭（ABOUT_TO_START ～ STOPPED），以及手动 startPiP 还没返回的这段时间
  private active: boolean = false;
  private starting: boolean = false;
  // 用户点了“恢复全屏”：随后的 STOPPED 是回到播放页，不是关闭小窗
  private restoring: boolean = false;
  /** 用户关掉了小窗（点关闭、拖到垃圾桶等），不包括恢复全屏 */
  onClosed: () => void = () => {
  };
  /** 小窗里点了静音按钮：on = 打开声音 */
  onAudioToggled: (on: boolean) => void = () => {
  };

  /** 设备支持并且系统设置里开着画中画 */
  static supported(): boolean {
    // PC 从 HarmonyOS 6.0（API 20）起才支持 XComponent 画中画
    if (deviceInfo.deviceType === '2in1' && deviceInfo.sdkApiVersion < 20) {
      return false;
    }
    try {
      return PiPWindow.isPiPEnabled();
    } catch (e) {
      return false;
    }
  }

  /** 播放器 surface 就绪后调用：创建控制器，按设置决定切后台时是否自动转入小窗 */
  open(context: Context, xController: XComponentController, autoStart: boolean, audioOn: boolean): void {
    this.close();
    if (!PipSession.supported()) {
      return;
    }
    const gen = this.generation;
    this.autoStart = autoStart;
    this.audioOn = audioOn;
    const config: PiPWindow.PiPConfiguration = {
      context: context,
      componentController: xController,
      templateType: PiPWindow.PiPTemplateType.VIDEO_LIVE,
      contentWidth: CONTENT_WIDTH,
      contentHeight: CONTENT_HEIGHT,
      controlGroups: [PiPWindow.VideoLiveControlGroup.MUTE_SWITCH]
    };
    PiPWindow.create(config).then((controller: PiPWindow.PiPController) => {
      if (gen !== this.generation) {
        controller.stopPiP().catch(() => {
        });
        return;
      }
      this.controller = controller;
      controller.on('stateChange', (state: PiPWindow.PiPState, reason: string) => {
        this.onStateChange(state, reason);
      });
      controller.on('controlPanelActionEvent', (event: PiPWindow.PiPActionEventType, status?: number) => {
        hilog.info(DOMAIN, TAG, 'pip action %{public}s status=%{public}d', event, status ?? -1);
        if (event === 'voiceStateChanged' && status !== undefined) {
          this.audioOn = status === 1;
          this.onAudioToggled(this.audioOn);
        }
      });
      controller.setAutoStartEnabled(this.autoStart);
    }).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'pip create failed: %{public}d %{public}s', e.code, e.message);
    });
  }

  /** 会话结束：关掉小窗、注销回调 */
  close(): void {
    this.generation++;
    const c = this.controller;
    this.controller = null;
    this.active = false;
    this.starting = false;
    this.restoring = false;
    if (c === null) {
      return;
    }
    try {
      c.setAutoStartEnabled(false);
      c.off('stateChange');
      c.off('controlPanelActionEvent');
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'pip off failed: %{public}s', JSON.stringify(e));
    }
    c.stopPiP().catch(() => {
      // 没在小窗里时 stopPiP 会报错，忽略
    });
  }

  /** 小窗正在显示（或正要弹出）：这时播放页虽然在后台，画面仍要照常渲染 */
  isActive(): boolean {
    return this.active || this.starting;
  }

  /** 切后台时系统会不会自动转入小窗（是否真的转入还取决于系统设置，要看随后的 stateChange） */
  autoStartArmed(): boolean {
    return this.controller !== null && this.autoStart;
  }

  setAutoStart(on: boolean): void {
    this.autoStart = on;
    if (this.controller !== null) {
      this.controller.setAutoStartEnabled(on);
    }
  }

  /** 声音开关在别处（快捷设置）改了，同步小窗里静音按钮的状态 */
  setAudio(on: boolean): void {
    this.audioOn = on;
    this.syncAudioButton();
  }

  /** 小窗还开着时用户从桌面图标回到了 App：收起小窗，画面回到播放页（不算用户关闭） */
  restore(): void {
    const c = this.controller;
    // 点了小窗上的“恢复全屏”时系统已经在收起（restoring），不用再调 stopPiP
    if (c === null || !this.active || this.restoring) {
      return;
    }
    this.restoring = true;
    c.stopPiP().catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'pip stop failed: %{public}d %{public}s', e.code, e.message);
    });
  }

  /** 播放页的“小窗”按钮：只能在前台调用 */
  async start(): Promise<string> {
    const c = this.controller;
    if (c === null) {
      return 'not ready';
    }
    this.starting = true;
    try {
      await c.startPiP();
      return '';
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'pip start failed: %{public}d %{public}s', err.code, err.message);
      return `${err.code}`;
    } finally {
      this.starting = false;
    }
  }

  private syncAudioButton(): void {
    if (this.controller === null || !this.active) {
      return;
    }
    try {
      this.controller.updatePiPControlStatus(PiPWindow.PiPControlType.MUTE_SWITCH,
        this.audioOn ? PiPWindow.PiPControlStatus.OPEN : PiPWindow.PiPControlStatus.CLOSE);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'pip update control failed: %{public}s', JSON.stringify(e));
    }
  }

  private onStateChange(state: PiPWindow.PiPState, reason: string): void {
    hilog.info(DOMAIN, TAG, 'pip state %{public}d reason=%{public}s', state, reason);
    switch (state) {
      case PiPWindow.PiPState.ABOUT_TO_START:
        this.active = true;
        this.restoring = false;
        break;
      case PiPWindow.PiPState.STARTED:
        this.active = true;
        this.syncAudioButton();
        break;
      case PiPWindow.PiPState.ABOUT_TO_RESTORE:
        this.restoring = true;
        break;
      case PiPWindow.PiPState.STOPPED:
      case PiPWindow.PiPState.ERROR:
        if (this.active) {
          this.active = false;
          const restored = this.restoring;
          this.restoring = false;
          if (!restored) {
            this.onClosed();
          }
        }
        break;
      default:
        break;
    }
  }
}
