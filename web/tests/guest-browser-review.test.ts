import { runGuestBrowserReview, type GuestBrowserReviewApi, type GuestBrowserReviewEngine } from "../src/guest-browser-review";
import type { BrowserEngineObservation } from "../src/browser-engine";
import type { StoredGame } from "../src/types";

function assert(condition: unknown, message: string): void {
  if (!condition) throw new Error(message);
}

const game: StoredGame = {
  game: {
    id: "game-guest",
    tags: { Result: "1-0" },
    plies: [
      {
        ply: 0,
        san: "e4",
        uci: "e2e4",
        fen_before: "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        fen_after: "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
        clock_ms: null,
        elapsed_ms: null,
      },
      {
        ply: 1,
        san: "e5",
        uci: "e7e5",
        fen_before: "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
        fen_after: "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
        clock_ms: null,
        elapsed_ms: null,
      },
    ],
  },
  source_url: "",
  import_method: "pgn",
  analysis_status: "pending",
  analysis: null,
};

class FakeEngine implements GuestBrowserReviewEngine {
  started = false;
  disposed = false;
  readonly fens: string[] = [];

  async start(): Promise<void> { this.started = true; }
  async analyze(request: { fen: string; ply?: number; profile?: "quick" | "balanced" }): Promise<BrowserEngineObservation> {
    this.fens.push(request.fen);
    return {
      bestMove: request.ply === 0 ? "e2e4" : "e7e5",
      score: { type: "cp", value: 24 },
      depth: 10,
      nodes: 1000,
      timeMs: 40,
      principalVariation: [request.ply === 0 ? "e2e4" : "e7e5"],
      profile: request.profile ?? "quick",
      engineName: "Stockfish",
      engineVersion: "stockfish-18.0.8-lite-single",
      engineHash: "js:5243fd9b276cab7dfe3ad1d43ab9ead73568fac76468c614242977a210c4a391;wasm:a8fbc05ec6920b56d7485826dcb02c5ffd2826bcbf751cf973046f237a9096f1",
      source: "browser",
    };
  }
  async cancel(): Promise<void> {}
  dispose(): void { this.disposed = true; }
}

const startedRequests: unknown[] = [];
const submitted: Array<{ sequence: number; ply: number; fen: string }> = [];
const api: GuestBrowserReviewApi = {
  start: async (request) => {
    startedRequests.push(request);
    return { ...request, status: "collecting", expectedObservations: 4 };
  },
  submit: async (request) => {
    submitted.push({ sequence: request.sequence, ply: request.ply, fen: request.fen });
    return { status: "accepted", staging: true, analysisRunId: request.analysisRunId, gameId: request.gameId, ply: request.ply, sequence: request.sequence };
  },
  finalize: async (gameId, analysisRunId) => ({
    status: "complete",
    staging: false,
    gameId,
    analysisRunId,
    analysis: null as never,
  }),
};

async function run() {
  const progress: string[] = [];
  const engine = new FakeEngine();
  const result = await runGuestBrowserReview(game, {
    profile: "quick",
    engine,
    api,
    runId: "guest-run-1",
    onProgress: (event) => progress.push(`${event.stage}:${event.complete}/${event.total}`),
  });

  assert(engine.started, "the guest flow starts the browser worker");
  assert(!engine.disposed, "an injected engine remains owned by its caller");
  assert(startedRequests.length === 1, "the guest flow registers one run");
  assert(submitted.length === 4, "the guest flow submits before and after evidence for every ply");
  assert(submitted.every((item, index) => item.sequence === index), "observations are submitted in sequence");
  assert(submitted[0]?.fen === game.game.plies[0]?.fen_before, "the first observation uses the canonical before FEN");
  assert(submitted[1]?.fen === game.game.plies[0]?.fen_after, "the second observation uses the canonical after FEN");
  assert(progress.includes("finalizing:4/4"), "progress reaches C++ finalization with real counts");
  assert(result.status === "complete" && result.gameId === game.game.id, "the guest flow returns the completed review response");

  console.log("guest browser review tests passed");
}

run().catch((error: unknown) => {
  console.error(error);
  throw error;
});
