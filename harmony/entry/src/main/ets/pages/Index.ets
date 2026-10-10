import { window, promptAction, SymbolGlyphModifier } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { KeyCode } from '@kit.InputKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { HdsTabs, hdsMaterial } from '@kit.UIDesignKit';
import sysdvr from 'libentry.so';
import { ConnectConfig, DeviceInfo, SessionStats } from 'libentry.so';
import { BackgroundKeeper } from '../common/BackgroundKeeper';
import {
  AudioCombo, codecLabels, comboLabel, comboTag, currentCombo, DEFAULT_ROTATE_PLAN, ExtCodec, ExtRotation, isExtHost,
  OPUS_COMPLEXITY_OPTIONS, OPUS_FRAME_OPTIONS, OPUS_KBPS_OPTIONS, parsePlan, rememberExtHost, ROTATE_MINUTES
} from '../common/ExtAudio';
import { HotspotService } from '../common/HotspotService';
import { SHORTCUT_HOTSPOT_CONNECT, SHORTCUT_KEY, shortcutAction } from '../common/LaunchShortcut';
import { NetBooster } from '../common/NetBooster';
import { NetQualityWatcher } from '../common/NetQuality';
import { PipSession } from '../common/PipSession';
import {
  APP_NAME, Breakpoint, breakpointOf, enhanceLabels, formatDuration, hotspotSupported, KEEP_ALIVE_MINUTES,
  keepAliveLabels, smoothLabels, StreamMode, supportsEnhance, supportsFreeze, supportsKeepAlive, supportsNetOpt, supportsSmooth
} from '../common/Prefs';
import { languageSignature, tr, uiLanguageIndex } from '../common/I18n';
import { StatsRecorder } from '../common/StatsRecorder';
import { StreamTarget } from '../common/StreamTarget';
import { lastRecentHost, RecentHost, rememberHost } from '../common/RecentHosts';
import { RtspPlayer } from '../common/RtspPlayer';
import { applyColorMode } from '../common/Theme';
import { UsbLog, UsbPump, UsbSwitch, UsbWatcher } from '../common/UsbSwitch';
import { AboutView } from '../views/AboutView';
import { ConfigView } from '../views/ConfigView';
import { ConnectView } from '../views/ConnectView';
import { GuideView } from '../views/GuideView';

const DOMAIN = 0x5344;
const TAG = 'SysDVR';
const LIVE_VIEW_INTERVAL_MS = 5000;
// 前台时实况窗不可见（沉浸式隐藏了状态栏），每分钟刷新一次满足长时任务的进度更新要求即可
const FOREGROUND_REFRESH_TICKS = 12;
const RTSP_XCOMPONENT_ID = 'sysdvrRtspSurface';
const VIDEO_ASPECT = 16 / 9;
// 锁定后锁形图标亮显多久再变淡
const LOCK_HINT_MS = 3000;
// 切后台后等多久还没转入小窗（例如系统设置里关了画中画），就按普通切后台处理
const PIP_WAIT_MS = 1000;
// 小窗关闭后等一下再按切后台处理：用户也可能是点了桌面图标回到 App
const PIP_CLOSED_GRACE_MS = 500;
// “热点+连接”快捷方式：开启专用热点后最多等游戏机多久
const SHORTCUT_WAIT_MS = 90000;
// 播放时请求的屏幕刷新率：画面最高 30 帧，60Hz 每帧占两个刷新周期，上屏节奏不受网络抖动影响
const PLAYBACK_REFRESH_HZ = 60;
// 切换语言重建首页时，要回到的页签（-1 = 无）
const RESTORE_TAB_KEY = 'ui.restoreTab';
// 页签标题（资源键）
const TAB_TITLES: string[] = ['app_tab_stream', 'app_tab_settings', 'app_tab_about'];
const TAB_ICONS: Resource[] = [$r('sys.symbol.gamecontroller'), $r('sys.symbol.gearshape'), $r('sys.symbol.info_circle')];

// 通过 XComponentController 拿 surface 的创建/销毁时机（API 12）
class StreamSurfaceController extends XComponentController {
  onCreated: (surfaceId: string) => void = () => {
  };
  onDestroyed: () => void = () => {
  };

  onSurfaceCreated(surfaceId: string): void {
    this.onCreated(surfaceId);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.onDestroyed();
  }
}

function selectOptions(labels: string[]): SelectOption[] {
  return labels.map((label: string): SelectOption => {
    const option: SelectOption = { value: label };
    return option;
  });
}

@Entry
@Component
struct Index {
  // ---- 布局
  @State @Watch('updateReserve') bp: Breakpoint = 'sm';
  // 导航位置按横竖屏决定：横屏（且足够宽）放左侧竖排，竖屏和手机一样放底部；内容布局仍按宽度断点
  @State @Watch('updateReserve') navSide: boolean = false;
  @State tab: number = 0;
  @StorageProp('ui.safeBottom') @Watch('updateReserve') safeBottom: number = 0;
  @StorageLink('sysdvr.colorMode') colorMode: number = -1;
  // 界面语言变了就加一（键名同 I18n.LANGUAGE_EPOCH_KEY）：tr() 取的是普通字符串，要重建页面才会换成新语言
  @StorageProp('ui.langEpoch') @Watch('onLanguageChanged') langEpoch: number = 0;
  // 桌面快捷方式（见 LaunchShortcut.ets）
  @StorageProp(SHORTCUT_KEY) @Watch('onShortcut') shortcut: string = '';
  private handledShortcut: string = '';
  private shortcutTimer: number = -1;
  // 本页面建立时资源用的语言（见 languageSignature）；语言真的变了才重建
  private builtLanguage: string = '';
  private languageTimer: number = -1;
  // 播放中语言变了：先不重建（会断开串流），退出播放后再重建
  private languageStale: boolean = false;
  // HarmonyOS 6.1（API 23）起用 HdsTabs 悬浮页签 + 沉浸光感材质，低版本退回普通页签
  private useFloating: boolean = deviceInfo.sdkApiVersion >= 23;
  private material: hdsMaterial.MaterialType = hdsMaterial.MaterialType.ADAPTIVE;
  private navStack: NavPathStack = new NavPathStack();
  @State playerWidth: number = 0;
  @State playerHeight: number = 0;
  // ---- 设置（与首页“串流选项”共用）
  @StorageLink('sysdvr.enableAudio') enableAudio: boolean = true;
  @StorageLink('sysdvr.smoothLevelV2') smoothLevel: number = 1;
  @StorageLink('sysdvr.netOpt') netOpt: boolean = true;
  @StorageLink('sysdvr.screenOff') screenOff: boolean = false;
  @StorageLink('sysdvr.keepAliveIndexV2') keepAliveIndex: number = 1;
  @StorageLink('sysdvr.pipAuto') pipAuto: boolean = true;
  @StorageLink('sysdvr.lowRefresh') @Watch('applyRefreshHint') lowRefresh: boolean = true;
  @StorageLink('sysdvr.showStatsOverlay') showStats: boolean = false;
  @StorageLink('sysdvr.freezeOnLoss') freezeOnLoss: boolean = true;
  @StorageLink('sysdvr.enhanceLevel') enhanceLevel: number = 0;
  @StorageProp('hs.channel') hotspotChannel: number = 0;
  @StorageProp('hs.staChannel') staChannel: number = 0;
  // ---- 实验功能（仅 TCP Bridge；服务端表明支持扩展、或打开了开发者选项才显示）
  @StorageLink('sysdvr.extCodec') extCodec: number = 0;
  @StorageLink('sysdvr.extOpusKbps') extOpusKbps: number = 96;
  @StorageLink('sysdvr.extOpusComplexity') extOpusComplexity: number = 5;
  @StorageLink('sysdvr.extOpusFrameMs') extOpusFrameMs: number = 20;
  @StorageLink('sysdvr.devMode') devMode: boolean = false;
  @StorageLink('sysdvr.rotateOn') rotateOn: boolean = false;
  @StorageProp('sysdvr.rotateMinutesIndex') rotateMinutesIndex: number = 1;
  @StorageProp('sysdvr.rotatePlan') rotatePlan: string = DEFAULT_ROTATE_PLAN;
  // 本次连接的服务端表明支持扩展（见到扩展音频标记或诊断包）
  @State extActive: boolean = false;
  // 自动轮换是否在跑、快捷设置里显示的进度
  @State rotating: boolean = false;
  @State rotationText: string = '';
  // ---- 播放
  @State playing: boolean = false;
  @State playMode: number = StreamMode.TCP;
  @State showPanel: boolean = false;
  @State statsText: string = '';
  @State statusText: string = '';
  // 播放页：点画面显示/隐藏按钮；锁定后只显示锁形图标
  @State controlsVisible: boolean = true;
  @State locked: boolean = false;
  @State lockHintBright: boolean = true;
  private lockHintTimer: number = -1;
  private target: StreamTarget | null = null;
  private xController: StreamSurfaceController = new StreamSurfaceController();
  private surfaceId: string = '';
  private sessionStarted: boolean = false;
  private connecting: boolean = false;
  // 在播放页里申请长时任务时可能弹系统框，期间忽略 onPageHide，免得被当成切后台
  private suppressPageHide: boolean = false;
  private inBackground: boolean = false;
  private backgroundSince: number = 0;
  // 播放页当前是否不可见（切到后台、转入小窗）
  private pageHidden: boolean = false;
  // 小窗（画中画）：设备支持时 TCP Bridge / USB 可用
  private pip: PipSession = new PipSession();
  private pipSupported: boolean = PipSession.supported();
  private pipTimer: number = -1;
  private keeper: BackgroundKeeper | null = null;
  private booster: NetBooster = new NetBooster();
  // 网络质量感知：系统预警网络变差时提前加大平滑缓冲（TCP Bridge 经路由器连接时生效）
  private netWatcher: NetQualityWatcher = new NetQualityWatcher();
  private recorder: StatsRecorder = new StatsRecorder();
  private rtsp: RtspPlayer = new RtspPlayer();
  private usbPump: UsbPump = new UsbPump();
  private usbWatcher: UsbWatcher = new UsbWatcher();
  private statsTimer: number = -1;
  private liveViewTimer: number = -1;
  private liveViewTick: number = 0;
  private lastStats: SessionStats | null = null;
  private lastUsbMessage: string = '';
  // 本次会话是否已经提示过“画质增强没能启用”
  private enhanceWarned: boolean = false;
  // 已写进 stats.csv / 日志的 Switch 内存报告（变化时再写一次，比如重连后）
  private lastMemoryNote: string = '';
  private rotation: ExtRotation = new ExtRotation();
  // 本次会话是否已经把这台设备记成“支持扩展”
  private extRemembered: boolean = false;
  // 请求的编码从什么时候起还没生效（0 = 已生效）：超过 3 秒多半是服务端没采用（例如 Opus 回落成 PCM）
  private extPendingSince: number = 0;
  // 本次会话是否已记进“最近连接”（TCP 收到视频、RTSP 开始播放后记一次）
  private hostRemembered: boolean = false;

