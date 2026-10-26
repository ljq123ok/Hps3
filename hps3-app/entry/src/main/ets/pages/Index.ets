import hilog from '@ohos.hilog';
import fs from '@ohos.file.fs';
import picker from '@ohos.file.picker';
import window from '@ohos.window';
import { VirtualGamePad } from '../components/VirtualGamePad';
import {
  FirmwareInstallResult,
  FirmwareInstallStatus,
  GameImageInspection,
  RunGameResult,
  RuntimeStatus,
  SurfaceStatus,
  VulkanPresentTestResult,
  VulkanSurfaceProbeResult,
  bootGameAsync,
  getFirmwareInstallStatus,
  getInstalledFirmwareVersion,
  getRuntimeStatus,
  getResolutionScale,
  getSurfaceStatus,
  getState,
  initialize,
  inspectGameImage,
  installFirmware,
  isCoreLinked,
  presentVulkanTestPattern,
  probeVulkanSurface,
  readFaultLog,
  runGame,
  setDecoders,
  setPadButton,
  setResolutionScale,
  setRenderer
} from 'libhps3core.so';

const DOMAIN = 0x3201;
const TAG = 'HPS3';

@Entry
@Component
struct Index {
  @State nativeCoreLoaded: boolean = false;
  @State initializationStatus: string = '尚未初始化';
  @State emulatorState: number = -1;
  @State firmwareVersion: string = '';
  @State firmwareStatus: string = '尚未安装 PS3 系统软件';
  @State firmwareProgress: string = '';
  @State installingFirmware: boolean = false;
  @State surfaceProbeOk: boolean = false;
  @State surfaceStatus: string = '等待 XComponent surface';
  @State surfaceSize: string = '';
  @State presentTestStatus: string = '尚未执行 swapchain 呈现测试';
  @State gameImageOk: boolean = false;
  @State gameImageStatus: string = '尚未选择 PS3 游戏 ISO';
  @State gameImageDetails: string = '';
  @State bootValidationOk: boolean = false;
  @State bootValidationBusy: boolean = false;
  @State bootValidationStatus: string = '尚未验证 EBOOT.BIN 启动链';
  @State runStatus: string = '尚未启动游戏执行';
  @State runOk: boolean = false;
  @State runBusy: boolean = false;
  @State runElapsed: string = '';
  @State runtimeDetail: string = '';
  @State useVulkan: boolean = true;
  @State resolutionScale: number = 100;
  @State resolutionScaleStatus: string = '100% 原始画质';
  @State fullScreenGame: boolean = false;
  @State private overlayExpanded: boolean = false;
  @State private virtualPadVisible: boolean = true;
  @State private bubbleX: number = -1;
  @State private bubbleY: number = -1;
  @State private fpsEstimate: number = 0;
  @State private gameplayWindowPrepared: boolean = false;
  @State viewportWidth: number = 0;
  @State viewportHeight: number = 0;
  @State faultInfo: string = '';
  @State interpreterMode: boolean = false;
  private selectedGamePath: string = '';
  private firmwareTimer: number = -1;
  private surfaceProbeTimer: number = -1;
  private surfaceProbeAttempts: number = 0;
  private runWatchTimer: number = -1;
  private runStartedAt: number = 0;
  private lastCiaSum: number = -1;
  private advancingSamples: number = 0;
  private previousFpsFrames: number = 0;
  private previousFpsAt: number = 0;
  private bubbleDragX: number = 0;
  private bubbleDragY: number = 0;

  aboutToAppear(): void {
    try {
      this.nativeCoreLoaded = isCoreLinked();
      hilog.info(DOMAIN, TAG, 'T2_CORE_LINKED result=%{public}s', `${this.nativeCoreLoaded}`);
      if (!this.nativeCoreLoaded) {
        this.initializationStatus = '核心库未加载，未执行初始化';
        return;
      }

      const rootDir: string = getContext(this).filesDir;
      hilog.info(DOMAIN, TAG, 'T2_EMU_INIT_BEGIN');
      const initialized: boolean = initialize(rootDir);
      this.emulatorState = getState();
      this.initializationStatus = initialized
        ? '核心初始化完成'
        : 'Emu.Init() 返回失败';
      hilog.info(DOMAIN, TAG, 'T2_EMU_INIT_DONE result=%{public}s state=%{public}d',
        `${initialized}`, this.emulatorState);

      if (initialized) {
        this.resolutionScale = getResolutionScale();
        this.resolutionScaleStatus = `${this.resolutionScale}% ${this.resolutionScale < 100 ? '性能对照' : '原始画质'}（重启游戏后生效）`;
        this.firmwareVersion = getInstalledFirmwareVersion();
        this.firmwareStatus = this.firmwareVersion.length > 0
          ? `已安装 PS3 系统软件 ${this.firmwareVersion}`
          : '尚未安装 PS3 系统软件';
      }
    } catch (_error) {
      this.initializationStatus = 'Emu.Init() 抛出异常';
      hilog.error(DOMAIN, TAG, 'T2_EMU_INIT_ERROR');
    }
  }

