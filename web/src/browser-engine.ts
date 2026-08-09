import {
  BROWSER_ENGINE_VERSION,
  browserEngineProfile,
  normalizeBrowserEngineProfile,
  type BrowserEngineProfile,
} from "./engine-profile";

const ENGINE_SCRIPT_PATH = "/engine/stockfish-18-lite-single.js";
const ENGINE_WASM_PATH = "/engine/stockfish-18-lite-single.wasm";
const MAX_UCI_LINE_LENGTH = 8_192;
const MAX_PV_LENGTH = 32;
const MAX_FEN_LENGTH = 128;
const MAX_PLY = 20_000;
const DEFAULT_HANDSHAKE_TIMEOUT_MS = 8_000;
const DEFAULT_CANCEL_TIMEOUT_MS = 1_500;

export type BrowserEngineState = "idle" | "starting" | "ready" | "analyzing" | "cancelling" | "failed" | "disposed";
export type BrowserEngineErrorCode = "unavailable" | "invalid_request" | "busy" | "timeout" | "cancelled" | "failed" | "disposed";

export class BrowserEngineError extends Error {
  constructor(readonly code: BrowserEngineErrorCode, message: string) {
    super(message);
    this.name = "BrowserEngineError";
  }
}

export interface BrowserEngineRequest {
  readonly fen: string;
  readonly ply?: number;
  readonly profile?: BrowserEngineProfile;
}

export interface BrowserEngineScore {
  readonly type: "cp" | "mate";
  readonly value: number;
}

export interface BrowserEngineObservation {
  readonly bestMove: string;
  readonly ponderMove?: string;
  readonly score: BrowserEngineScore | null;
  readonly depth: number;
  readonly nodes: number;
  readonly timeMs: number;
  readonly principalVariation: readonly string[];
  readonly profile: BrowserEngineProfile;
  readonly engineVersion: typeof BROWSER_ENGINE_VERSION;
  readonly source: "browser";
}

export interface BrowserEngineProgress {
  readonly profile: BrowserEngineProfile;
  readonly depth: number;
  readonly targetDepth: number;
  readonly nodes: number;
  readonly timeMs: number;
  readonly percent: number;
}

interface WorkerMessageEvent {
  readonly data: unknown;
}

interface WorkerErrorEvent {
  readonly message?: string;
  readonly error?: unknown;
}

interface EngineWorker {
  onmessage: ((event: WorkerMessageEvent) => void) | null;
  onerror: ((event: WorkerErrorEvent) => void) | null;
  onmessageerror: (() => void) | null;
  postMessage(message: string): void;
  terminate(): void;
}

type WorkerFactory = () => EngineWorker;
type LinePredicate = (line: string) => boolean;

interface LineWaiter {
  readonly predicate: LinePredicate;
  readonly resolve: () => void;
  readonly reject: (error: BrowserEngineError) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

interface ParsedInfo {
  depth?: number;
  nodes?: number;
  timeMs?: number;
  score?: BrowserEngineScore;
  principalVariation?: string[];
}

interface ActiveAnalysis {
  readonly profile: BrowserEngineProfile;
  readonly request: BrowserEngineRequest;
  readonly resolve: (observation: BrowserEngineObservation) => void;
  readonly reject: (error: BrowserEngineError) => void;
  readonly progress?: (progress: BrowserEngineProgress) => void;
  readonly latest: {
    depth: number;
    nodes: number;
    timeMs: number;
    score: BrowserEngineScore | null;
    principalVariation: string[];
  };
  timer: ReturnType<typeof setTimeout> | null;
  cancelTimer: ReturnType<typeof setTimeout> | null;
  cancelPromise: Promise<void> | null;
  cancelResolve: (() => void) | null;
  cancelRequested: boolean;
  timeoutRequested: boolean;
}

export interface BrowserEngineOptions {
  readonly workerFactory?: WorkerFactory;
  readonly handshakeTimeoutMs?: number;
  readonly cancelTimeoutMs?: number;
  readonly onStateChange?: (state: BrowserEngineState) => void;
}

export function validateBrowserEngineRequest(request: BrowserEngineRequest): Required<Pick<BrowserEngineRequest, "fen" | "ply" | "profile">> {
  if (!request || typeof request.fen !== "string" || !isSafeFen(request.fen)) {
    throw new BrowserEngineError("invalid_request", "The browser engine received an invalid position.");
  }
  const ply = request.ply ?? 0;
  if (!Number.isSafeInteger(ply) || ply < 0 || ply > MAX_PLY) {
    throw new BrowserEngineError("invalid_request", "The browser engine received an invalid move index.");
  }
  return { fen: request.fen, ply, profile: normalizeBrowserEngineProfile(request.profile) };
}

export function createBrowserEngine(options: BrowserEngineOptions = {}): BrowserEngine {
  return new BrowserEngine(options);
}

export class BrowserEngine {
  private readonly workerFactory: WorkerFactory;
  private readonly handshakeTimeoutMs: number;
  private readonly cancelTimeoutMs: number;
  private readonly onStateChange?: (state: BrowserEngineState) => void;
  private worker: EngineWorker | null = null;
  private startPromise: Promise<void> | null = null;
  private waiters: LineWaiter[] = [];
  private activeAnalysis: ActiveAnalysis | null = null;
  private stateValue: BrowserEngineState = "idle";
  private disposed = false;