  aboutToAppear(): void {
    this.xController.onCreated = (surfaceId: string) => {
      this.onSurfaceReady(surfaceId);
    };
    this.xController.onDestroyed = () => {
      this.onSurfaceGone();
    };
    const keeper = new BackgroundKeeper(getContext(this) as common.UIAbilityContext);
    keeper.onStopRequested = () => {
      this.leavePlayer();
    };
    keeper.onCanceled = () => {
      this.onKeepAliveCanceled();
    };
    this.keeper = keeper;
    this.pip.onClosed = () => {
      this.onPipClosed();
    };
    this.pip.onAudioToggled = (on: boolean) => {
      this.applyAudio(on);
    };
    applyColorMode(getContext(this), this.colorMode);
    if (this.useFloating) {
      try {
        // 设备支持沉浸光感就用，否则交给系统自适应
        if (hdsMaterial.getSystemMaterialTypes().includes(hdsMaterial.MaterialType.IMMERSIVE)) {
          this.material = hdsMaterial.MaterialType.IMMERSIVE;
        }
      } catch (e) {
        this.useFloating = false;
      }
    }
    this.updateReserve();
    // native 层的状态文字跟随界面语言（切换语言时页面会重建，这里再同步一次）
    sysdvr.setLanguage(uiLanguageIndex());
    this.builtLanguage = languageSignature();
    // 因为切换语言而重建时，回到重建前的页签（例如配置页）
    const restoreTab = AppStorage.get<number>(RESTORE_TAB_KEY) ?? -1;
    if (restoreTab >= 0) {
      this.tab = restoreTab;
      AppStorage.setOrCreate(RESTORE_TAB_KEY, -1);
    }
    // 专用热点：同步系统里仍在运行的热点，按设置自动开启
    HotspotService.get().init();
    if (this.shortcut.length > 0) {
      // 从快捷方式冷启动：等首页建好再连接
      setTimeout(() => {
        this.onShortcut();
      }, 300);
    }
  }

  /** 底部导航栏会盖住内容的那段高度，各页面据此在底部留白 */
  updateReserve(): void {
    let reserve = 0;
    if (!this.navSide) {
      reserve = this.useFloating ? this.safeBottom + 80 : this.safeBottom + 56;
    }
    AppStorage.setOrCreate('ui.bottomReserve', reserve);
  }

  aboutToDisappear(): void {
    this.cancelShortcutWait();
    if (this.languageTimer >= 0) {
      clearTimeout(this.languageTimer);
      this.languageTimer = -1;
    }
    this.stopPlayback();
  }

  // ---------------------------------------------------------------- 界面语言

  // 手动切换语言、或跟随系统时系统语言变了。配置更新可能稍晚才到，等一下再比对，多次通知只处理一次
  onLanguageChanged(): void {
    if (this.languageTimer >= 0) {
      clearTimeout(this.languageTimer);
    }
    this.languageTimer = setTimeout(() => {
      this.languageTimer = -1;
      this.refreshLanguage();
    }, 150);
  }

  private refreshLanguage(): void {
    // native 层的状态文字立即换（播放中也一样）
    sysdvr.setLanguage(uiLanguageIndex());
    const now = languageSignature();
    hilog.info(DOMAIN, TAG, 'language: built=%{public}s now=%{public}s', this.builtLanguage, now);
    if (now === this.builtLanguage) {
      return;
    }
    if (this.playing) {
      this.languageStale = true;
      return;
    }
    this.rebuildForLanguage();
  }

