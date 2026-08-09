import {
  BrowserEngineError,
  createBrowserAnalysisRequest,
  createBrowserObservationPayload,
  createBrowserEngine,
  validateBrowserEngineRequest,
} from "../src/browser-engine";
import {
  BROWSER_ENGINE_ASSET_HASH,
  browserEngineProfile,
  normalizeBrowserEngineProfile,
} from "../src/engine-profile";

const startFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

class FakeWorker {
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onerror: ((event: { message?: string; error?: unknown }) => void) | null = null;
  onmessageerror: (() => void) | null = null;
  readonly messages: string[] = [];
  terminated = false;

  constructor(private readonly completeSearch: boolean) {}

  postMessage(message: string) {
    this.messages.push(message);
    if (message === "uci") this.emit("uciok");
    if (message === "isready") this.emit("readyok");
    if (message.startsWith("go depth")) {
      this.emit("info depth 4 nodes 200 time 12 score cp 34 pv e2e4 e7e5");
      if (this.completeSearch) this.emit("bestmove e2e4 ponder e7e5");
    }
    if (message === "stop") this.emit("bestmove 0000");
  }

  terminate() {
    this.terminated = true;
  }

  private emit(data: string) {
    queueMicrotask(() => this.onmessage?.({ data }));
  }
}

function expect(condition: unknown, message: string) {
  if (!condition) throw new Error(message);
}

async function expectRejected(promise: Promise<unknown>, code: BrowserEngineError["code"], message: string) {
  try {
    await promise;
  } catch (error) {
    expect(error instanceof BrowserEngineError && error.code === code, message);
    return;
  }
  throw new Error(message);
}

async function run() {
  expect(browserEngineProfile("quick").depth === 10, "quick should use the pinned shallow depth");
  expect(browserEngineProfile("balanced").depth === 14, "balanced should use the pinned deeper depth");
  expect(browserEngineProfile("aggressive").depth === 18, "aggressive should use the explicit longest browser depth");
  expect(browserEngineProfile("aggressive").maxAnalysisMs === 60_000, "aggressive should carry its battery-aware time budget");
  expect(normalizeBrowserEngineProfile("aggressive") === "aggressive", "aggressive should survive profile normalization");
  expect(normalizeBrowserEngineProfile("unexpected") === "quick", "unknown profiles should fail closed to quick");
  expect(validateBrowserEngineRequest({ fen: startFen }).ply === 0, "missing ply should default to zero");
  await expectRejected(Promise.resolve().then(() => validateBrowserEngineRequest({ fen: `${startFen}\nuci` })), "invalid_request", "newline injection must be rejected");

  const workers: FakeWorker[] = [];
  const engine = createBrowserEngine({
    workerFactory: () => {
      const worker = new FakeWorker(true);
      workers.push(worker);
      return worker;
    },
    handshakeTimeoutMs: 500,
  });
  await engine.start();
  expect(engine.state === "ready", "engine should be ready after the UCI handshake");
  const progress: number[] = [];
  const observation = await engine.analyze({ fen: startFen, profile: "quick" }, (event) => progress.push(event.percent));
  expect(observation.bestMove === "e2e4", "engine should return the typed best move");
  expect(observation.ponderMove === "e7e5", "engine should preserve the optional ponder move");
  expect(observation.principalVariation.join(" ") === "e2e4 e7e5", "engine should preserve a bounded principal variation");
  expect(observation.engineHash === BROWSER_ENGINE_ASSET_HASH, "engine observations should carry the pinned asset hash");
  const request = createBrowserAnalysisRequest({ analysisRunId: "run-1", gameId: "game-1" }, "quick");
  expect(request.engine.hash === BROWSER_ENGINE_ASSET_HASH, "run requests should pin the same engine asset");
  const payload = createBrowserObservationPayload(observation, {
    analysisRunId: "run-1",
    gameId: "game-1",
    ply: 0,
    sequence: 0,
    fen: startFen,
  });
  expect(payload.lines[0].moves[0] === "e2e4", "observation payloads should preserve the legal PV");
  const terminalPayload = createBrowserObservationPayload({
    ...observation,
    bestMove: "0000",
    score: { type: "mate", value: 1 },
    principalVariation: [],
  }, {
    analysisRunId: "run-1",
    gameId: "game-1",
    ply: 1,
    sequence: 1,
    fen: startFen,
  });
  expect(terminalPayload.lines[0].moves.length === 0, "terminal observations should not invent a legal move");
  expect(progress.length === 1 && progress[0] > 0, "progress should come from an engine info line");
  expect(engine.state === "ready", "engine should return to ready after analysis");

  const cancellableWorkers: FakeWorker[] = [];
  const cancellable = createBrowserEngine({
    workerFactory: () => {
      const worker = new FakeWorker(false);
      cancellableWorkers.push(worker);
      return worker;
    },
    handshakeTimeoutMs: 500,
    cancelTimeoutMs: 500,
  });
  await cancellable.start();
  const pending = cancellable.analyze({ fen: startFen });
  await new Promise((resolve) => setTimeout(resolve, 0));
  await cancellable.cancel();
  await expectRejected(pending, "cancelled", "cancel should reject the active analysis with a typed error");
  expect(cancellable.state === "ready", "cancel should keep a healthy worker ready for another request");

  await cancellable.restart();
  expect(cancellableWorkers.length === 2, "restart should replace the worker so a stalled search cannot leak state");
  cancellable.dispose();
  expect(cancellable.state === "disposed", "dispose should make the lifecycle terminal");

  console.log("browser engine tests passed");
}

run().catch((error: unknown) => {
  console.error(error);
  throw error;
});