  constructor(options: BrowserEngineOptions = {}) {
    this.workerFactory = options.workerFactory ?? defaultWorkerFactory;
    this.handshakeTimeoutMs = boundedTimeout(options.handshakeTimeoutMs, DEFAULT_HANDSHAKE_TIMEOUT_MS);
    this.cancelTimeoutMs = boundedTimeout(options.cancelTimeoutMs, DEFAULT_CANCEL_TIMEOUT_MS);
    this.onStateChange = options.onStateChange;
  }

  get state(): BrowserEngineState {
    return this.stateValue;
  }

  async start(): Promise<void> {
    this.ensureUsable();
    if (this.stateValue === "ready") return;
    if (this.stateValue === "analyzing" || this.stateValue === "cancelling") {
      throw new BrowserEngineError("busy", "The browser engine is already analyzing a position.");
    }
    if (this.startPromise) return this.startPromise;

    this.setState("starting");
    this.startPromise = this.startWorker();
    try {
      await this.startPromise;
    } finally {
      this.startPromise = null;
    }
  }

  async analyze(request: BrowserEngineRequest, onProgress?: (progress: BrowserEngineProgress) => void): Promise<BrowserEngineObservation> {
    this.ensureUsable();
    const validated = validateBrowserEngineRequest(request);
    if (this.stateValue === "analyzing" || this.stateValue === "cancelling") {
      throw new BrowserEngineError("busy", "Finish or cancel the current browser analysis first.");
    }
    if (this.stateValue !== "ready") await this.start();
    if (!this.worker) throw new BrowserEngineError("unavailable", "The browser engine worker is unavailable.");

    const config = browserEngineProfile(validated.profile);
    const result = new Promise<BrowserEngineObservation>((resolve, reject) => {
      this.activeAnalysis = {
        profile: validated.profile,
        request: validated,
        resolve,
        reject,
        progress: onProgress,
        latest: { depth: 0, nodes: 0, timeMs: 0, score: null, principalVariation: [] },
        timer: null,
        cancelTimer: null,
        cancelPromise: null,
        cancelResolve: null,
        cancelRequested: false,
        timeoutRequested: false,
      };
    });

    const active = this.activeAnalysis;
    if (!active) throw new BrowserEngineError("failed", "The browser engine could not start analysis.");
    this.setState("analyzing");
    active.timer = setTimeout(() => this.requestTimedStop(), config.maxAnalysisMs);
    try {
      this.send("ucinewgame");
      this.send(`setoption name MultiPV value 1`);
      this.send(`position fen ${validated.fen}`);
      this.send(`go depth ${config.depth}`);
    } catch (error) {
      this.finishAnalysisError(toEngineError(error, "failed", "The browser engine could not start analysis."));
    }
    return result;
  }