  aboutToDisappear(): void {
    setPadButton(-1, false);
    // Do not rotate the NativeWindow while a loaded renderer or running game
    // still owns its Vulkan swapchain. It is safe to restore the window before
    // the renderer has been created.
    if (getState() === 0) {
      this.setGameplayWindow(false);
    }
    this.stopFirmwareProgressTimer();
    this.stopSurfaceProbeTimer();
    this.stopRunWatch();
  }

  private stopFirmwareProgressTimer(): void {
    if (this.firmwareTimer >= 0) {
      clearInterval(this.firmwareTimer);
      this.firmwareTimer = -1;
    }
  }

  private refreshFirmwareProgress(): void {
    try {
      const status: FirmwareInstallStatus = getFirmwareInstallStatus();
      this.firmwareStatus = status.message;
      this.firmwareProgress = status.total > 0
        ? `${status.completed} / ${status.total} 个组件`
        : '';
    } catch (_error) {
      hilog.error(DOMAIN, TAG, 'T4_FIRMWARE_STATUS_ERROR');
    }
  }

  private stopSurfaceProbeTimer(): void {
    if (this.surfaceProbeTimer >= 0) {
      clearTimeout(this.surfaceProbeTimer);
      this.surfaceProbeTimer = -1;
    }
  }

  private scheduleSurfaceProbe(delayMs: number): void {
    this.stopSurfaceProbeTimer();
    this.surfaceProbeTimer = setTimeout(() => {
      this.surfaceProbeTimer = -1;
      this.runSurfaceProbe();
    }, delayMs);
  }

  private runSurfaceProbe(): void {
    try {
      const status: SurfaceStatus = getSurfaceStatus();
      this.surfaceSize = status.available ? `${status.width} × ${status.height}` : '';
      if (!status.available) {
        this.surfaceProbeAttempts += 1;
        this.surfaceStatus = `正在获取 NativeWindow…（${this.surfaceProbeAttempts}）`;
        if (this.surfaceProbeAttempts < 8) {
          this.scheduleSurfaceProbe(250);
        } else {
          this.surfaceStatus = '未获取到 XComponent NativeWindow';
          hilog.error(DOMAIN, TAG, 'T6_SURFACE_PROBE_NO_WINDOW');
        }
        return;
      }

      const probe: VulkanSurfaceProbeResult = probeVulkanSurface();
      this.surfaceProbeOk = probe.ok;
      this.surfaceStatus = probe.ok
        ? `${probe.message} · ${probe.deviceName} · Vulkan ${probe.apiVersion}`
        : `Surface 探针失败：${probe.message}`;
      hilog.info(DOMAIN, TAG,
        'T6_SURFACE_PROBE ok=%{public}s code=%{public}s size=%{public}dx%{public}d device=%{public}s formats=%{public}d queue=%{public}d',
        `${probe.ok}`, probe.code, status.width, status.height, probe.deviceName,
        probe.formatCount, probe.queueFamily);
      if (probe.ok) {
        const present: VulkanPresentTestResult = presentVulkanTestPattern();
        this.presentTestStatus = present.ok
          ? `${present.message} · ${present.width} × ${present.height} · ${present.imageCount} images`
          : `swapchain 呈现失败：${present.message}`;
        hilog.info(DOMAIN, TAG,
          'T6_SWAPCHAIN_PRESENT ok=%{public}s code=%{public}s size=%{public}dx%{public}d images=%{public}d format=%{public}d',
          `${present.ok}`, present.code, present.width, present.height, present.imageCount, present.format);
      }
    } catch (_error) {
      const error = _error as BusinessError;
      this.surfaceProbeOk = false;
      this.surfaceStatus = 'Surface 探针异常：' + error.message;
      hilog.error(DOMAIN, TAG, 'T6_SURFACE_PROBE_ERROR message=%{public}s', error.message);
    }
  }

