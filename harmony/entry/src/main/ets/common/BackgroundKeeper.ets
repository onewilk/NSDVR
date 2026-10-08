import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { notificationManager } from '@kit.NotificationKit';
import { common, wantAgent, WantAgent } from '@kit.AbilityKit';
import { BusinessError, commonEventManager, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { tr } from './I18n';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';

export interface LiveViewState {
  title: string;
  text: string;
  // 0~100
  progress: number;
}

/**
 * 后台保持连接：DATA_TRANSFER 长时任务 + 它自带的系统实况窗（下载进度模板）。
 *
 * 为什么是这个组合：
 * - 后台“接收但不渲染”本质是持续的数据下载，对应 DATA_TRANSFER；
 * - DATA_TRANSFER 要求用系统实况窗定期更新进度（约 10 分钟不更新会被取消），
 *   这个实况窗不需要 Live View Kit 的场景权益审批；
 * - 长时任务只能在前台申请（后台申请会返回 9800005），所以开始串流时就申请，
 *   前台播放时状态栏是隐藏的，实况窗实际只在切到后台后被看到。
 *
 * 实况窗交互（Mate 60 Pro / API 26 实测）：
 * - 点击：拉起应用回到前台；
 * - 左滑删除：系统终止长时任务，约 17 秒后冻结应用并切断网络。这里监听取消事件（API 15+）主动断开，
 *   当作“停止串流”；
 * - 下载模板的系统实况窗不显示 actionButtons，“回到画面 / 停止串流”按钮留着，以后系统版本支持时自动生效。
 */
export class BackgroundKeeper {
  // 通知里的“停止串流”按钮触发（公共事件回调在主线程）
  onStopRequested: () => void = () => {
  };
  // 长时任务被取消（用户在通知栏删除实况窗，或系统回收）时回调，只在 API 15+ 设备上生效
  onCanceled: () => void = () => {
  };

  private ctx: common.UIAbilityContext;
  private stopEvent: string;
  private running: boolean = false;
  private notificationId: number = -1;
  private openAgent: WantAgent | undefined = undefined;
  private stopAgent: WantAgent | undefined = undefined;
  private subscriber: commonEventManager.CommonEventSubscriber | undefined = undefined;
  private publishErrors: number = 0;
  private listeningCancel: boolean = false;
  private cancelCallback = (info: backgroundTaskManager.ContinuousTaskCancelInfo): void => {
    hilog.info(DOMAIN, TAG, 'continuous task canceled, reason=%{public}d', info.reason);
    if (!this.running) {
      return;
    }
    this.running = false;
    this.cleanup();
    this.onCanceled();
  };

  constructor(ctx: common.UIAbilityContext) {
    this.ctx = ctx;
    this.stopEvent = `${ctx.abilityInfo.bundleName}.action.STOP_STREAM`;
  }

  isRunning(): boolean {
    return this.running;
  }

  /** 申请长时任务，返回空串表示成功，否则为错误信息（调用方应降级为“切后台即断开”） */
  async start(): Promise<string> {
    if (this.running) {
      return '';
    }
    try {
      await this.ensureNotificationEnabled();
      const openInfo: wantAgent.WantAgentInfo = {
        wants: [{ bundleName: this.ctx.abilityInfo.bundleName, abilityName: this.ctx.abilityInfo.name }],
        actionType: wantAgent.OperationType.START_ABILITY,
        requestCode: 1,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      this.openAgent = await wantAgent.getWantAgent(openInfo);
      const stopInfo: wantAgent.WantAgentInfo = {
        wants: [{ action: this.stopEvent }],
        actionType: wantAgent.OperationType.SEND_COMMON_EVENT,
        requestCode: 2,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      };
      this.stopAgent = await wantAgent.getWantAgent(stopInfo);
      await this.subscribeStop();

      const res = await backgroundTaskManager.startBackgroundRunning(this.ctx, ['dataTransfer'], this.openAgent);
      this.notificationId = res.notificationId;
      this.running = true;
      this.publishErrors = 0;
      this.listenCancel();
      hilog.info(DOMAIN, TAG, 'continuous task started, notificationId=%{public}d', this.notificationId);
      return '';
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(DOMAIN, TAG, 'start continuous task failed: %{public}d %{public}s', err.code, err.message);
      await this.cleanup();
      return `${err.code} ${err.message}`;
    }
  }

  /** 更新实况窗内容；必须用长时任务返回的通知 id、系统实况窗类型和 downloadTemplate */
  async update(state: LiveViewState): Promise<void> {
    const open = this.openAgent;
    const stop = this.stopAgent;
    if (!this.running || open === undefined || stop === undefined) {
      return;
    }
    const template: notificationManager.NotificationTemplate = {
      name: 'downloadTemplate', // 当前系统只支持这个模板
      data: {
        title: state.title,
        fileName: state.text,
        progressValue: Math.max(0, Math.min(100, Math.round(state.progress)))
      }
    };
    const request: notificationManager.NotificationRequest = {
      id: this.notificationId,
      notificationSlotType: notificationManager.SlotType.LIVE_VIEW,
      content: {
        notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_SYSTEM_LIVE_VIEW,
        systemLiveView: {
          typeCode: 8, // 上传下载类型，系统要求固定为 8
          title: state.title,
          text: state.text
        }
      },
      template: template,
      wantAgent: open,
      // 下载模板的系统实况窗目前不显示按钮（API 26 实测），保留以兼容以后的系统版本
      actionButtons: [
        { title: tr('app_live_btn_open'), wantAgent: open },
        { title: tr('app_live_btn_stop'), wantAgent: stop }
      ]
    };
    try {
      await notificationManager.publish(request);
    } catch (e) {
      const err = e as BusinessError;
      // 未授权通知时每次都会失败，只记前几次
      if (this.publishErrors++ < 3) {
        hilog.warn(DOMAIN, TAG, 'update live view failed: %{public}d %{public}s', err.code, err.message);
      }
    }
  }

  async stop(): Promise<void> {
    if (this.running) {
      this.running = false;
      try {
        await backgroundTaskManager.stopBackgroundRunning(this.ctx);
        hilog.info(DOMAIN, TAG, 'continuous task stopped');
      } catch (e) {
        const err = e as BusinessError;
        hilog.warn(DOMAIN, TAG, 'stop continuous task failed: %{public}d %{public}s', err.code, err.message);
      }
    }
    await this.cleanup();
  }

  // 更新实况窗要发通知，应用通知默认关闭；用户拒绝的话实况窗无法更新，长时任务约 10 分钟后被系统取消
  private async ensureNotificationEnabled(): Promise<void> {
    try {
      const enabled = await notificationManager.isNotificationEnabled();
      if (!enabled) {
        await notificationManager.requestEnableNotification(this.ctx);
      }
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'notification not enabled: %{public}d %{public}s', err.code, err.message);
    }
  }

  private listenCancel(): void {
    // on('continuousTaskCancel') 从 API 15 开始提供；工程最低兼容 API 12，低版本设备上跳过
    if (this.listeningCancel || deviceInfo.sdkApiVersion < 15) {
      return;
    }
    try {
      backgroundTaskManager.on('continuousTaskCancel', this.cancelCallback);
      this.listeningCancel = true;
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'listen continuousTaskCancel failed: %{public}d %{public}s', err.code, err.message);
    }
  }

  private async subscribeStop(): Promise<void> {
    if (this.subscriber !== undefined) {
      return;
    }
    // 事件名带包名前缀，只有本应用的通知按钮会发
    const info: commonEventManager.CommonEventSubscribeInfo = { events: [this.stopEvent] };
    const subscriber = await commonEventManager.createSubscriber(info);
    commonEventManager.subscribe(subscriber, (err: BusinessError, data: commonEventManager.CommonEventData) => {
      if (err) {
        hilog.warn(DOMAIN, TAG, 'stop event error: %{public}d', err.code);
        return;
      }
      if (data.event === this.stopEvent) {
        hilog.info(DOMAIN, TAG, 'stop requested from live view');
        this.onStopRequested();
      }
    });
    this.subscriber = subscriber;
  }

  private async cleanup(): Promise<void> {
    if (this.listeningCancel) {
      try {
        backgroundTaskManager.off('continuousTaskCancel', this.cancelCallback);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'off continuousTaskCancel failed');
      }
      this.listeningCancel = false;
    }
    const subscriber = this.subscriber;
    if (subscriber !== undefined) {
      commonEventManager.unsubscribe(subscriber);
      this.subscriber = undefined;
    }
    this.openAgent = undefined;
    this.stopAgent = undefined;
    this.notificationId = -1;
  }
}