  async cancel(): Promise<void> {
    const active = this.activeAnalysis;
    if (!active) return;
    if (active.cancelPromise) return active.cancelPromise;

    active.cancelRequested = true;
    active.cancelPromise = new Promise<void>((resolve) => { active.cancelResolve = resolve; });
    this.setState("cancelling");
    try {
      this.send("stop");
    } catch (error) {
      this.finishAnalysisError(toEngineError(error, "failed", "The browser engine could not stop cleanly."));
      active.cancelResolve?.();
      return active.cancelPromise;
    }
    active.cancelTimer = setTimeout(() => this.finishCancelled(), this.cancelTimeoutMs);
    return active.cancelPromise;
  }

  async restart(): Promise<void> {
    this.ensureUsable();
    if (this.activeAnalysis) await this.cancel();
    this.shutdownWorker();
    this.setState("idle");
    await this.start();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    const error = new BrowserEngineError("disposed", "The browser engine has been disposed.");
    this.rejectWaiters(error);
    this.finishAnalysisError(error);
    this.shutdownWorker();
    this.setState("disposed");
  }

  private async startWorker(): Promise<void> {
    this.shutdownWorker();
    try {
      this.worker = this.workerFactory();
    } catch (error) {
      const engineError = toEngineError(error, "unavailable", "Browser engine support is not available in this browser.");
      this.setState("failed");
      throw engineError;
    }
    this.worker.onmessage = (event) => this.handleMessage(event.data);
    this.worker.onerror = (event) => this.handleWorkerFailure(event.message || "The browser engine worker failed.");
    this.worker.onmessageerror = () => this.handleWorkerFailure("The browser engine returned an unreadable message.");
    try {
      const uciReady = this.waitForLine((line) => line === "uciok", "UCI handshake");
      this.send("uci");
      await uciReady;
      this.send("setoption name Threads value 1");
      this.send("setoption name Hash value 16");
      this.send("setoption name MultiPV value 1");
      const engineReady = this.waitForLine((line) => line === "readyok", "engine readiness");
      this.send("isready");
      await engineReady;
      this.setState("ready");
    } catch (error) {
      const engineError = toEngineError(error, "failed", "The browser engine could not finish its handshake.");
      this.rejectWaiters(engineError);
      this.shutdownWorker();
      this.setState(this.disposed ? "disposed" : "failed");
      throw engineError;
    }
  }

  private handleMessage(data: unknown): void {
    if (typeof data !== "string") return;
    const line = data.trim();
    if (!line || line.length > MAX_UCI_LINE_LENGTH || /[\r\n]/.test(line)) return;
    this.resolveWaiters(line);
    if (line.startsWith("info ")) this.handleInfo(line);
    else if (line.startsWith("bestmove ")) this.handleBestMove(line);
  }

  private handleInfo(line: string): void {
    const active = this.activeAnalysis;
    if (!active || active.cancelRequested) return;
    const info = parseInfo(line);
    if (!info) return;
    if (info.depth !== undefined) active.latest.depth = info.depth;
    if (info.nodes !== undefined) active.latest.nodes = info.nodes;
    if (info.timeMs !== undefined) active.latest.timeMs = info.timeMs;
    if (info.score !== undefined) active.latest.score = info.score;
    if (info.principalVariation !== undefined) active.latest.principalVariation = info.principalVariation;
    active.progress?.({
      profile: active.profile,
      depth: active.latest.depth,
      targetDepth: browserEngineProfile(active.profile).depth,
      nodes: active.latest.nodes,
      timeMs: active.latest.timeMs,
      percent: Math.min(0.99, active.latest.depth / browserEngineProfile(active.profile).depth),
    });
  }

  private handleBestMove(line: string): void {
    const active = this.activeAnalysis;
    if (!active) return;
    const match = /^bestmove\s+(\S+)(?:\s+ponder\s+(\S+))?$/.exec(line);
    const bestMove = match?.[1];
    const ponderMove = match?.[2];
    if (!bestMove || !isUciMove(bestMove) || (ponderMove !== undefined && !isUciMove(ponderMove))) {
      this.finishAnalysisError(new BrowserEngineError("failed", "The browser engine returned an invalid move."));
      return;
    }
    if (active.timeoutRequested) {
      this.finishAnalysisError(new BrowserEngineError("timeout", "The browser engine exceeded its analysis limit."));
      return;
    }
    if (active.cancelRequested) {
      this.finishCancelled();
      return;
    }
    const observation: BrowserEngineObservation = {
      bestMove,
      ...(ponderMove ? { ponderMove } : {}),
      score: active.latest.score,
      depth: active.latest.depth,
      nodes: active.latest.nodes,
      timeMs: active.latest.timeMs,
      principalVariation: active.latest.principalVariation,
      profile: active.profile,
      engineVersion: BROWSER_ENGINE_VERSION,
      source: "browser",
    };
    this.finishAnalysisSuccess(observation);
  }