  // 重建首页，让所有 tr() 按新语言重新取词（热点等全局状态不受影响）
  private rebuildForLanguage(): void {
    this.languageStale = false;
    AppStorage.setOrCreate(RESTORE_TAB_KEY, this.tab);
    try {
      this.getUIContext().getRouter().replaceUrl({ url: 'pages/Index' }).catch((e: Error) => {
        hilog.warn(DOMAIN, TAG, 'rebuild page failed: %{public}s', JSON.stringify(e));
      });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'rebuild page failed: %{public}s', JSON.stringify(e));
    }
  }

  onPageHide(): void {
    this.pageHidden = true;
    if (!this.playing) {
      // 离开 App 就不再等游戏机连上热点（回来后用户自己点连接）
      this.cancelShortcutWait();
    }
    if (!this.playing || this.suppressPageHide) {
      return;
    }
    if (this.pip.isActive()) {
      // 转入了小窗（点了“小窗”按钮，或系统自动转入）：照常渲染
      return;
    }
    if (this.pip.autoStartArmed()) {
      // 系统会在切后台时自动转入小窗；没转入（例如系统设置里关了画中画）再按切后台处理
      this.schedulePipCheck(PIP_WAIT_MS);
      return;
    }
    this.handleBackground();
  }

  // 切到后台、且没有在小窗里播放
  private handleBackground(): void {
    if (this.playMode === StreamMode.RTSP) {
      // ijkplayer 没有“只收不渲染”的模式：后台暂停拉流，回前台重连
      this.inBackground = true;
      this.rtsp.suspend();
      this.applyRefreshHint();
    } else if (this.keeper !== null && this.keeper.isRunning()) {
      this.enterBackground();
    } else {
      // 没开后台保持（或长时任务申请失败）：直接断开
      this.leavePlayer();
    }
  }

  onPageShow(): void {
    this.pageHidden = false;
    this.cancelPipCheck();
    // 小窗开着时点桌面图标回来：系统不会自动收起小窗，播放页只剩黑底
    this.pip.restore();
    if (!this.inBackground) {
      return;
    }
    if (this.playMode === StreamMode.RTSP) {
      this.inBackground = false;
      this.rtsp.resume();
      this.applyRefreshHint();
    } else {
      this.exitBackground();
    }
  }

  // 过一会儿再看：页面仍在后台、也没在小窗里播放，就按切后台处理
  private schedulePipCheck(delayMs: number): void {
    this.cancelPipCheck();
    this.pipTimer = setTimeout(() => {
      this.pipTimer = -1;
      if (this.pageHidden && this.playing && !this.inBackground && !this.pip.isActive()) {
        this.handleBackground();
      }
    }, delayMs);
  }

  private cancelPipCheck(): void {
    if (this.pipTimer >= 0) {
      clearTimeout(this.pipTimer);
      this.pipTimer = -1;
    }
  }

  // 播放画面可见时（前台或小窗）请求 60Hz，切后台、断开后撤销，交还系统决定
  applyRefreshHint(): void {
    const visible = this.sessionStarted && !this.inBackground;
    sysdvr.setRefreshRateHint(visible && this.lowRefresh ? PLAYBACK_REFRESH_HZ : 0);
  }

  // 游戏机连在本机的专用热点上（和热点同一网段）：串流不走系统统计的 Wi-Fi 链路
  private viaHotspot(host: string): boolean {
    const goIp = AppStorage.get<string>('hs.goIp') ?? '';
    if (goIp.length === 0) {
      return false;
    }
    const prefix = (ip: string): string => ip.split('.').slice(0, 3).join('.');
    return prefix(goIp) === prefix(host);
  }

  // 用户关掉了小窗：之后和直接切后台一样（开了后台保持就继续接收，否则断开）
  private onPipClosed(): void {
    hilog.info(DOMAIN, TAG, 'pip closed by user');
    if (this.playing) {
      this.schedulePipCheck(PIP_CLOSED_GRACE_MS);
    }
  }

  private async startPip(): Promise<void> {
    this.showPanel = false;
    const err = await this.pip.start();
    if (err.length > 0) {
      promptAction.showToast({ message: tr('app_toast_pip_failed', err) });
      return;
    }
    // 手动转入小窗时主窗口不会自己退到后台，留在前台只剩黑底和按钮：主动退回桌面
    try {
      await (getContext(this) as common.UIAbilityContext).moveAbilityToBackground();
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'move to background failed: %{public}s', JSON.stringify(e));
    }
  }

  private applyPipAuto(on: boolean): void {
    if (on === this.pipAuto) {
      return;
    }
    this.pipAuto = on;
    this.pip.setAutoStart(on);
  }

  onBackPress(): boolean {
    if (this.playing && this.locked) {
      // 锁定时返回手势不断开，只提示怎么解锁
      this.flashLockHint();
      promptAction.showToast({ message: tr('app_toast_locked') });
      return true;
    }
    if (!this.playing && this.navStack.size() > 0) {
      this.navStack.pop();
      return true;
    }
    if (this.showPanel) {
      this.showPanel = false;
      return true;
    }
    if (this.playing) {
      this.leavePlayer();
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------- 桌面快捷方式

  onShortcut(): void {
    const raw = this.shortcut;
    if (raw.length === 0 || raw === this.handledShortcut) {
      return;
    }
    this.handledShortcut = raw;
    // 已经在串流（或正在连接）：快捷方式只是把 App 带回前台
    if (this.playing || this.connecting) {
      return;
    }
    this.cancelShortcutWait();
    const last = lastRecentHost();
    if (last === null) {
      promptAction.showToast({ message: tr('shortcut_no_recent') });
      return;
    }
    // 回到串流页、切到上次的模式（TCP Bridge 下顺便打开局域网发现，等游戏机广播）
    this.tab = 0;
    this.navStack.clear();
    AppStorage.set('sysdvr.mode', last.mode);
    if (shortcutAction(raw) === SHORTCUT_HOTSPOT_CONNECT) {
      if (hotspotSupported()) {
        this.hotspotThenConnect(last);
        return;
      }
      promptAction.showToast({ message: tr('shortcut_hotspot_unsupported') });
    }
    this.connectRecent(last, last.host, last.name);
  }

  // 和串流页“最近连接”卡片点“连接”一样
  private connectRecent(h: RecentHost, host: string, name: string): void {
    if (h.mode === StreamMode.RTSP) {
      AppStorage.set('sysdvr.rtspHost', host);
      this.play({ mode: StreamMode.RTSP, host: host, label: RtspPlayer.urlOf(host), usb: null, name: name });
    } else {
      AppStorage.set('sysdvr.lastHost', host);
      this.play({ mode: StreamMode.TCP, host: host, label: host, usb: null, name: name });
    }
  }

  // “热点+连接”：开启专用热点，等游戏机连上来再连接。TCP Bridge 看局域网广播（游戏机在热点上的 IP 不一定和上次一样）；
  // RTSP 没有广播，等热点上有设备连上后连上次的地址
  private async hotspotThenConnect(last: RecentHost): Promise<void> {
    const hs = HotspotService.get();
    if (!hs.isOn()) {
      promptAction.showToast({ message: tr('shortcut_hotspot_starting') });
      const err = await hs.start();
      if (err.length > 0) {
        promptAction.showToast({ message: tr('shortcut_hotspot_failed', err), duration: 4000 });
        return;
      }
    }
    promptAction.showToast({ message: tr('shortcut_waiting'), duration: 4000 });
    const deadline = Date.now() + SHORTCUT_WAIT_MS;
    this.shortcutTimer = setInterval(() => {
      this.pollShortcutWait(last, deadline);
    }, 1000);
  }

  private pollShortcutWait(last: RecentHost, deadline: number): void {
    if (this.playing || this.connecting) {
      // 用户等不及自己点了连接
      this.cancelShortcutWait();
      return;
    }
    const goIp = AppStorage.get<string>('hs.goIp') ?? '';
    if (HotspotService.get().isOn() && goIp.length > 0) {
      if (last.mode === StreamMode.TCP) {
        const devices = sysdvr.getDevices();
        const prefix = (ip: string): string => ip.split('.').slice(0, 3).join('.');
        const pick = devices.find((d: DeviceInfo): boolean => d.ip === last.host) ??
          devices.find((d: DeviceInfo): boolean => prefix(d.ip) === prefix(goIp));
        if (pick !== undefined) {
          this.cancelShortcutWait();
          this.connectRecent(last, pick.ip, tr('connect_device_name', pick.version, pick.protocol));
          return;
        }
      } else if ((AppStorage.get<number>('hs.clients') ?? 0) > 0) {
        this.cancelShortcutWait();
        this.connectRecent(last, last.host, last.name);
        return;
      }
    }
    if (Date.now() > deadline) {
      this.cancelShortcutWait();
      promptAction.showToast({ message: tr('shortcut_wait_timeout'), duration: 4000 });
    }
  }

  private cancelShortcutWait(): void {
    if (this.shortcutTimer >= 0) {
      clearInterval(this.shortcutTimer);
      this.shortcutTimer = -1;
    }
  }

  // ---------------------------------------------------------------- 连接与播放

  private keepAliveMinutes(): number {
    return supportsKeepAlive(this.playMode) ? KEEP_ALIVE_MINUTES[this.keepAliveIndex] : 0;
  }

  private async play(target: StreamTarget): Promise<void> {
    if (this.connecting || this.playing) {
      return;
    }
    this.connecting = true;
    this.target = target;
    this.hostRemembered = false;
    this.playMode = target.mode;
    // 长时任务只能在前台申请，而且可能先弹“允许通知”授权框，所以留在首页申请完再进播放页，
    // 避免授权框触发播放页的 onPageHide
    if (this.keepAliveMinutes() > 0) {
      await this.startKeeper();
    }
    this.statusText = tr('app_connecting', target.label);
    this.statsText = '';
    this.locked = false;
    this.controlsVisible = true;
    this.showPanel = false;
    // 显示 XComponent，surface 就绪后（TCP/USB 在 onSurfaceReady、RTSP 在 onLoad）真正建立连接
    this.playing = true;
    this.connecting = false;
    this.setImmersive(true);
    if (target.mode === StreamMode.USB) {
      // 播放中拔线：及时退出并提示
      this.usbWatcher.start(() => {
        if (this.playing && this.playMode === StreamMode.USB && UsbSwitch.find() === undefined) {
          promptAction.showToast({ message: tr('app_toast_usb_detached') });
          this.leavePlayer();
        }
      });
    }
  }

  private async startKeeper(): Promise<void> {
    const keeper = this.keeper;
    if (keeper === null) {
      return;
    }
    const err = await keeper.start();
    if (err.length > 0) {
      promptAction.showToast({ message: tr('app_toast_keepalive_unavailable', err), duration: 4000 });
      return;
    }
    this.liveViewTick = 0;
    this.refreshLiveView(true);
    this.liveViewTimer = setInterval(() => {
      this.refreshLiveView(false);
    }, LIVE_VIEW_INTERVAL_MS);
  }

  private onSurfaceReady(surfaceId: string): void {
    this.surfaceId = surfaceId;
    const t = this.target;
    if (t === null) {
      return;
    }
    if (!this.sessionStarted) {
      const cfg: ConnectConfig = {
        mode: t.mode,
        surfaceId: surfaceId,
        audio: this.enableAudio,
        smoothLevel: this.smoothLevel,
        netOpt: this.netOpt && supportsNetOpt(t.mode),
        screenOff: this.screenOff,
        freezeOnLoss: this.freezeOnLoss,
        enhanceLevel: supportsEnhance(t.mode) ? this.enhanceLevel : 0
      };
      if (t.mode === StreamMode.TCP) {
        // 实验扩展：TCP Bridge 下总是请求（官方 SysDVR 忽略）；握手就带上要用的编码，开了自动轮换就从第一组开始
        const init = this.initialCombo();
        cfg.nextExt = true;
        cfg.extCodec = init.codec;
        cfg.extOpusKbps = init.kbps;
        cfg.extOpusComplexity = init.complexity;
        cfg.extOpusFrameMs = init.frameMs;
      }
      const link = t.usb;
      if (t.mode === StreamMode.USB && link !== null) {
        cfg.usbFd = link.fd;
        cfg.usbInterface = link.iface.id;
        cfg.usbEpIn = link.epIn.address;
        cfg.usbEpOut = link.epOut.address;
        cfg.usbPump = link.pump;
        if (link.pump) {
          cfg.onUsbWrite = this.usbPump.onWrite;
        }
      } else {
        cfg.host = t.host;
      }
      const err = sysdvr.connect(cfg);
      if (err.length > 0) {
        hilog.error(DOMAIN, TAG, 'connect failed: %{public}s', err);
        promptAction.showToast({ message: tr('app_toast_start_failed', err), duration: 4000 });
        this.leavePlayer();
        return;
      }
      if (link !== null && link.pump) {
        this.usbPump.start(link);
      }
      this.sessionStarted = true;
      this.lastStats = null;
      this.enhanceWarned = false;
      this.lastMemoryNote = '';
      this.extActive = false;
      this.extRemembered = false;
      this.extPendingSince = 0;
      this.rotation.stop();
      this.rotating = false;
      this.rotationText = '';
      if (cfg.netOpt) {
        this.booster.enter();
      }
      if (t.mode === StreamMode.TCP) {
        this.netWatcher.start(!this.viaHotspot(t.host));
      }
      if (this.pipSupported) {
        this.pip.open(getContext(this), this.xController, this.pipAuto, this.enableAudio);
      }
      this.applyRefreshHint();
      this.startStats(t);
    } else if (!this.inBackground) {
      // 会话还在但 surface 重建了：绑定到新 surface
      this.resumeRendering();
    }
  }

  private onRtspLoad(context: object): void {
    const t = this.target;
    if (t === null || this.sessionStarted) {
      return;
    }
    this.rtsp.onStatus = (msg: string) => {
      this.statusText = msg;
    };
    this.rtsp.logDir = (getContext(this) as common.UIAbilityContext).filesDir;
    this.rtsp.start(context, RTSP_XCOMPONENT_ID, t.host, this.enableAudio);
    this.sessionStarted = true;
    this.applyRefreshHint();
    this.startStats(t);
  }

  private startStats(t: StreamTarget): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    this.recorder.start(ctx.filesDir, `mode=${t.mode} target=${t.label}`);
    this.statsTimer = setInterval(() => {
      try {
        this.pollStats();
      } catch (e) {
        // 统计只是辅助信息，出错不能把播放带崩
        hilog.warn(DOMAIN, TAG, 'pollStats failed: %{public}s', JSON.stringify(e));
      }
    }, 1000);
  }

  private onSurfaceGone(): void {
    this.surfaceId = '';
    if (this.sessionStarted) {
      // 解码器不能继续往已销毁的 surface 上画
      sysdvr.pauseVideo();
    }
  }

  // ---------------------------------------------------------------- 前后台切换（TCP Bridge / USB）

  private enterBackground(): void {
    this.inBackground = true;
    this.backgroundSince = Date.now();
    // 照常接收，释放解码器和音频，只维护 GOP 缓存；后台不渲染，也不必再占用网络加速
    sysdvr.pauseVideo();
    this.booster.leave();
    this.applyRefreshHint();
    this.refreshLiveView(true);
    hilog.info(DOMAIN, TAG, 'entered background, keep receiving');
  }

  private exitBackground(): void {
    this.inBackground = false;
    this.resumeRendering();
    if (this.netOpt && supportsNetOpt(this.playMode)) {
      this.booster.enter();
    }
    this.applyRefreshHint();
    this.refreshLiveView(true);
    hilog.info(DOMAIN, TAG, 'back to foreground after %{public}d s',
      Math.round((Date.now() - this.backgroundSince) / 1000));
  }

  // 用户在通知栏删除了实况窗（长时任务随之被系统取消）：
  // 在后台时当作“停止串流”，立刻断开；前台播放时只关闭后台保持，不打断观看
  private onKeepAliveCanceled(): void {
    if (this.liveViewTimer >= 0) {
      clearInterval(this.liveViewTimer);
      this.liveViewTimer = -1;
    }
    if (!this.playing) {
      return;
    }
    if (this.inBackground) {
      hilog.info(DOMAIN, TAG, 'live view removed in background, stopping stream');
      this.leavePlayer();
    } else {
      promptAction.showToast({ message: tr('app_toast_keepalive_off') });
    }
  }

  private resumeRendering(): void {
    if (this.surfaceId.length === 0) {
      return; // surface 还没就绪，等 onSurfaceCreated 再恢复
    }
    const err = sysdvr.resumeVideo(this.surfaceId);
    if (err.length > 0) {
      hilog.error(DOMAIN, TAG, 'resume failed: %{public}s', err);
      promptAction.showToast({ message: tr('app_toast_resume_failed', err) });
    }
  }

  private refreshLiveView(force: boolean): void {
    const keeper = this.keeper;
    const t = this.target;
    if (keeper === null || !keeper.isRunning() || t === null) {
      return;
    }
    this.liveViewTick++;
    if (this.inBackground) {
      const budgetMs = this.keepAliveMinutes() * 60000;
      const elapsed = Date.now() - this.backgroundSince;
      if (budgetMs > 0 && elapsed >= budgetMs) {
        hilog.info(DOMAIN, TAG, 'background keep-alive expired, disconnecting');
        this.leavePlayer();
        return;
      }
      const s = sysdvr.getStats();
      const mb = ((s.videoBytes + s.audioBytes) / 1e6).toFixed(1);
      const link = s.videoConnected ? tr('app_live_receiving') : tr('app_live_reconnecting');
      keeper.update({
        title: tr('app_live_title_bg', APP_NAME),
        text: tr('app_live_text_bg', t.label, link, mb, formatDuration(budgetMs - elapsed)),
        progress: budgetMs > 0 ? elapsed * 100 / budgetMs : 0
      });
    } else if (force || this.liveViewTick % FOREGROUND_REFRESH_TICKS === 0) {
      keeper.update({ title: tr('app_live_title_fg', APP_NAME), text: tr('app_live_text_fg', t.label), progress: 0 });
    }
  }

  // ---------------------------------------------------------------- 统计与退出

  private pollStats(): void {
    if (this.playMode === StreamMode.RTSP) {
      this.pollRtspStats();
      return;
    }
    const s = sysdvr.getStats();
    const prev = this.lastStats;
    if (this.netWatcher.tick(Date.now())) {
      sysdvr.setNetworkGuard(this.netWatcher.guard);
    }
    for (const note of this.netWatcher.takeNotes()) {
      this.recorder.note(note);
    }
    this.rememberTarget(this.playMode === StreamMode.TCP && s.videoConnected && s.videoPackets > 0);
    let recvFps = 0;
    let fps = 0;
    let kbps = 0;
    let linkMbps = 0;
    let audioKbps = 0;
    if (prev !== null) {
      audioKbps = Math.max(0, Math.round((s.audioBytes - prev.audioBytes) * 8 / 1000));
      // Switch 一帧一个包，收包数/秒就是“收到的帧率”；和渲染帧率对比就能看出瓶颈在传输还是客户端
      recvFps = s.videoPackets - prev.videoPackets;
      fps = s.framesRendered - prev.framesRendered;
      kbps = Math.round((s.videoBytes - prev.videoBytes) * 8 / 1000);
      linkMbps = (s.videoBytes + s.audioBytes - prev.videoBytes - prev.audioBytes) * 8 / 1e6;
    }
    this.lastStats = s;
    if (this.playMode === StreamMode.USB && s.message !== this.lastUsbMessage) {
      // USB 会话的状态变化写进 usb.log（这台手机上看不到 App 的 hilog）
      this.lastUsbMessage = s.message;
      UsbLog.write(`session: ${s.message}`);
    }
    // 画面起来之前显示连接状态，起来之后收起；丢帧定格时这一秒可能一帧都没出，但画面是有意停住的，不显示“连接中”
    this.statusText = s.videoConnected && (fps > 0 || s.frozen) ? '' : s.message;

    let video = tr('app_stats_video', s.videoConnected ? '●' : '○', recvFps, fps, kbps, s.videoDropped);
    if (s.smoothLevel > 0) {
      video += tr('app_stats_smooth', smoothLabels()[s.smoothLevel], s.smoothDelayMs, s.videoLate);
    }
    let audio = this.playMode === StreamMode.TCP ? tr('app_stats_audio_off_tcp') : tr('app_stats_audio_off');
    if (s.audioEnabled) {
      audio = tr('app_stats_audio', s.audioConnected ? '●' : '○', s.audioBufferedMs, s.audioTargetMs, s.audioUnderruns,
        s.audioFastMode ? tr('app_stats_audio_fast') : tr('app_stats_audio_normal'));
    }
    const linkName = this.playMode === StreamMode.USB ? 'USB' : tr('app_stats_link_network');
    const net = tr('app_stats_net', linkName, linkMbps.toFixed(1), s.replayHits, s.resyncs, s.reconnects);
    if (this.playMode === StreamMode.TCP) {
      this.updateExt(s, prev);
    }
    const ext = this.extStatsText(s, prev, audioKbps);
    let text = `${s.message}\n${video}\n${audio}${ext.length > 0 ? '\n' + ext : ''}\n${net}`;
    if (this.playMode === StreamMode.TCP) {
      text += '\n' + (s.netOpt ? tr('app_stats_netopt_on', this.booster.status) : tr('app_stats_netopt_off'));
      const quality = this.netWatcher.statsText(Date.now(), s.smoothLevel > 0);
      if (quality.length > 0) {
        text += '\n' + quality;
      }
      if (this.netOpt && !this.inBackground) {
        this.booster.updateFlows(sysdvr.getSocketFds());
      }
    } else if (this.target !== null && this.target.usb !== null) {
      text += '\n' + tr('app_stats_transport', this.target.usb.transport);
    }
    const resume = s.lastCatchUpMs >= 0 ? `${s.lastCatchUpMs} ms` : '—';
    text += '\n' + tr('app_stats_gop', s.gopPackets, Math.round(s.gopBytes / 1024), resume);
    text += '\n' + this.lossStatsText(s);
    text += '\n' + this.renderStatsText(s);
    const enhance = this.enhanceStatsText(s);
    if (enhance.length > 0) {
      text += '\n' + enhance;
    }
    const channel = this.channelStatsText();
    if (channel.length > 0) {
      text += '\n' + channel;
    }
    if (s.videoConnected) {
      text += '\n' + this.memoryStatsText(s);
      this.recordMemory(s);
    }
    if (!this.enhanceWarned && s.enhanceRequested > 0 && s.enhanceLevel === 0 && s.enhanceError.length > 0) {
      // 设置里开了画质增强但本机不支持：只提示一次，播放照常进行
      this.enhanceWarned = true;
      promptAction.showToast({ message: tr('app_toast_enhance_unavailable', s.enhanceError), duration: 3500 });
    }
    if (prev !== null) {
      // 各项增量取非负：底层计数理论上单调递增，这里再兜一层底
      const delta = (now: number, before: number): number => Math.max(0, now - before);
      const ms1 = (us: number): number => Math.round(us / 100) / 10;
      const pct = (permille: number): number => permille === 0xFFFFFFFF ? -1 : permille / 10;
      this.recorder.write([this.inBackground ? 1 : 0, Math.max(0, recvFps), Math.max(0, fps), Math.max(0, kbps),
        audioKbps, delta(s.videoDropped, prev.videoDropped), s.smoothLevel, s.smoothDelayMs,
        delta(s.videoLate, prev.videoLate), s.audioEnabled ? 1 : 0, s.audioBufferedMs, s.audioTargetMs,
        delta(s.audioUnderruns, prev.audioUnderruns), s.netOpt ? 1 : 0, s.reconnects, s.resyncs,
        delta(s.lossGap, prev.lossGap), delta(s.lostFrames, prev.lostFrames), delta(s.lossError, prev.lossError),
        delta(s.lossReplayMiss, prev.lossReplayMiss), delta(s.lossResync, prev.lossResync),
        delta(s.lossOversize, prev.lossOversize), s.freezeOnLoss ? 1 : 0, s.frozen ? 1 : 0,
        delta(s.freezeEvents, prev.freezeEvents), delta(s.freezeTotalMs, prev.freezeTotalMs),
        delta(s.freezeGiveUps, prev.freezeGiveUps), Math.round(s.frameIntervalUs / 100) / 10, s.keyframeIntervalMs,
        Math.round(s.vsyncPeriodUs / 100) / 10, delta(s.vsyncDeferred, prev.vsyncDeferred),
        delta(s.vsyncDropped, prev.vsyncDropped), delta(s.stutters, prev.stutters), s.renderJitterMs,
        s.renderMaxGapMs, s.enhanceLevel, Math.round(s.enhanceLatencyUs / 100) / 10, this.hotspotChannel,
        this.staChannel,
        s.extSupported ? 1 : 0, s.audioCodec, s.audioKbpsConfig, s.opusComplexity, s.opusFrameMs,
        delta(s.audioEncodeUs, prev.audioEncodeUs), delta(s.audioDecodeErrors, prev.audioDecodeErrors),
        delta(s.diagTotalIntervalMs, prev.diagTotalIntervalMs), delta(s.diagTotalFramesSent, prev.diagTotalFramesSent),
        delta(s.diagTotalGrcGaps, prev.diagTotalGrcGaps), ms1(delta(s.diagTotalBlockUs, prev.diagTotalBlockUs)),
        delta(s.diagTotalSendsOver20ms, prev.diagTotalSendsOver20ms),
        delta(s.diagTotalGapsAfterSlowSend, prev.diagTotalGapsAfterSlowSend),
        delta(s.diagTotalAudioPackets, prev.diagTotalAudioPackets),
        delta(s.diagTotalAudioEncodeUs, prev.diagTotalAudioEncodeUs),
        ms1(delta(s.diagTotalAudioBlockUs, prev.diagTotalAudioBlockUs)), ms1(s.diagBlockMaxUs),
        pct(s.diagCore3IdlePermille), pct(s.diagCpuPermille), s.diagTosFlags,
        this.rotation.running() ? this.rotation.index + 1 : 0].concat(this.netWatcher.csvFields()));
    }
    if (s.decoderError.length > 0) {
      text += `\n${s.decoderError}`;
    }
    this.statsText = text;
  }

  // ---------------------------------------------------------------- 实验扩展（仅 TCP Bridge）

  /** 握手时请求的编码：开了自动轮换就用第一组，否则用当前设置 */
  private initialCombo(): AudioCombo {
    const plan = parsePlan(this.rotatePlan);
    return this.rotateOn && plan.length >= 2 ? plan[0] : currentCombo();
  }

  /** 自动轮换每组时长（分钟） */
  private rotateMinutes(): number {
    const i = this.rotateMinutesIndex;
    return i >= 0 && i < ROTATE_MINUTES.length ? ROTATE_MINUTES[i] : 2;
  }

  /** 实验选项是否显示在快捷设置里：本次服务端支持、这台设备以前支持过，或开了开发者选项 */
  private extShown(): boolean {
    if (this.playMode !== StreamMode.TCP) {
      return false;
    }
    const t = this.target;
    return this.extActive || this.devMode || (t !== null && isExtHost(t.host));
  }

  // 每秒随统计调用：服务端表明支持扩展后记住设备、推进自动轮换
  private updateExt(s: SessionStats, prev: SessionStats | null): void {
    if (!s.extSupported) {
      return;
    }
    this.extActive = true;
    const t = this.target;
    if (!this.extRemembered && t !== null) {
      rememberExtHost(t.host);
      this.extRemembered = true;
      hilog.info(DOMAIN, TAG, 'server supports the NSDVR extension: %{public}s', t.host);
    }
    if (this.rotateOn && !this.rotation.running()) {
      this.startRotation();
    }
    if (!this.rotation.running()) {
      return;
    }
    if (prev !== null) {
      const delta = (now: number, before: number): number => Math.max(0, now - before);
      this.rotation.add({
        audioBytes: delta(s.audioBytes, prev.audioBytes),
        underruns: delta(s.audioUnderruns, prev.audioUnderruns),
        decodeErrors: delta(s.audioDecodeErrors, prev.audioDecodeErrors),
        lostFrames: delta(s.lostFrames, prev.lostFrames),
        lossEvents: delta(s.lossGap + s.lossError + s.lossReplayMiss + s.lossResync,
          prev.lossGap + prev.lossError + prev.lossReplayMiss + prev.lossResync),
        encodeUs: delta(s.audioEncodeUs, prev.audioEncodeUs),
        diagMs: delta(s.diagTotalIntervalMs, prev.diagTotalIntervalMs),
        diagEncodeUs: delta(s.diagTotalAudioEncodeUs, prev.diagTotalAudioEncodeUs),
        grcGaps: delta(s.diagTotalGrcGaps, prev.diagTotalGrcGaps),
        gapsAfterSlow: delta(s.diagTotalGapsAfterSlowSend, prev.diagTotalGapsAfterSlowSend),
        blockUs: delta(s.diagTotalBlockUs, prev.diagTotalBlockUs),
        sendsOver20ms: delta(s.diagTotalSendsOver20ms, prev.diagTotalSendsOver20ms)
      });
    }
    const now = Date.now();
    if (this.rotation.remainingMs(now) <= 0) {
      // 先把这一组的汇总写进 stats.csv，再切到下一组
      this.recorder.note(this.rotation.summary());
      const next = this.rotation.tick(now);
      if (next !== null) {
        this.sendCombo(next, true);
      }
    }
    this.updateRotationText();
  }

  private updateRotationText(): void {
    const c = this.rotation.current();
    this.rotationText = c === null ? '' : tr('ext_quick_rotate_state', this.rotation.index + 1, this.rotation.plan.length,
      formatDuration(this.rotation.remainingMs(Date.now())));
  }

  private startRotation(): void {
    const plan = parsePlan(this.rotatePlan);
    if (plan.length < 2) {
      return;
    }
    const first = this.rotation.start(plan, this.rotateMinutes(), Date.now());
    this.rotating = true;
    this.recorder.note(`rotation_start combos=${plan.map((c: AudioCombo) => comboTag(c)).join(',')} ` +
      `minutes=${this.rotateMinutes()}`);
    if (first !== null) {
      this.sendCombo(first, true);
    }
    this.updateRotationText();
  }

  /** 停止轮换，恢复成手动设置的编码 */
  private stopRotation(): void {
    if (this.rotation.running()) {
      this.recorder.note(this.rotation.summary());
      this.recorder.note('rotation_stop');
    }
    this.rotation.stop();
    this.rotating = false;
    this.rotationText = '';
    if (this.sessionStarted && this.extActive) {
      this.sendCombo(currentCombo(), false);
    }
  }

  /** 把一组编码参数发给 native（控制消息，不重连）；返回空串表示已发出或已记下 */
  private sendCombo(c: AudioCombo, fromRotation: boolean): string {
    const err = sysdvr.setExtAudio(c.codec, c.kbps, c.complexity, c.frameMs);
    if (fromRotation) {
      this.recorder.note(`rotation_switch combo=${this.rotation.index + 1}/${this.rotation.plan.length} ${comboTag(c)}`);
    }
    hilog.info(DOMAIN, TAG, 'ext audio -> %{public}s %{public}s', comboTag(c), err);
    return err;
  }

  /** 快捷设置里改了编码或 Opus 参数：记下设置，播放中立即生效 */
  private applyExtAudio(): void {
    if (!this.sessionStarted || this.playMode !== StreamMode.TCP || this.rotation.running()) {
      return;
    }
    const err = this.sendCombo(currentCombo(), false);
    if (err.length > 0) {
      promptAction.showToast({ message: err });
    }
  }

  private applyRotate(on: boolean): void {
    if (on === this.rotateOn) {
      return;
    }
    if (on && parsePlan(this.rotatePlan).length < 2) {
      promptAction.showToast({ message: tr('ext_rotate_need_two') });
      return;
    }
    this.rotateOn = on;
    if (!this.sessionStarted || this.playMode !== StreamMode.TCP) {
      return;
    }
    if (!on) {
      this.stopRotation();
    } else if (this.extActive) {
      this.startRotation();
    } else {
      promptAction.showToast({ message: tr('ext_unsupported') });
    }
  }

  // 统计浮层里的实验扩展部分：当前编码与实际码率、Switch 编码耗时占实时的比例、诊断数据、轮换进度
  private extStatsText(s: SessionStats, prev: SessionStats | null, audioKbps: number): string {
    if (this.playMode !== StreamMode.TCP) {
      return '';
    }
    if (!s.extSupported) {
      return this.devMode ? tr('ext_stats_unsupported') : '';
    }
    const lines: string[] = [];
    const codec = s.audioCodec >= 0 ? comboLabel({
      codec: s.audioCodec, kbps: s.audioKbpsConfig, complexity: s.opusComplexity, frameMs: s.opusFrameMs
    }) : '—';
    // Switch 编码耗时占实时：优先用诊断包（audioEncodeTotalUs / intervalMs），没有诊断时按音频包头里的耗时估算
    let enc = '—';
    if (s.diagPackets > 0 && s.diagIntervalMs > 0) {
      enc = `${(s.diagAudioEncodeUs / s.diagIntervalMs / 10).toFixed(2)}%`;
    } else if (prev !== null && s.audioCodec > 0) {
      enc = `${(Math.max(0, s.audioEncodeUs - prev.audioEncodeUs) / 10000).toFixed(2)}%`;
    }
    let audio = tr('ext_stats_audio', codec, audioKbps, enc);
    if (s.extAudioPending) {
      const now = Date.now();
      if (this.extPendingSince === 0) {
        this.extPendingSince = now;
      }
      audio += now - this.extPendingSince > 3000 ? tr('ext_stats_not_applied') : tr('ext_stats_pending');
    } else {
      this.extPendingSince = 0;
    }
    if (s.audioDecodeErrors > 0) {
      audio += tr('ext_stats_dec_err', s.audioDecodeErrors);
    }
    lines.push(audio);
    if (s.diagPackets > 0) {
      const perSec = s.diagIntervalMs > 0 ? s.diagBlockTotalUs / s.diagIntervalMs : 0;
      lines.push(tr('ext_stats_send', perSec.toFixed(1), (s.diagBlockMaxUs / 1000).toFixed(1), s.diagSendsOver20ms,
        s.diagTotalSendsOver20ms, s.diagGrcGaps, s.diagTotalGrcGaps, s.diagTotalGapsAfterSlowSend));
      const pct = (permille: number): string => permille === 0xFFFFFFFF ? '—' : `${(permille / 10).toFixed(1)}%`;
      // 3 号核心显示占用率（服务端报的是空闲千分比）
      const core3Used = s.diagCore3IdlePermille === 0xFFFFFFFF ? 0xFFFFFFFF : Math.max(0, 1000 - s.diagCore3IdlePermille);
      lines.push(tr('ext_stats_cpu', pct(core3Used), pct(s.diagCpuPermille),
        (s.diagTosFlags & 1) !== 0 ? '✓' : '✗', (s.diagTosFlags & 2) !== 0 ? '✓' : '✗'));
    }
    const c = this.rotation.current();
    if (c !== null) {
      lines.push(tr('ext_stats_rotate', this.rotation.index + 1, this.rotation.plan.length, comboLabel(c),
        formatDuration(this.rotation.remainingMs(Date.now()))));
    }
    return lines.join('\n');
  }

  // 丢帧与定格：原因分类 + 定格次数/累计/最近一次 + 帧间隔与关键帧间隔
  private lossStatsText(s: SessionStats): string {
    const events = s.lossGap + s.lossError + s.lossReplayMiss + s.lossResync + s.lossOversize;
    let text = tr('app_stats_loss', events, s.lostFrames, s.lossGap, s.lossError, s.lossReplayMiss, s.lossResync,
      s.lossOversize);
    if (s.freezeOnLoss) {
      const last = s.lastFreezeMs >= 0 ? `${s.lastFreezeMs} ms` : '—';
      text += tr('app_stats_freeze', s.freezeEvents, (s.freezeTotalMs / 1000).toFixed(1), last, s.freezeGiveUps,
        s.frozen ? tr('app_stats_frozen_now') : '');
    } else {
      text += tr('app_stats_freeze_off');
    }
    const keyframe = s.keyframeIntervalMs > 0 ? `${(s.keyframeIntervalMs / 1000).toFixed(1)} s` : '—';
    text += tr('app_stats_intervals', (s.frameIntervalUs / 1000).toFixed(1), keyframe);
    return text;
  }

  // 上屏节奏：刷新率、顺延/跳过、卡顿次数、最近一秒的抖动和最长间隔
  private renderStatsText(s: SessionStats): string {
    const hz = s.vsyncPeriodUs > 0 ? Math.round(1e6 / s.vsyncPeriodUs) : 0;
    return tr('app_stats_render', hz, s.vsyncDeferred, s.vsyncDropped, s.stutters, s.renderJitterMs.toFixed(1),
      s.renderMaxGapMs);
  }

  private enhanceStatsText(s: SessionStats): string {
    if (s.enhanceRequested <= 0) {
      return '';
    }
    if (s.enhanceLevel > 0) {
      return tr('app_stats_enhance_on', enhanceLabels()[s.enhanceLevel], (s.enhanceLatencyUs / 1000).toFixed(1));
    }
    return tr('app_stats_enhance_off', s.enhanceError.length > 0 ? s.enhanceError : '—');
  }

  // Switch 内存（握手时 sysmodule 报告，用来评估服务端还有多少内存余量）
  private memoryStatsText(s: SessionStats): string {
    if (!s.switchMemValid) {
      return tr('app_stats_mem_none');
    }
    const mb = (used: number, size: number): string => ((size - used) / 1048576).toFixed(1);
    return tr('app_stats_mem', mb(s.systemPoolUsed, s.systemPoolSize), (s.systemPoolSize / 1048576).toFixed(1),
      mb(s.appletPoolUsed, s.appletPoolSize), (s.appletPoolSize / 1048576).toFixed(1));
  }

  private recordMemory(s: SessionStats): void {
    const mb = (b: number): string => (b / 1048576).toFixed(2);
    const line = s.switchMemValid ?
      `switch_memory system_free_mb=${mb(s.systemPoolSize - s.systemPoolUsed)} system_mb=${mb(s.systemPoolSize)} ` +
        `applet_free_mb=${mb(s.appletPoolSize - s.appletPoolUsed)} applet_mb=${mb(s.appletPoolSize)} ` +
        `application_free_mb=${mb(s.applicationPoolSize - s.applicationPoolUsed)} ` +
        `application_mb=${mb(s.applicationPoolSize)} system_unsafe_free_mb=` +
        `${mb(s.systemUnsafeSize - s.systemUnsafeUsed)} system_unsafe_mb=${mb(s.systemUnsafeSize)}` :
      `switch_memory unavailable result=${s.switchMemQueryResult}`;
    if (line === this.lastMemoryNote) {
      return;
    }
    this.lastMemoryNote = line;
    this.recorder.note(line);
    hilog.info(DOMAIN, TAG, '%{public}s', line);
    if (this.playMode === StreamMode.USB) {
      UsbLog.write(line);
    }
  }

  // 专用热点与手机 WLAN 的信道：不同信道时芯片要来回切换，会周期性掉帧
  private channelStatsText(): string {
    if (this.playMode === StreamMode.USB || this.hotspotChannel <= 0) {
      return '';
    }
    if (this.staChannel <= 0) {
      return tr('app_stats_channel_only', this.hotspotChannel);
    }
    return this.staChannel === this.hotspotChannel ? tr('app_stats_channel_same', this.hotspotChannel) :
      tr('app_stats_channel_diff', this.hotspotChannel, this.staChannel);
  }

  /** 连上之后把地址记进“最近连接”，每次会话只记一次；USB 没有地址，不记 */
  private rememberTarget(connected: boolean): void {
    const t = this.target;
    if (!connected || this.hostRemembered || t === null || t.mode === StreamMode.USB || t.host.length === 0) {
      return;
    }
    this.hostRemembered = true;
    rememberHost(t.mode, t.host, t.name ?? '');
  }

  private pollRtspStats(): void {
    const r = this.rtsp.getStats();
    this.statusText = r.playing ? '' : r.message;
    this.rememberTarget(r.playing);
    const decoder = r.decoder === 2 ? tr('app_stats_decoder_hw') : (r.decoder === 1 ? tr('app_stats_decoder_sw') : '—');
    this.statsText = `${r.message}\n` +
      tr('app_stats_rtsp_video', r.playing ? '●' : '○', r.decodeFps.toFixed(0), r.outputFps.toFixed(0), r.kbps, decoder) +
      '\n' + tr('app_stats_rtsp_buffer', r.videoCachedMs, r.audioCachedMs, r.dropFrameRate.toFixed(2)) + '\n' +
      tr('app_stats_rtsp_misc', this.enableAudio ? tr('app_on') : tr('app_muted'), r.reconnects);
    if (!this.inBackground) {
      this.recorder.write([0, Math.round(r.decodeFps), Math.round(r.outputFps), r.kbps, 0, 0, 0, 0, 0,
        this.enableAudio ? 1 : 0, r.audioCachedMs, 0, 0, 0, r.reconnects, 0]);
    }
  }

  private stopPlayback(): void {
    if (this.statsTimer >= 0) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
    if (this.liveViewTimer >= 0) {
      clearInterval(this.liveViewTimer);
      this.liveViewTimer = -1;
    }
    this.cancelPipCheck();
    this.pip.close();
    this.rtsp.stop();
    // 顺序：先停兼容传输的读循环、再停 native 会话，最后关闭 USB 设备
    this.usbPump.stop();
    sysdvr.disconnect();
    this.usbWatcher.stop();
    const t = this.target;
    if (t !== null && t.usb !== null) {
      UsbSwitch.close(t.usb);
      t.usb = null;
    }
    this.booster.leave();
    this.netWatcher.stop();
    if (this.rotation.running()) {
      this.recorder.note(this.rotation.summary());
    }
    this.rotation.stop();
    this.rotating = false;
    this.rotationText = '';
    this.extActive = false;
    this.recorder.stop();
    this.sessionStarted = false;
    this.inBackground = false;
    this.applyRefreshHint();
    this.showPanel = false;
    this.lastStats = null;
    if (this.keeper !== null) {
      this.keeper.stop();
    }
  }

  // ---------------------------------------------------------------- 播放页快捷设置（即时生效，不用重连）

  private applyAudio(on: boolean): void {
    if (on === this.enableAudio) {
      return;
    }
    this.enableAudio = on;
    if (!this.sessionStarted) {
      return;
    }
    if (this.playMode === StreamMode.RTSP) {
      this.rtsp.setAudioEnabled(on);
      return;
    }
    const err = sysdvr.setAudioEnabled(on);
    if (err.length > 0) {
      this.enableAudio = !on;
      promptAction.showToast({ message: err });
    }
    this.pip.setAudio(this.enableAudio);
  }

  private applySmooth(level: number): void {
    if (level === this.smoothLevel) {
      return;
    }
    this.smoothLevel = level;
    if (this.sessionStarted) {
      sysdvr.setSmoothLevel(level);
    }
  }

  private applyFreeze(on: boolean): void {
    if (on === this.freezeOnLoss) {
      return;
    }
    this.freezeOnLoss = on;
    if (this.sessionStarted && supportsFreeze(this.playMode)) {
      sysdvr.setFreezeOnLoss(on);
    }
  }

  private applyEnhance(level: number): void {
    if (level === this.enhanceLevel) {
      return;
    }
    this.enhanceLevel = level;
    if (!this.sessionStarted || !supportsEnhance(this.playMode)) {
      return;
    }
    // 开关画质增强会重建视频链路（重放缓存的画面，一般不到半秒）；失败时已退回直出
    const err = sysdvr.setEnhanceLevel(level);
    this.enhanceWarned = err.length > 0;
    if (err.length > 0) {
      promptAction.showToast({ message: tr('app_toast_enhance_unavailable', err), duration: 3500 });
    }
  }

  private applyNetOpt(on: boolean): void {
    if (on === this.netOpt) {
      return;
    }
    this.netOpt = on;
    if (!this.sessionStarted) {
      return;
    }
    sysdvr.setNetOptimization(on);
    if (on && !this.inBackground) {
      this.booster.enter();
    } else {
      this.booster.leave();
    }
  }

  private applyKeepAlive(index: number): void {
    if (index === this.keepAliveIndex) {
      return;
    }
    this.keepAliveIndex = index;
    const keeper = this.keeper;
    if (!this.playing || keeper === null) {
      return;
    }
    if (KEEP_ALIVE_MINUTES[index] > 0 && !keeper.isRunning()) {
      // 当前在前台，可以申请长时任务
      this.suppressPageHide = true;
      this.startKeeper().then(() => {
        this.suppressPageHide = false;
      });
    } else if (KEEP_ALIVE_MINUTES[index] === 0 && keeper.isRunning()) {
      if (this.liveViewTimer >= 0) {
        clearInterval(this.liveViewTimer);
        this.liveViewTimer = -1;
      }
      keeper.stop();
    }
  }

  private leavePlayer(): void {
    this.stopPlayback();
    this.locked = false;
    this.controlsVisible = true;
    this.playing = false;
    this.target = null;
    this.statusText = '';
    this.setImmersive(false);
    if (this.languageStale) {
      // 播放中界面语言变了：退出播放后再重建首页（等退出全屏、转回竖屏）
      setTimeout(() => {
        if (!this.playing) {
          this.rebuildForLanguage();
        }
      }, 300);
    }
  }

  // 播放时全屏、常亮；手机和平板横屏。折叠屏展开、PC 窗口保持当前方向
  private async setImmersive(on: boolean): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const win = await window.getLastWindow(ctx);
      await win.setWindowKeepScreenOn(on);
      const type = deviceInfo.deviceType;
      if (type === '2in1') {
        return;
      }
      await win.setWindowLayoutFullScreen(on);
      await win.setWindowSystemBarEnable(on ? [] : ['status', 'navigation']);
      const rotate = type === 'tablet' || (type === 'phone' && this.bp === 'sm');
      await win.setPreferredOrientation(on && rotate ? window.Orientation.AUTO_ROTATION_LANDSCAPE :
        window.Orientation.UNSPECIFIED);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'setImmersive failed: %{public}s', JSON.stringify(e));
    }
  }

  // ---------------------------------------------------------------- 播放页：锁定与控件显隐

  private onPlayerTap(): void {
    if (this.locked) {
      this.flashLockHint();
      return;
    }
    this.controlsVisible = !this.controlsVisible;
    if (!this.controlsVisible) {
      this.showPanel = false;
    }
  }

  private setLocked(on: boolean): void {
    this.locked = on;
    this.showPanel = false;
    if (on) {
      this.flashLockHint();
    } else {
      this.controlsVisible = true;
    }
  }

  // 锁形图标亮显几秒后变淡，既能找到又不挡画面
  private flashLockHint(): void {
    this.lockHintBright = true;
    if (this.lockHintTimer >= 0) {
      clearTimeout(this.lockHintTimer);
    }
    this.lockHintTimer = setTimeout(() => {
      this.lockHintTimer = -1;
      this.lockHintBright = false;
    }, LOCK_HINT_MS);
  }

  private openGuide(): void {
    this.navStack.pushPathByName('guide', null);
  }

  // ---------------------------------------------------------------- 界面

  build() {
    Stack() {
      Navigation(this.navStack) {
        this.HomeShell()
      }
      .hideTitleBar(true)
      .hideToolBar(true)
      .mode(NavigationMode.Stack)
      .navDestination(this.PageMap)
      .width('100%')
      .height('100%')
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
      .visibility(this.playing ? Visibility.Hidden : Visibility.Visible)

      if (this.playing) {
        this.PlayerView()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.playing ? Color.Black : $r('app.color.page_bg'))
    // 背景延伸到状态栏和底部小白条区域，页面与系统栏融为一体
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
    .onAreaChange((_old: Area, area: Area) => {
      const w = Number(area.width);
      const h = Number(area.height);
      this.bp = breakpointOf(w);
      // 播放时（横屏全屏）不动首页导航，免得退出播放后导航位置闪一下
      if (!this.playing) {
        this.navSide = w >= 600 && w > h;
      }
    })
  }

  @Builder
  PageMap(name: string) {
    if (name === 'guide') {
      NavDestination() {
        GuideView({ bp: this.bp })
      }
      .title(tr('app_guide_title'))
      .backgroundColor($r('app.color.page_bg'))
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
    }
  }

  private tabBarStyle(index: number): BottomTabBarStyle {
    const style = BottomTabBarStyle.of({
      normal: new SymbolGlyphModifier(TAB_ICONS[index]).fontColor([$r('app.color.text_secondary')]),
      selected: new SymbolGlyphModifier(TAB_ICONS[index]).fontColor([$r('app.color.accent')])
    }, tr(TAB_TITLES[index])).labelStyle({
      unselectedColor: $r('app.color.text_secondary'),
      selectedColor: $r('app.color.accent')
    });
    if (!this.useFloating && !this.navSide) {
      // 普通底部页签：延伸到小白条区域，文字图标留在上方
      style.padding({ bottom: this.safeBottom });
    }
    return style;
  }

  @Builder
  TabPages() {
    TabContent() {
      ConnectView({
        bp: this.bp,
        playing: this.playing,
        onConnect: (t: StreamTarget) => {
          this.play(t);
        },
        onOpenGuide: () => {
          this.openGuide();
        }
      })
    }
    .tabBar(this.tabBarStyle(0))
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])

    TabContent() {
      ConfigView({ bp: this.bp })
    }
    .tabBar(this.tabBarStyle(1))
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])

    TabContent() {
      AboutView({
        bp: this.bp,
        onOpenGuide: () => {
          this.openGuide();
        }
      })
    }
    .tabBar(this.tabBarStyle(2))
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
  }

  // 首页框架：竖屏底部悬浮页签（沉浸光感）；横屏（平板、PC、折叠屏展开横放）左侧导航栏
  @Builder
  HomeShell() {
    if (this.useFloating && !this.navSide) {
      HdsTabs({ barPosition: BarPosition.End, index: this.tab }) {
        this.TabPages()
      }
      .barOverlap(true)
      .barFloatingStyle({
        // HdsTabs 悬浮页签自己会避让底部小白条区域，这里只加与小白条之间的间距（再加 safeBottom 会抬高一倍）
        barBottomMargin: 8,
        gradientMask: { maskColor: $r('app.color.tab_mask') },
        systemMaterialEffect: { materialType: this.material, materialLevel: hdsMaterial.MaterialLevel.ADAPTIVE }
      })
      .scrollable(false)
      .animationDuration(0)
      .onChange((index: number) => {
        this.tab = index;
      })
      .width('100%')
      .height('100%')
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
    } else {
      Tabs({ barPosition: this.navSide ? BarPosition.Start : BarPosition.End, index: this.tab }) {
        this.TabPages()
      }
      .vertical(this.navSide)
      .barWidth(this.navSide ? 96 : '100%')
      .barHeight(this.navSide ? '100%' : 56 + this.safeBottom)
      .barMode(BarMode.Fixed)
      .barOverlap(!this.navSide)
      .barBackgroundBlurStyle(BlurStyle.COMPONENT_THICK)
      .barBackgroundColor(this.navSide ? $r('app.color.bar_bg') : Color.Transparent)
      .scrollable(false)
      .animationDuration(0)
      .onChange((index: number) => {
        this.tab = index;
      })
      .width('100%')
      .height('100%')
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.BOTTOM])
    }
  }

  @Builder
  CapsuleButton(icon: Resource, text: string, bg: ResourceColor, onTap: () => void) {
    Button({ type: ButtonType.Capsule }) {
      Row({ space: 4 }) {
        SymbolGlyph(icon).fontSize(16).fontColor([Color.White])
        Text(text).fontSize(12).fontColor(Color.White)
      }
      .padding({ left: 12, right: 12 })
    }
    .height(34)
    .backgroundColor(bg)
    .onClick(() => {
      onTap();
    })
  }

  // 注意：直接读 this.locked，不能作为 @Builder 参数按值传入（按值传的参数不会随状态刷新）
  @Builder
  LockButton() {
    Row() {
      SymbolGlyph(this.locked ? $r('sys.symbol.lock_fill') : $r('sys.symbol.lock_open'))
        .fontSize(22)
        .fontColor([Color.White])
    }
      .width(48)
      .height(48)
      .justifyContent(FlexAlign.Center)
      .borderRadius(24)
      .backgroundColor('#66000000')
      .opacity(this.locked && !this.lockHintBright ? 0.35 : 1)
      .animation({ duration: 300 })
      .onClick(() => {
        this.setLocked(!this.locked);
      })
      .accessibilityText(this.locked ? tr('app_unlock_screen') : tr('app_lock_screen'))
  }

  @Builder
  PlayerView() {
    Stack() {
      if (this.playMode === StreamMode.RTSP) {
        // ijkplayer 自己管理这块 surface；按 16:9 适配窗口，避免拉伸
        XComponent({ id: RTSP_XCOMPONENT_ID, type: XComponentType.SURFACE, libraryname: 'ijkplayer_napi' })
          .onLoad((context?: object) => {
            if (context !== undefined) {
              this.onRtspLoad(context);
            }
          })
          .width(this.playerWidth)
          .height(this.playerHeight)
      } else {
        // 和 RTSP 一样按 16:9 适配窗口：画质增强接管输出后不认 native 侧设的 FIT 缩放模式，
        // surface 本身是 16:9 才不会被拉伸；直出时效果不变
        XComponent({ type: XComponentType.SURFACE, controller: this.xController })
          .width(this.playerWidth)
          .height(this.playerHeight)
      }

      if (!this.locked) {
        Column() {
          Row({ space: 8 }) {
            if (this.controlsVisible && this.showStats && this.statsText.length > 0) {
              // 统计行数较多：从锁形按钮右侧开始排，限制宽度，不压住左边的锁和右上角按钮
              Text(this.statsText)
                .fontSize(11)
                .lineHeight(16)
                .fontColor(Color.White)
                .backgroundColor('#99000000')
                .padding(8)
                .borderRadius(8)
                .margin({ left: 56 })
                .constraintSize({ maxWidth: '72%' })
                .hitTestBehavior(HitTestMode.Transparent)
            } else if (this.statusText.length > 0) {
              // 连接中的状态不受控件显隐影响，避免以为卡住了
              Row({ space: 8 }) {
                LoadingProgress().width(18).height(18).color(Color.White)
                Text(this.statusText).fontSize(13).fontColor(Color.White)
              }
              .padding({ left: 12, right: 12, top: 6, bottom: 6 })
              .backgroundColor('#99000000')
              .borderRadius(16)
              .hitTestBehavior(HitTestMode.Transparent)
            }
            Blank()
            if (this.controlsVisible) {
              if (this.pipSupported && this.playMode !== StreamMode.RTSP) {
                this.CapsuleButton($r('sys.symbol.rectangle_on_square'), tr('app_btn_pip'), '#66000000', () => {
                  this.startPip();
                })
              }
              this.CapsuleButton($r('sys.symbol.gearshape'), tr('app_btn_settings'), '#66000000', () => {
                this.showPanel = !this.showPanel;
              })
              this.CapsuleButton($r('sys.symbol.power'), tr('app_btn_disconnect'), '#CCE53935', () => {
                this.leavePlayer();
              })
            }
          }
          .width('100%')
          .alignItems(VerticalAlign.Top)
        }
        .width('100%')
        .height('100%')
        .padding(16)
        .hitTestBehavior(HitTestMode.Transparent)
      }

      // 锁形按钮：解锁状态随控件一起显隐；锁定后常驻（变淡），这是唯一可点的元素
      if (this.locked || this.controlsVisible) {
        Row() {
          this.LockButton()
        }
        .width('100%')
        .height('100%')
        .padding({ left: 24 })
        .alignItems(VerticalAlign.Center)
        .hitTestBehavior(HitTestMode.Transparent)
      }

      if (this.showPanel && this.controlsVisible && !this.locked) {
        this.QuickSettings()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    // 点画面空白处：解锁时显示/隐藏控件，锁定时只提示锁形图标。挂在根节点上，按钮自己的点击优先
    .onClick(() => {
      this.onPlayerTap();
    })
    .onAreaChange((_old: Area, area: Area) => {
      // 画面按 16:9 适配（RTSP 和我们的解码器共用）
      const w = Number(area.width);
      const h = Number(area.height);
      this.playerWidth = Math.min(w, h * VIDEO_ASPECT);
      this.playerHeight = this.playerWidth / VIDEO_ASPECT;
    })
    .focusable(true)
    .defaultFocus(true)
    .onKeyEvent((event: KeyEvent) => {
      // PC、平板外接键盘：Esc 相当于返回（锁定时不断开）
      if (event.type === KeyType.Down && event.keyCode === KeyCode.KEYCODE_ESCAPE) {
        this.onBackPress();
      }
    })
  }

  @Builder
  QuickRow(title: string, desc: string) {
    Column({ space: 2 }) {
      Text(title).fontSize(14).fontColor(Color.White)
      if (desc.length > 0) {
        Text(desc).fontSize(11).fontColor('#AAAAAA')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .layoutWeight(1)
  }

  // 快捷设置里的实验功能：音频编码、Opus 参数、自动轮换；改了立即通过控制消息生效
  @Builder
  ExtQuickSettings() {
    Divider().color('#33FFFFFF')
    Text(tr('ext_card_title')).fontSize(12).fontColor('#AAAAAA')

    Row() {
      // 说明文字随服务端支持情况、轮换状态变化：直接读 this.xxx（@Builder 按值传入的参数不会随状态刷新）
      Column({ space: 2 }) {
        Text(tr('ext_codec')).fontSize(14).fontColor(Color.White)
        Text(!this.extActive ? tr('ext_unsupported') :
          (this.rotating ? tr('ext_quick_rotating') : tr('ext_quick_codec_desc')))
          .fontSize(11)
          .fontColor('#AAAAAA')
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Select(selectOptions(codecLabels()))
        .selected(this.extCodec)
        .value(codecLabels()[this.extCodec])
        .font({ size: 13 })
        .fontColor(Color.White)
        .enabled(!this.rotating)
        .onSelect((index: number) => {
          this.extCodec = index;
          this.applyExtAudio();
        })
    }
    .width('100%')

    if (this.extCodec === ExtCodec.OPUS) {
      Row() {
        this.QuickRow(tr('ext_opus_kbps'), '')
        Select(selectOptions(OPUS_KBPS_OPTIONS.map((v: number) => tr('ext_kbps_value', v))))
          .selected(Math.max(0, OPUS_KBPS_OPTIONS.indexOf(this.extOpusKbps)))
          .value(tr('ext_kbps_value', this.extOpusKbps))
          .font({ size: 13 })
          .fontColor(Color.White)
          .enabled(!this.rotating)
          .onSelect((index: number) => {
            this.extOpusKbps = OPUS_KBPS_OPTIONS[index];
            this.applyExtAudio();
          })
      }
      .width('100%')

      Row() {
        this.QuickRow(tr('ext_opus_complexity'), '')
        Select(selectOptions(OPUS_COMPLEXITY_OPTIONS.map((v: number) => `${v}`)))
          .selected(this.extOpusComplexity)
          .value(`${this.extOpusComplexity}`)
          .font({ size: 13 })
          .fontColor(Color.White)
          .enabled(!this.rotating)
          .onSelect((index: number) => {
            this.extOpusComplexity = OPUS_COMPLEXITY_OPTIONS[index];
            this.applyExtAudio();
          })
      }
      .width('100%')

      Row() {
        this.QuickRow(tr('ext_opus_frame'), '')
        Select(selectOptions(OPUS_FRAME_OPTIONS.map((v: number) => tr('ext_ms_value', v))))
          .selected(Math.max(0, OPUS_FRAME_OPTIONS.indexOf(this.extOpusFrameMs)))
          .value(tr('ext_ms_value', this.extOpusFrameMs))
          .font({ size: 13 })
          .fontColor(Color.White)
          .enabled(!this.rotating)
          .onSelect((index: number) => {
            this.extOpusFrameMs = OPUS_FRAME_OPTIONS[index];
            this.applyExtAudio();
          })
      }
      .width('100%')
    }

    Row() {
      // 轮换进度每秒变化：同上，直接读 this.rotationText
      Column({ space: 2 }) {
        Text(tr('ext_rotate')).fontSize(14).fontColor(Color.White)
        Text(this.rotationText.length > 0 ? this.rotationText : tr('ext_quick_rotate_desc', this.rotateMinutes()))
          .fontSize(11)
          .fontColor('#AAAAAA')
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: this.rotateOn })
        .onChange((isOn: boolean) => {
          this.applyRotate(isOn);
        })
    }
    .width('100%')
  }

  @Builder
  QuickSettings() {
    Stack({ alignContent: Alignment.TopEnd }) {
      // 点面板外面关闭
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#33000000')
        .onClick(() => {
          this.showPanel = false;
        })

      Scroll() {
        Column({ space: 12 }) {
          Text(tr('app_quick_title'))
            .fontSize(15)
            .fontColor(Color.White)
            .fontWeight(FontWeight.Medium)

          Row() {
            this.QuickRow(tr('app_quick_audio'), this.playMode === StreamMode.TCP ? tr('app_quick_audio_desc_tcp') : '')
            Toggle({ type: ToggleType.Switch, isOn: this.enableAudio })
              .onChange((isOn: boolean) => {
                this.applyAudio(isOn);
              })
          }
          .width('100%')

          if (supportsSmooth(this.playMode)) {
            Row() {
              this.QuickRow(tr('app_quick_smooth'), tr('app_quick_smooth_desc'))
              Select(selectOptions(smoothLabels()))
                .selected(this.smoothLevel)
                .value(smoothLabels()[this.smoothLevel])
                .font({ size: 13 })
                .fontColor(Color.White)
                .onSelect((index: number) => {
                  this.applySmooth(index);
                })
            }
            .width('100%')
          }

          if (supportsFreeze(this.playMode)) {
            Row() {
              this.QuickRow(tr('app_quick_freeze'), tr('app_quick_freeze_desc'))
              Toggle({ type: ToggleType.Switch, isOn: this.freezeOnLoss })
                .onChange((isOn: boolean) => {
                  this.applyFreeze(isOn);
                })
            }
            .width('100%')
          }

          if (supportsEnhance(this.playMode)) {
            Row() {
              this.QuickRow(tr('app_quick_enhance'), tr('app_quick_enhance_desc'))
              Select(selectOptions(enhanceLabels()))
                .selected(this.enhanceLevel)
                .value(enhanceLabels()[this.enhanceLevel])
                .font({ size: 13 })
                .fontColor(Color.White)
                .onSelect((index: number) => {
                  this.applyEnhance(index);
                })
            }
            .width('100%')
          }

          if (supportsNetOpt(this.playMode)) {
            Row() {
              this.QuickRow(tr('app_quick_netopt'), tr('app_quick_netopt_desc'))
              Toggle({ type: ToggleType.Switch, isOn: this.netOpt })
                .onChange((isOn: boolean) => {
                  this.applyNetOpt(isOn);
                })
            }
            .width('100%')
          }

          Row() {
            this.QuickRow(tr('app_quick_stats'), '')
            Toggle({ type: ToggleType.Switch, isOn: this.showStats })
              .onChange((isOn: boolean) => {
                this.showStats = isOn;
              })
          }
          .width('100%')

          if (this.extShown()) {
            this.ExtQuickSettings()
          }

          if (supportsKeepAlive(this.playMode)) {
            Row() {
              this.QuickRow(tr('app_quick_keepalive'), '')
              Select(selectOptions(keepAliveLabels()))
                .selected(this.keepAliveIndex)
                .value(keepAliveLabels()[this.keepAliveIndex])
                .font({ size: 13 })
                .fontColor(Color.White)
                .onSelect((index: number) => {
                  this.applyKeepAlive(index);
                })
            }
            .width('100%')
          }

          if (this.pipSupported && this.playMode !== StreamMode.RTSP) {
            Row() {
              this.QuickRow(tr('app_quick_pip'), '')
              Toggle({ type: ToggleType.Switch, isOn: this.pipAuto })
                .onChange((isOn: boolean) => {
                  this.applyPipAuto(isOn);
                })
            }
            .width('100%')
          }
        }
        .padding(16)
      }
      .width(340)
      .constraintSize({ maxHeight: '80%' })
      .margin({ top: 60, right: 16 })
      .backgroundColor('#E6202124')
      .borderRadius(16)
      .onClick(() => {
        // 吞掉点击，避免冒泡到背景层把面板关掉
      })
    }
    .width('100%')
    .height('100%')
  }
}
