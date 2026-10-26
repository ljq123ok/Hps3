declare module 'libhps3core.so' {
  export interface FirmwareInstallStatus {
    active: boolean;
    completed: number;
    total: number;
    phase: string;
    message: string;
    version: string;
  }

  export interface FirmwareInstallResult {
    ok: boolean;
    code: string;
    message: string;
    version: string;
    installedVersion: string;
    completed: number;
    total: number;
  }

  export interface SurfaceStatus {
    available: boolean;
    width: number;
    height: number;
    epoch: number;
  }

  export interface VulkanSurfaceProbeResult {
    ok: boolean;
    code: string;
    message: string;
    deviceName: string;
    apiVersion: string;
    queueFamily: number;
    formatCount: number;
  }

  export interface VulkanPresentTestResult {
    ok: boolean;
    code: string;
    message: string;
    width: number;
    height: number;
    imageCount: number;
    format: number;
  }

  export interface GameImageInspection {
    ok: boolean;
    code: string;
    message: string;
    size: number;
    archiveValid: boolean;
    hasParamSfo: boolean;
    hasEboot: boolean;
    title: string;
    titleId: string;
    category: string;
    path: string;
  }

  export interface RunGameResult {
    ok: boolean;
    code: string;
    message: string;
    state: number;
  }

  export interface RuntimeStatus {
    state: number;
    running: boolean;
    ready: boolean;
    starting: boolean;
    paused: boolean;
    frames: number;
    renderer: number;
    runCalls: number;
    readyCalls: number;
    stopCalls: number;
    ppuThreads: number;
    spuThreads: number;
    ppuCiaSum: number;
    spuPcSum: number;
    ppuFlags: number;
    ppuHostState: number;
    ppuErroredThreads: number;
    ppuDetail: string;
    spuDetail: string;
    fault: { count: number; executeCount: number; addr: number; pc: number; lr: number };
    progTotal: number;
    progDone: number;
    ppuLr: number;
    ppuSp: number;
  }

  export function isCoreLinked(): boolean;
  export function getState(): number;
  export function initialize(rootDir: string): boolean;
  export function getInstalledFirmwareVersion(): string;
  export function getFirmwareInstallStatus(): FirmwareInstallStatus;
  export function installFirmware(path: string): Promise<FirmwareInstallResult>;
  export function getSurfaceStatus(): SurfaceStatus;
  export function probeVulkanSurface(): VulkanSurfaceProbeResult;
  export function presentVulkanTestPattern(): VulkanPresentTestResult;
  export function inspectGameImage(fd: number): GameImageInspection;
  export function bootGame(path: string): number;
  export function bootGameAsync(path: string): Promise<number>;
  export function runGame(): RunGameResult;
  export function getRuntimeStatus(): RuntimeStatus;
  export function setRenderer(mode: number): boolean;
  export function getResolutionScale(): number;
  export function setResolutionScale(percent: number): boolean;
  export function readFaultLog(): string;
  export function setDecoders(ppuMode: number, spuMode: number): boolean;
  export function setPadButton(index: number, pressed: boolean): boolean;
  /** Set analog stick 0 (left) or 1 (right) using PS3 byte range 0..255 (center=128). */
  export function setPadStick(stick: number, x: number, y: number): boolean;
}

export {};