  private requestTimedStop(): void {
    const active = this.activeAnalysis;
    if (!active || active.cancelRequested) return;
    active.timeoutRequested = true;
    active.cancelRequested = true;
    this.setState("cancelling");
    try {
      this.send("stop");
    } catch {
      this.finishAnalysisError(new BrowserEngineError("timeout", "The browser engine exceeded its analysis limit."));
      return;
    }
    active.cancelTimer = setTimeout(() => this.finishAnalysisError(new BrowserEngineError("timeout", "The browser engine exceeded its analysis limit.")), this.cancelTimeoutMs);
  }

  private finishCancelled(): void {
    const active = this.activeAnalysis;
    if (!active) return;
    this.clearAnalysisTimers(active);
    this.activeAnalysis = null;
    active.reject(new BrowserEngineError("cancelled", "Browser analysis cancelled."));
    active.cancelResolve?.();
    this.setState("ready");
  }

  private finishAnalysisSuccess(observation: BrowserEngineObservation): void {
    const active = this.activeAnalysis;
    if (!active) return;
    this.clearAnalysisTimers(active);
    this.activeAnalysis = null;
    active.resolve(observation);
    this.setState("ready");
  }

  private finishAnalysisError(error: BrowserEngineError): void {
    const active = this.activeAnalysis;
    if (!active) return;
    this.clearAnalysisTimers(active);
    this.activeAnalysis = null;
    active.reject(error);
    active.cancelResolve?.();
    this.setState(error.code === "cancelled" ? "ready" : "failed");
  }

  private clearAnalysisTimers(active: ActiveAnalysis): void {
    if (active.timer) clearTimeout(active.timer);
    if (active.cancelTimer) clearTimeout(active.cancelTimer);
    active.timer = null;
    active.cancelTimer = null;
  }

  private handleWorkerFailure(message: string): void {
    const error = new BrowserEngineError("failed", message);
    this.rejectWaiters(error);
    this.finishAnalysisError(error);
    this.shutdownWorker();
    this.setState("failed");
  }

  private waitForLine(predicate: LinePredicate, operation: string): Promise<void> {
    return new Promise((resolve, reject) => {
      const waiter: LineWaiter = {
        predicate,
        resolve,
        reject,
        timer: setTimeout(() => {
          this.waiters = this.waiters.filter((candidate) => candidate !== waiter);
          reject(new BrowserEngineError("timeout", `The browser engine timed out during ${operation}.`));
        }, this.handshakeTimeoutMs),
      };
      this.waiters.push(waiter);
    });
  }

  private resolveWaiters(line: string): void {
    const remaining: LineWaiter[] = [];
    for (const waiter of this.waiters) {
      if (!waiter.predicate(line)) {
        remaining.push(waiter);
        continue;
      }
      if (waiter.timer) clearTimeout(waiter.timer);
      waiter.resolve();
    }
    this.waiters = remaining;
  }

  private rejectWaiters(error: BrowserEngineError): void {
    const waiters = this.waiters;
    this.waiters = [];
    for (const waiter of waiters) {
      if (waiter.timer) clearTimeout(waiter.timer);
      waiter.reject(error);
    }
  }

  private send(command: string): void {
    if (!this.worker) throw new BrowserEngineError("unavailable", "The browser engine worker is unavailable.");
    this.worker.postMessage(command);
  }

  private shutdownWorker(): void {
    if (!this.worker) return;
    this.worker.onmessage = null;
    this.worker.onerror = null;
    this.worker.onmessageerror = null;
    this.worker.terminate();
    this.worker = null;
  }

  private setState(state: BrowserEngineState): void {
    this.stateValue = state;
    this.onStateChange?.(state);
  }