  private async copyPupToSandbox(srcUri: string): Promise<string> {
    const filesDir: string = getContext(this).filesDir;
    const firmwareDir: string = filesDir + '/firmware';
    const destination: string = firmwareDir + '/PS3UPDAT.PUP';
    if (!fs.accessSync(firmwareDir)) {
      fs.mkdirSync(firmwareDir, true);
    }

    let sourceFile: fs.File | null = null;
    let destinationFile: fs.File | null = null;
    try {
      sourceFile = fs.openSync(srcUri, fs.OpenMode.READ_ONLY);
      destinationFile = fs.openSync(destination,
        fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      await fs.copyFile(sourceFile.fd, destinationFile.fd);
    } finally {
      if (sourceFile !== null) {
        fs.closeSync(sourceFile);
      }
      if (destinationFile !== null) {
        fs.closeSync(destinationFile);
      }
    }
    return destination;
  }

  private async pickAndInstallFirmware(): Promise<void> {
    if (!this.nativeCoreLoaded || this.installingFirmware) {
      return;
    }

    try {
      const options = new picker.DocumentSelectOptions();
      options.fileSuffixFilters = ['.PUP', '.pup'];
      const documentPicker = new picker.DocumentViewPicker(getContext(this));
      const uris: Array<string> = await documentPicker.select(options);
      if (!uris || uris.length === 0) {
        return;
      }

      this.installingFirmware = true;
      this.firmwareStatus = '正在把 PUP 异步复制到应用沙箱';
      this.firmwareProgress = '';
      hilog.info(DOMAIN, TAG, 'T4_FIRMWARE_COPY_BEGIN');
      const pupPath: string = await this.copyPupToSandbox(uris[0]);
      hilog.info(DOMAIN, TAG, 'T4_FIRMWARE_COPY_DONE');

      this.firmwareStatus = '正在校验并安装固件';
      this.firmwareTimer = setInterval(() => this.refreshFirmwareProgress(), 250);
      hilog.info(DOMAIN, TAG, 'T4_FIRMWARE_INSTALL_BEGIN');
      const result: FirmwareInstallResult = await installFirmware(pupPath);
      this.stopFirmwareProgressTimer();
      this.installingFirmware = false;
      this.firmwareVersion = result.installedVersion;
      this.firmwareStatus = result.ok
        ? `已安装 PS3 系统软件 ${result.installedVersion}`
        : (result.installedVersion.length > 0
          ? `安装失败：${result.message}；现有版本 ${result.installedVersion} 已保留`
          : `安装失败：${result.message}`);
      this.firmwareProgress = result.total > 0
        ? `${result.completed} / ${result.total} 个组件`
        : '';
      hilog.info(DOMAIN, TAG,
        'T4_FIRMWARE_INSTALL_DONE ok=%{public}s code=%{public}s version=%{public}s progress=%{public}d/%{public}d',
        `${result.ok}`, result.code, result.installedVersion, result.completed, result.total);
    } catch (_error) {
      const error = _error as BusinessError;
      this.stopFirmwareProgressTimer();
      this.installingFirmware = false;
      this.firmwareStatus = '固件选择、复制或安装异常：' + error.message;
      this.firmwareProgress = '';
      hilog.error(DOMAIN, TAG, 'T4_FIRMWARE_INSTALL_ERROR message=%{public}s', error.message);
    }
  }

  private formatFileSize(bytes: number): string {
    if (bytes >= 1024 * 1024 * 1024) {
      return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
    }
    if (bytes >= 1024 * 1024) {
      return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }
    return bytes + ' B';
  }

  private async pickAndInspectGame(): Promise<void> {
    let sourceFile: fs.File | null = null;
    try {
      const options = new picker.DocumentSelectOptions();
      options.fileSuffixFilters = ['.ISO', '.iso'];
      const documentPicker = new picker.DocumentViewPicker(getContext(this));
      const uris: Array<string> = await documentPicker.select(options);
      if (!uris || uris.length === 0) {
        return;
      }

      this.gameImageOk = false;
      this.gameImageStatus = '正在读取 ISO 关键扇区与 PS3 元数据…';
      this.gameImageDetails = '';
      sourceFile = fs.openSync(uris[0], fs.OpenMode.READ_ONLY);
      hilog.info(DOMAIN, TAG, 'T7_ISO_INSPECT_BEGIN name=%{public}s fd=%{public}d',
        sourceFile.name, sourceFile.fd);
      const inspection: GameImageInspection = inspectGameImage(sourceFile.fd);
      this.gameImageOk = inspection.ok;
      this.selectedGamePath = inspection.ok ? inspection.path : '';
      this.bootValidationOk = false;
      this.bootValidationStatus = '尚未验证 EBOOT.BIN 启动链';
      this.gameImageStatus = inspection.message;
      const identity: string = inspection.title.length > 0
        ? `${inspection.title}${inspection.titleId.length > 0 ? ' · ' + inspection.titleId : ''}`
        : sourceFile.name;
      this.gameImageDetails = `${identity} · ${this.formatFileSize(inspection.size)}`;
      hilog.info(DOMAIN, TAG,
        'T7_ISO_INSPECT_DONE ok=%{public}s code=%{public}s size=%{public}d archive=%{public}s sfo=%{public}s eboot=%{public}s titleId=%{public}s',
        `${inspection.ok}`, inspection.code, inspection.size, `${inspection.archiveValid}`,
        `${inspection.hasParamSfo}`, `${inspection.hasEboot}`, inspection.titleId);
    } catch (_error) {
      const error = _error as BusinessError;
      this.gameImageOk = false;
      this.gameImageStatus = 'ISO 选择或检查异常：' + error.message;
      this.gameImageDetails = '';
      hilog.error(DOMAIN, TAG, 'T7_ISO_INSPECT_ERROR message=%{public}s', error.message);
    } finally {
      if (sourceFile !== null) {
        fs.closeSync(sourceFile);
      }
    }
  }

  private async runBootValidation(): Promise<void> {
    if (!this.gameImageOk || this.selectedGamePath.length === 0 || this.bootValidationBusy) {
      return;
    }

    this.bootValidationBusy = true;
    this.bootValidationOk = false;
    this.gameplayWindowPrepared = false;
    this.bootValidationStatus = '正在切换横屏并等待 Vulkan Surface 稳定…';
    hilog.info(DOMAIN, TAG, 'T8_LANDSCAPE_PREPARE_BEGIN');
    try {
      await this.setGameplayWindow(true);
      this.gameplayWindowPrepared = await this.waitForLandscapeSurface();
      if (!this.gameplayWindowPrepared) {
        this.bootValidationStatus = '横屏 Surface 未稳定，已取消启动；请重试';
        hilog.error(DOMAIN, TAG, 'T8_LANDSCAPE_PREPARE_TIMEOUT');
        await this.setGameplayWindow(false);
        return;
      }
      hilog.info(DOMAIN, TAG, 'T8_LANDSCAPE_PREPARE_DONE size=%{public}dx%{public}d',
        this.viewportWidth, this.viewportHeight);
      this.bootValidationStatus = this.useVulkan
        ? '正在以 Vulkan 渲染 / Null 音频与输入装载 EBOOT.BIN…'
        : '正在以 Null 渲染/音频/输入装载 EBOOT.BIN…';
      hilog.info(DOMAIN, TAG, 'T8_BOOT_BEGIN');
      // Persist the backend choice before booting: Load() reloads config.yml and
      // then runs fixup_settings(), so selecting it here is what makes
      // init_gs_render() build VKGSRender instead of NullGSRender.
      const decodersSet: boolean = setDecoders(this.interpreterMode ? 0 : 1, this.interpreterMode ? 0 : 1);
      hilog.info(DOMAIN, TAG, 'T9_SET_DECODERS interpreter=%{public}s applied=%{public}s',
        `${this.interpreterMode}`, `${decodersSet}`);

      const rendererSet: boolean = setRenderer(this.useVulkan ? 1 : 0);
      hilog.info(DOMAIN, TAG, 'T9_SET_RENDERER vulkan=%{public}s applied=%{public}s',
        `${this.useVulkan}`, `${rendererSet}`);

      const result: number = await bootGameAsync(this.selectedGamePath);
      const status: RuntimeStatus = getRuntimeStatus();
      this.emulatorState = getState();
      this.bootValidationOk = result === 0 && this.emulatorState === 6;
      this.bootValidationStatus = this.bootValidationOk
        ? `BootGame 装载成功，核心已停在 Ready（状态 ${this.emulatorState}）`
        : `BootGame 返回 ${result}，核心状态 ${this.emulatorState}`;
      this.runtimeDetail = this.describeRuntime(status);
      hilog.info(DOMAIN, TAG,
        'T8_BOOT_DONE ok=%{public}s result=%{public}d state=%{public}d renderer=%{public}d ppu=%{public}d spu=%{public}d',
        `${this.bootValidationOk}`, result, this.emulatorState, status.renderer,
        status.ppuThreads, status.spuThreads);
    } catch (_error) {
      const error = _error as BusinessError;
      this.bootValidationStatus = 'BootGame 调用异常：' + error.message;
      hilog.error(DOMAIN, TAG, 'T8_BOOT_ERROR message=%{public}s', error.message);
    } finally {
      this.bootValidationBusy = false;
    }
  }

  private async waitForLandscapeSurface(): Promise<boolean> {
    let previousKey: string = '';
    let stableSamples: number = 0;
    for (let attempt = 0; attempt < 50; attempt++) {
      const surface: SurfaceStatus = getSurfaceStatus();
      const isLandscape: boolean = surface.available && surface.width > surface.height &&
        this.viewportWidth > this.viewportHeight;
      if (isLandscape) {
        const key: string = `${surface.epoch}:${surface.width}x${surface.height}:` +
          `${this.viewportWidth}x${this.viewportHeight}`;
        stableSamples = key === previousKey ? stableSamples + 1 : 1;
        previousKey = key;
        if (stableSamples >= 3) {
          await new Promise<void>((resolve: () => void) => { setTimeout(resolve, 200); });
          const confirmed: SurfaceStatus = getSurfaceStatus();
          if (confirmed.available && confirmed.width > confirmed.height &&
            confirmed.epoch === surface.epoch && confirmed.width === surface.width &&
            confirmed.height === surface.height && this.viewportWidth > this.viewportHeight) {
            this.surfaceSize = `${confirmed.width} × ${confirmed.height}`;
            return true;
          }
          stableSamples = 0;
          previousKey = '';
        }
      } else {
        stableSamples = 0;
        previousKey = '';
      }
      await new Promise<void>((resolve: () => void) => { setTimeout(resolve, 100); });
    }
    return false;
  }

  private describeRuntime(status: RuntimeStatus): string {
    const rendererName: string = status.renderer === 2
      ? 'Vulkan (VKGSRender)'
      : (status.renderer === 1 ? 'Null (NullGSRender)' : '尚未创建');
    return `渲染后端 ${rendererName} · 帧 ${status.frames} · PPU 线程 ${status.ppuThreads} · SPU 线程 ${status.spuThreads}`;
  }

  private chooseResolutionScale(percent: number): void {
    const applied: boolean = setResolutionScale(percent);
    if (!applied) {
      this.resolutionScaleStatus = '游戏运行中无法切换；请停止游戏后再选';
      return;
    }
    this.resolutionScale = percent;
    this.resolutionScaleStatus = `${percent}% ${percent < 100 ? '性能对照' : '原始画质'}（下次启动游戏生效）`;
    hilog.info(DOMAIN, TAG, 'T9_RESOLUTION_SCALE percent=%{public}d', percent);
  }

  private async setGameplayWindow(enable: boolean): Promise<void> {
    try {
      const win = await window.getLastWindow(getContext(this));
      await win.setPreferredOrientation(enable ? window.Orientation.LANDSCAPE : window.Orientation.AUTO_ROTATION);
      await win.setWindowKeepScreenOn(enable);
      await win.setWindowLayoutFullScreen(enable);
      await win.setWindowSystemBarEnable(enable ? [] : ['status', 'navigation']);
    } catch (_error) {
      const error = _error as BusinessError;
      hilog.error(DOMAIN, TAG, 'T9_GAME_WINDOW_ERROR enabled=%{public}d message=%{public}s', enable ? 1 : 0, error.message);
    }
  }

  private enterGameplay(): void {
    this.fullScreenGame = true;
    this.overlayExpanded = false;
    this.virtualPadVisible = true;
    this.previousFpsFrames = getRuntimeStatus().frames;
    this.previousFpsAt = Date.now();
    this.fpsEstimate = 0;
    if (this.bubbleX < 0 || this.bubbleY < 0) {
      // Place the handle on the left edge, clear of the right shoulder buttons.
      this.bubbleX = 14;
      this.bubbleY = Math.max(90, this.viewportHeight * 0.42);
    }
  }

  private leaveGameplay(): void {
    this.fullScreenGame = false;
    this.overlayExpanded = false;
    setPadButton(-1, false);
    // Keep the display orientation while the loaded Vulkan renderer is alive.
    // Reorienting this same surface mid-run can make Mali lose its device.
  }

  private updateOverlayFps(status: RuntimeStatus): void {
    const now: number = Date.now();
    const elapsed: number = now - this.previousFpsAt;
    if (elapsed >= 800) {
      this.fpsEstimate = Math.max(0, Math.round((status.frames - this.previousFpsFrames) * 1000 / elapsed));
      this.previousFpsFrames = status.frames;
      this.previousFpsAt = now;
    }
  }

  private clampBubblePosition(x: number, y: number): void {
    const maxX: number = Math.max(8, this.viewportWidth - 64);
    const maxY: number = Math.max(8, this.viewportHeight - 64);
    this.bubbleX = Math.max(8, Math.min(maxX, x));
    this.bubbleY = Math.max(8, Math.min(maxY, y));
  }

  private stopRunWatch(): void {
    if (this.runWatchTimer >= 0) {
      clearInterval(this.runWatchTimer);
      this.runWatchTimer = -1;
    }
  }

  // Polls independent counters rather than trusting a single getState() sample:
  // "running" alone would also be reported if the guest threads were parked.
  private watchRun(): void {
    this.stopRunWatch();
    if (this.runStartedAt === 0) {
      this.runStartedAt = Date.now();
    }
    this.lastCiaSum = -1;
    this.advancingSamples = 0;

    this.runWatchTimer = setInterval(() => {
      try {
        const status: RuntimeStatus = getRuntimeStatus();
        const elapsedSec: number = Math.floor((Date.now() - this.runStartedAt) / 1000);
        this.runElapsed = `${elapsedSec}s`;
        this.emulatorState = status.state;
        this.runtimeDetail = this.describeRuntime(status);
        this.updateOverlayFps(status);

        // A changing summed CIA is direct evidence PPU code advanced between
        // samples; a frozen sum means the threads exist but are not executing.
        if (this.lastCiaSum >= 0 && status.ppuCiaSum !== this.lastCiaSum) {
          this.advancingSamples += 1;
        }
        this.lastCiaSum = status.ppuCiaSum;

        if (status.fault.executeCount > 0 || status.ppuErroredThreads > 0) {
          this.runOk = false;
          this.leaveGameplay();
          this.runStatus = `游戏执行异常 · PPU 宿主状态 ${status.ppuHostState} · 取指故障 ${status.fault.executeCount}`;
          this.faultInfo = readFaultLog().trim().slice(-1600);
        } else if (status.state === 3) {
          this.runOk = true;
          this.runStatus = `游戏执行中 Running（状态 3）· 已运行 ${this.runElapsed} · 指令推进采样 ${this.advancingSamples}`;
        } else if (status.starting) {
          this.runStatus = `Starting（状态 7），等待 guest 线程就绪 · ${this.runElapsed}`;
        } else if (status.state === 0 || status.state === 2) {
          this.runStatus = `核心已停止（状态 ${status.state}）· 运行 ${this.runElapsed}`;
          this.runOk = false;
          this.leaveGameplay();
          this.stopRunWatch();
        } else {
          this.runStatus = `运行中，核心状态 ${status.state} · ${this.runElapsed}`;
        }

        // Surface any SIGSEGV the native probe captured. It is persisted to a
        // file because hilog masks pointers and the process dies moments later.
        if (status.fault.executeCount > 0 || status.ppuErroredThreads > 0) {
          const fault: string = readFaultLog();
          if (fault.length > 0) {
            this.faultInfo = fault.trim().slice(-1600);
          }
        }

        hilog.info(DOMAIN, TAG,
          'T9_RUN_TICK state=%{public}d frames=%{public}d ppu=%{public}d spu=%{public}d ciaSum=%{public}s advancing=%{public}d',
          status.state, status.frames, status.ppuThreads, status.spuThreads,
          `${status.ppuCiaSum}`, this.advancingSamples);
        hilog.info(DOMAIN, TAG, 'T9_PPU_FLAGS flags=0x%{public}s cia=0x%{public}s progTotal=%{public}d progDone=%{public}d',
          status.ppuFlags.toString(16), status.ppuCiaSum.toString(16),
          status.progTotal, status.progDone);
        hilog.info(DOMAIN, TAG, 'T9_PPU_POS lr=0x%{public}s sp=0x%{public}s',
          status.ppuLr.toString(16), status.ppuSp.toString(16));
        hilog.info(DOMAIN, TAG, 'T9_PPU_HOST state=%{public}d errored=%{public}d faults=%{public}d',
          status.ppuHostState, status.ppuErroredThreads, status.fault.executeCount);
        if (elapsedSec % 5 === 0) {
          hilog.info(DOMAIN, TAG, 'T9_PPU_DETAIL %{public}s', status.ppuDetail);
          hilog.info(DOMAIN, TAG, 'T9_SPU_DETAIL %{public}s', status.spuDetail);
        }

        // Keep monitoring: startup progress alone does not prove continued execution.
      } catch (_error) {
        const error = _error as BusinessError;
        hilog.error(DOMAIN, TAG, 'T9_RUN_TICK_ERROR message=%{public}s', error.message);
        this.stopRunWatch();
      }
    }, 1000);
  }

  private startGameRun(): void {
    if (this.runBusy || !this.bootValidationOk) {
      return;
    }
    if (!this.gameplayWindowPrepared) {
      this.runStatus = '请先完成横屏 Surface 准备与 EBOOT 验证';
      return;
    }

    this.runBusy = true;
    this.runOk = false;
    this.runStartedAt = 0;
    this.runStatus = '正在从 Ready 切到 guest 执行…';
    hilog.info(DOMAIN, TAG, 'T9_RUN_BEGIN');
    try {
      const result: RunGameResult = runGame();
      this.emulatorState = result.state;
      this.runStatus = result.message;
      hilog.info(DOMAIN, TAG, 'T9_RUN_ISSUED ok=%{public}s code=%{public}s state=%{public}d',
        `${result.ok}`, result.code, result.state);
      if (result.ok) {
        this.runStartedAt = Date.now();
        this.enterGameplay();
        this.watchRun();
      }
    } catch (_error) {
      const error = _error as BusinessError;
      this.runStatus = '启动游戏执行异常：' + error.message;
      hilog.error(DOMAIN, TAG, 'T9_RUN_ERROR message=%{public}s', error.message);
    } finally {
      this.runBusy = false;
    }
  }

  @Builder
  padKey(label: string, index: number) {
    Button(label)
      .layoutWeight(1)
      .height(44)
      .fontSize(label.length > 3 ? 9 : 14)
      .padding({ left: 0, right: 0 })
      .backgroundColor('#315172')
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          setPadButton(index, true);
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          setPadButton(index, false);
        }
      })
  }

  build() {
    Stack() {
      XComponent({
        id: 'hps3_surface',
        type: XComponentType.SURFACE,
        libraryname: 'hps3core'
      })
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')
        .onLoad(() => {
          this.surfaceProbeAttempts = 0;
          this.surfaceStatus = '正在获取 NativeWindow…';
          hilog.info(DOMAIN, TAG, 'T6_XCOMPONENT_LOADED');
          this.scheduleSurfaceProbe(250);
        })
        .onDestroy(() => {
          this.stopSurfaceProbeTimer();
          hilog.info(DOMAIN, TAG, 'T6_XCOMPONENT_DESTROYED');
        });

      if (this.fullScreenGame) {
        if (this.virtualPadVisible) {
          VirtualGamePad({ areaWidth: this.viewportWidth, areaHeight: this.viewportHeight })
            .width('100%')
            .height('100%');
        }

        // Draggable in-game control bubble, kept separate from the R2 corner.
        Stack() {
          if (this.overlayExpanded) {
            Column({ space: 7 }) {
              Text(`${this.fpsEstimate} FPS`)
                .fontSize(12).fontColor('#9BE7C0')
                .width(106).height(30).textAlign(TextAlign.Center)
                .backgroundColor('#E6101A2C').borderRadius(15)
              Button(this.virtualPadVisible ? '隐藏虚拟按键' : '显示虚拟按键')
                .width(132).height(36).fontSize(12).fontColor('#FFFFFF')
                .backgroundColor('#E6101A2C').borderRadius(18)
                .onClick(() => {
                  this.virtualPadVisible = !this.virtualPadVisible;
                  if (!this.virtualPadVisible) setPadButton(-1, false);
                  this.overlayExpanded = false;
                })
              Button('返回首页')
                .width(132).height(36).fontSize(12).fontColor('#FFFFFF')
                .backgroundColor('#E6101A2C').borderRadius(18)
                .onClick(() => { this.leaveGameplay(); })
              Text('暂停 / 即时存档尚未接入核心')
                .fontSize(10).fontColor('#B8C9DE')
                .padding({ left: 8, right: 8, top: 5, bottom: 5 })
                .backgroundColor('#E6101A2C').borderRadius(12)
              Button('收起')
                .width(88).height(30).fontSize(11).fontColor('#CFD9E8')
                .backgroundColor('#CC101A2C').borderRadius(15)
                .onClick(() => { this.overlayExpanded = false; })
            }
            .padding(8)
            .backgroundColor('#B8101A2C')
            .border({ width: 1, color: '#55FFFFFF' })
            .borderRadius(20)
            .position({ x: Math.max(8, Math.min(this.bubbleX, this.viewportWidth - 150)), y: Math.max(8, Math.min(this.bubbleY, this.viewportHeight - 205)) })
          } else {
            Column({ space: 1 }) {
              Text(`${this.fpsEstimate}`)
                .fontSize(14).fontWeight(FontWeight.Medium)
                .fontColor('#9BE7C0')
              Text('FPS').fontSize(8).fontColor('#CFD9E8')
            }
            .width(56).height(56).justifyContent(FlexAlign.Center)
            .backgroundColor('#E6101A2C').borderRadius(28)
            .border({ width: 1, color: '#66FFFFFF' })
            .position({ x: this.bubbleX, y: this.bubbleY })
            .gesture(
              PanGesture({ fingers: 1, distance: 5 })
                .onActionStart(() => {
                  this.bubbleDragX = this.bubbleX;
                  this.bubbleDragY = this.bubbleY;
                })
                .onActionUpdate((event: GestureEvent) => {
                  this.clampBubblePosition(this.bubbleDragX + event.offsetX, this.bubbleDragY + event.offsetY);
                })
                .onActionEnd(() => {
                  this.clampBubblePosition(this.bubbleX, this.bubbleY);
                })
            )
            .onClick(() => { this.overlayExpanded = true; })
          }
        }
        .width('100%').height('100%')
        .hitTestBehavior(HitTestMode.Transparent)
      } else {
      Scroll() {
      Column({ space: 18 }) {
        Text('hps3')
          .fontSize(30)
          .fontWeight(FontWeight.Bold)
          .fontColor('#EAF5FF');
        Text('HarmonyOS 原生 PS3 模拟器')
          .fontSize(16)
          .fontColor('#AFCBEA');

        Column({ space: 10 }) {
          Text('PS3 系统软件')
            .fontSize(13)
            .fontColor('#9CB6D7');
          Text(this.firmwareStatus)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.firmwareVersion.length > 0 ? '#80E7B0' : '#FFFFFF');
          if (this.firmwareProgress.length > 0) {
            Text(this.firmwareProgress)
              .fontSize(13)
              .fontColor('#B8C9DE');
          }
          Button(this.installingFirmware ? '正在处理…' : '选择并安装 PS3 固件 PUP')
            .width('100%')
            .height(44)
            .enabled(this.nativeCoreLoaded && !this.installingFirmware)
            .backgroundColor('#2878D4')
            .fontColor('#FFFFFF')
            .onClick(() => { this.pickAndInstallFirmware(); });
          Text('请仅选择你从 Sony 官方渠道取得的 PS3UPDAT.PUP。文件会先复制到应用沙箱，完整校验成功后才替换现有固件。')
            .fontSize(12)
            .fontColor('#91A8C5');
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
        .padding(18)
        .backgroundColor('#10233F')
        .borderRadius(14);

        Column({ space: 10 }) {
          Text('PS3 游戏镜像')
            .fontSize(13)
            .fontColor('#9CB6D7');
          Text(this.gameImageStatus)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.gameImageOk ? '#80E7B0' : '#FFFFFF');
          if (this.gameImageDetails.length > 0) {
            Text(this.gameImageDetails)
              .fontSize(13)
              .fontColor('#B8C9DE');
          }
          Button('选择并检查 PS3 ISO')
            .width('100%')
            .height(44)
            .enabled(this.nativeCoreLoaded)
            .backgroundColor('#2878D4')
            .fontColor('#FFFFFF')
            .onClick(() => { this.pickAndInspectGame(); });
          Button(this.bootValidationBusy ? '正在装载…' : '验证 EBOOT.BIN 启动链')
            .width('100%')
            .height(44)
            .enabled(this.gameImageOk && !this.bootValidationBusy)
            .backgroundColor(this.gameImageOk ? '#1F9D68' : '#315172')
            .fontColor('#FFFFFF')
            .onClick(() => { this.runBootValidation(); });
          Text(this.bootValidationStatus)
            .fontSize(13)
            .fontColor(this.bootValidationOk ? '#80E7B0' : '#FFD166');
          Row({ space: 10 }) {
            Text('使用 Vulkan 渲染（关闭则用 Null，仅验证 guest 执行）')
              .fontSize(13)
              .fontColor('#B8C9DE')
              .layoutWeight(1);
            Toggle({ type: ToggleType.Switch, isOn: this.useVulkan })
              .enabled(!this.bootValidationBusy && !this.runBusy)
              .selectedColor('#1F9D68')
              .onChange((isOn: boolean) => { this.useVulkan = isOn; });
          }
          .width('100%');
          Row({ space: 10 }) {
            Text('内部渲染比例')
              .fontSize(13)
              .fontColor('#B8C9DE')
              .layoutWeight(1);
            Button('100%')
              .height(36)
              .backgroundColor(this.resolutionScale === 100 ? '#1F9D68' : '#315172')
              .fontColor('#FFFFFF')
              .enabled(!this.bootValidationBusy && !this.runBusy)
              .onClick(() => { this.chooseResolutionScale(100); });
            Button('75%')
              .height(36)
              .backgroundColor(this.resolutionScale === 75 ? '#1F9D68' : '#315172')
              .fontColor('#FFFFFF')
              .enabled(!this.bootValidationBusy && !this.runBusy)
              .onClick(() => { this.chooseResolutionScale(75); });
            Button('50%')
              .height(36)
              .backgroundColor(this.resolutionScale === 50 ? '#1F9D68' : '#315172')
              .fontColor('#FFFFFF')
              .enabled(!this.bootValidationBusy && !this.runBusy)
              .onClick(() => { this.chooseResolutionScale(50); });
          }
          .width('100%');
          Text(this.resolutionScaleStatus)
            .fontSize(12)
            .fontColor('#91A8C5');
          Row({ space: 10 }) {
            Text('PPU/SPU 解释器模式（绕过 JIT，用于定位）')
              .fontSize(13)
              .fontColor('#B8C9DE')
              .layoutWeight(1);
            Toggle({ type: ToggleType.Switch, isOn: this.interpreterMode })
              .enabled(!this.bootValidationBusy && !this.runBusy)
              .selectedColor('#C2410C')
              .onChange((isOn: boolean) => { this.interpreterMode = isOn; });
          }
          .width('100%');

          Button(this.runBusy ? '正在启动…' : '开始运行游戏')
            .width('100%')
            .height(48)
            .enabled(this.bootValidationOk && !this.runBusy)
            .backgroundColor(this.bootValidationOk ? '#C2410C' : '#315172')
            .fontColor('#FFFFFF')
            .onClick(() => { this.startGameRun(); });
          Text(this.runStatus)
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.runOk ? '#80E7B0' : '#FFD166');
          if (this.faultInfo.length > 0) {
            Text(this.faultInfo)
              .fontSize(12)
              .fontColor('#FF9E9E');
          }
          if (this.runtimeDetail.length > 0) {
            Text(this.runtimeDetail)
              .fontSize(13)
              .fontColor('#B8C9DE');
          }
          Text('检查过程不会复制或修改整份 ISO。识别成功需同时找到 PS3_GAME/PARAM.SFO 与 PS3_GAME/USRDIR/EBOOT.BIN；加密 Redump 镜像还需要匹配的 .dkey 或 .key。')
            .fontSize(12)
            .fontColor('#91A8C5');
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
        .padding(18)
        .backgroundColor('#10233F')
        .borderRadius(14);

        Column({ space: 10 }) {
          Text('Vulkan 输出 Surface')
            .fontSize(13)
            .fontColor('#9CB6D7');
          Text(this.surfaceStatus)
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.surfaceProbeOk ? '#80E7B0' : '#FFFFFF');
          Row({ space: 6 }) {
            this.padKey('↑', 0)
            this.padKey('↓', 1)
            this.padKey('←', 2)
            this.padKey('→', 3)
            this.padKey('START', 14)
            this.padKey('SELECT', 15)
          }
          Row({ space: 6 }) {
            this.padKey('□', 5)
            this.padKey('△', 7)
            this.padKey('○', 6)
            this.padKey('×', 4)
            this.padKey('L1', 8)
            this.padKey('R1', 11)
          }
          Row({ space: 6 }) {
            this.padKey('移动↑', 16)
            this.padKey('移动↓', 17)
            this.padKey('移动←', 18)
            this.padKey('移动→', 19)
          }
          Row({ space: 6 }) {
            this.padKey('L2', 9)
            this.padKey('R2', 12)
            this.padKey('L3', 10)
            this.padKey('R3', 13)
          }
          if (this.surfaceSize.length > 0) {
            Text(`NativeWindow ${this.surfaceSize}`)
              .fontSize(13)
              .fontColor('#B8C9DE');
          }
          if (!this.runOk) {
            Text(this.presentTestStatus)
              .fontSize(13)
              .fontColor('#FFD166');
          }
          Text(this.runOk ? '运行游戏后会自动进入横屏全屏，显示虚拟摇杆与按键。' : '游戏画面使用全屏 Vulkan Surface 呈现。')
            .fontSize(12)
            .fontColor('#91A8C5');
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
        .padding(18)
        .backgroundColor('#10233F')
        .borderRadius(14);

        Column({ space: 8 }) {
          Text('模拟器核心')
            .fontSize(13)
            .fontColor('#9CB6D7');
          Text(this.nativeCoreLoaded ? 'RPCS3 ARM64 核心库已加载' : 'RPCS3 ARM64 核心库未加载')
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor('#FFFFFF');
          Text(this.runStartedAt > 0 ? this.runStatus : this.initializationStatus)
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor('#D6E8FF');
          Text('固件和存档保存在应用数据中。')
            .fontSize(13)
            .fontColor('#B8C9DE');
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
        .padding(18)
        .backgroundColor('#10233F')
        .borderRadius(14);

        Text('按住按键持续输入，松开后停止输入。')
          .fontSize(12)
          .fontColor('#7892B4');
      }
      .width('100%')
      .padding(24);
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#071225');
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .onAreaChange((_, area) => {
      const width = Number(area.width);
      const height = Number(area.height);
      if (width > 0 && height > 0 && (width !== this.viewportWidth || height !== this.viewportHeight)) {
        this.viewportWidth = width;
        this.viewportHeight = height;
        hilog.info(DOMAIN, TAG, 'T9_VIEWPORT size=%{public}dx%{public}d', width, height);
      }
    });
  }
}