  private ensureUsable(): void {
    if (this.disposed) throw new BrowserEngineError("disposed", "The browser engine has been disposed.");
  }
}

function defaultWorkerFactory(): EngineWorker {
  if (typeof globalThis.Worker !== "function" || !globalThis.location?.origin) {
    throw new BrowserEngineError("unavailable", "This browser cannot run a Stockfish worker.");
  }
  const scriptUrl = new URL(ENGINE_SCRIPT_PATH, globalThis.location.origin);
  const wasmUrl = new URL(ENGINE_WASM_PATH, globalThis.location.origin);
  scriptUrl.hash = `${encodeURIComponent(wasmUrl.href)},worker`;
  return new globalThis.Worker(scriptUrl.href) as unknown as EngineWorker;
}

function parseInfo(line: string): ParsedInfo | null {
  const tokens = line.split(/\s+/);
  const depth = boundedNumber(tokens, "depth", 0, 128);
  const nodes = boundedNumber(tokens, "nodes", 0, Number.MAX_SAFE_INTEGER);
  const timeMs = boundedNumber(tokens, "time", 0, 86_400_000);
  const scoreIndex = tokens.indexOf("score");
  let score: BrowserEngineScore | undefined;
  if (scoreIndex >= 0 && tokens[scoreIndex + 1] && tokens[scoreIndex + 2]) {
    const value = Number(tokens[scoreIndex + 2]);
    const scoreType = tokens[scoreIndex + 1] === "cp" ? "cp" : tokens[scoreIndex + 1] === "mate" ? "mate" : null;
    if (Number.isSafeInteger(value) && scoreType && Math.abs(value) <= 1_000_000) {
      score = { type: scoreType, value };
    }
  }
  const pvIndex = tokens.indexOf("pv");
  const principalVariation = pvIndex >= 0
    ? tokens.slice(pvIndex + 1, pvIndex + 1 + MAX_PV_LENGTH).filter(isUciMove)
    : undefined;
  if (depth === undefined && nodes === undefined && timeMs === undefined && score === undefined && principalVariation === undefined) return null;
  return { depth, nodes, timeMs, score, principalVariation };
}

function boundedNumber(tokens: string[], key: string, min: number, max: number): number | undefined {
  const index = tokens.indexOf(key);
  if (index < 0 || !tokens[index + 1]) return undefined;
  const value = Number(tokens[index + 1]);
  return Number.isSafeInteger(value) && value >= min && value <= max ? value : undefined;
}

function isUciMove(value: string): boolean {
  return value === "0000" || /^[a-h][1-8][a-h][1-8][qrbn]?$/.test(value);
}

function isSafeFen(value: string): boolean {
  if (value.length === 0 || value.length > MAX_FEN_LENGTH || value.trim() !== value || /[\r\n\t]/.test(value)) return false;
  const fields = value.split(/ +/);
  if (fields.length !== 6) return false;
  const ranks = fields[0].split("/");
  if (ranks.length !== 8 || ranks.some((rank) => !validRank(rank))) return false;
  if (fields[1] !== "w" && fields[1] !== "b") return false;
  if (fields[2] !== "-" && !/^[KQkq]+$/.test(fields[2])) return false;
  if (fields[3] !== "-" && !/^[a-h][36]$/.test(fields[3])) return false;
  return /^\d{1,6}$/.test(fields[4]) && /^[1-9]\d{0,5}$/.test(fields[5]);
}

function validRank(rank: string): boolean {
  if (!/^[prnbqkPRNBQK1-8]+$/.test(rank)) return false;
  let squares = 0;
  for (const symbol of rank) squares += /\d/.test(symbol) ? Number(symbol) : 1;
  return squares === 8;
}

function boundedTimeout(value: number | undefined, fallback: number): number {
  return Number.isFinite(value) && value !== undefined && value >= 100 ? Math.min(value, 120_000) : fallback;
}

function toEngineError(error: unknown, code: BrowserEngineErrorCode, fallback: string): BrowserEngineError {
  if (error instanceof BrowserEngineError) return error;
  return new BrowserEngineError(code, error instanceof Error && error.message ? error.message : fallback);
}
