import {
  BrowserEngineError,
  createBrowserAnalysisRequest,
  createBrowserObservationPayload,
  type BrowserEngine,
  type BrowserAnalysisRequest,
  type BrowserEngineObservation,
  type BrowserEngineProgress,
  type BrowserObservationPayload,
} from "./browser-engine";
import type { BrowserEngineProfile } from "./engine-profile";
import type { StoredGame } from "./types";

export type GuestBrowserReviewPosition = "before" | "after";

export type GuestBrowserReviewProgress = {
  readonly gameId: string;
  readonly profile: BrowserEngineProfile;
  readonly stage: "starting" | "analyzing" | "submitting" | "finalizing";
  readonly complete: number;
  readonly total: number;
  readonly ply: number;
  readonly position: GuestBrowserReviewPosition;
  readonly depth?: number;
  readonly targetDepth?: number;
  readonly message: string;
};

export type GuestBrowserReviewEngine = Pick<BrowserEngine, "start" | "analyze" | "cancel" | "dispose">;

export type GuestBrowserReviewStartResponse = BrowserAnalysisRequest & {
  readonly status: "collecting";
  readonly expectedObservations: number;
};

export type GuestBrowserReviewObservationResponse = {
  readonly status: "accepted" | "duplicate";
  readonly staging: true;
  readonly analysisRunId: string;
  readonly gameId: string;
  readonly ply: number;
  readonly sequence: number;
};

export type GuestBrowserReviewFinalizationResponse = {
  readonly status: "complete";
  readonly staging: false;
  readonly analysisRunId: string;
  readonly gameId: string;
  readonly analysis: NonNullable<StoredGame["analysis"]>;
};

export type GuestBrowserReviewApi = {
  readonly start: (request: BrowserAnalysisRequest) => Promise<GuestBrowserReviewStartResponse>;
  readonly submit: (payload: BrowserObservationPayload) => Promise<GuestBrowserReviewObservationResponse>;
  readonly finalize: (gameId: string, analysisRunId: string) => Promise<GuestBrowserReviewFinalizationResponse>;
};

export type GuestBrowserReviewOptions = {
  readonly profile: BrowserEngineProfile;
  readonly engine: GuestBrowserReviewEngine;
  readonly api: GuestBrowserReviewApi;
  readonly runId?: string;
  readonly signal?: AbortSignal;
  readonly onProgress?: (progress: GuestBrowserReviewProgress) => void;
};

/**
 * Runs one guest review through the browser worker and C++ observation boundary.
 * The worker only supplies evidence; final classifications come back from C++.
 */
export async function runGuestBrowserReview(
  stored: StoredGame,
  options: GuestBrowserReviewOptions,
): Promise<GuestBrowserReviewFinalizationResponse> {
  const gameId = stored.game.id;
  const plies = stored.game.plies;
  if (plies.length === 0) throw new BrowserEngineError("invalid_request", "This game has no moves to analyze.");

  const api = options.api;
  const runId = options.runId ?? createRunId();
  const request = createBrowserAnalysisRequest({ analysisRunId: runId, gameId }, options.profile);
  const engine = options.engine;
  const expectedObservations = plies.length * 2;
  let sequence = 0;
  let currentPly = 0;
  let currentPosition: GuestBrowserReviewPosition = "before";
  let abortListener: (() => void) | null = null;

  const report = (progress: Omit<GuestBrowserReviewProgress, "gameId" | "profile">) => {
    options.onProgress?.({ ...progress, gameId, profile: options.profile });
  };
  const throwIfAborted = () => {
    if (options.signal?.aborted) throw new BrowserEngineError("cancelled", "Browser analysis cancelled.");
  };
  const onEngineProgress = (progress: BrowserEngineProgress) => {
    report({
      stage: "analyzing",
      complete: sequence,
      total: expectedObservations,
      ply: currentPly,
      position: currentPosition,
      depth: progress.depth,
      targetDepth: progress.targetDepth,
      message: `Analyzing ${currentPosition} position ${currentPly + 1} of ${plies.length}`,
    });
  };
  abortListener = () => { void engine.cancel(); };
  if (options.signal?.aborted) throw new BrowserEngineError("cancelled", "Browser analysis cancelled.");
  options.signal?.addEventListener("abort", abortListener, { once: true });

  try {
    report({
      stage: "starting",
      complete: 0,
      total: expectedObservations,
      ply: 0,
      position: "before",
      message: `Starting ${options.profile === "balanced" ? "Balanced" : "Quick"} browser analysis`,
    });
    const started = await api.start(request);
    if (started.expectedObservations !== expectedObservations) {
      throw new BrowserEngineError("failed", "The service returned an incomplete browser-analysis contract.");
    }
    throwIfAborted();
    await engine.start();

    for (const [ply, canonical] of plies.entries()) {
      for (const position of ["before", "after"] as const) {
        currentPly = ply;
        currentPosition = position;
        throwIfAborted();
        const fen = position === "before" ? canonical.fen_before : canonical.fen_after;
        const observation = await engine.analyze({ fen, ply, profile: options.profile }, onEngineProgress);
        throwIfAborted();
        report({
          stage: "submitting",
          complete: sequence,
          total: expectedObservations,
          ply,
          position,
          depth: observation.depth,
          targetDepth: undefined,
          message: `Validating ${position} position ${ply + 1} of ${plies.length}`,
        });
        await submitObservation(api, observation, { analysisRunId: runId, gameId, ply, sequence, fen });
        sequence += 1;
        report({
          stage: "submitting",
          complete: sequence,
          total: expectedObservations,
          ply,
          position,
          depth: observation.depth,
          targetDepth: undefined,
          message: `${sequence} of ${expectedObservations} positions validated`,
        });
      }
    }

    throwIfAborted();
    report({
      stage: "finalizing",
      complete: sequence,
      total: expectedObservations,
      ply: Math.max(0, plies.length - 1),
      position: "after",
      message: "Assembling the review in C++",
    });
    return await api.finalize(gameId, runId);
  } finally {
    if (options.signal && abortListener) options.signal.removeEventListener("abort", abortListener);
    // The caller owns the worker so it can expose cancellation in its UI.
  }
}

async function submitObservation(
  api: GuestBrowserReviewApi,
  observation: BrowserEngineObservation,
  context: { analysisRunId: string; gameId: string; ply: number; sequence: number; fen: string },
): Promise<GuestBrowserReviewObservationResponse> {
  return api.submit(createBrowserObservationPayload(observation, context));
}

function createRunId(): string {
  const suffix = typeof crypto !== "undefined" && typeof crypto.randomUUID === "function"
    ? crypto.randomUUID()
    : `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  return `guest-browser-${suffix}`;
}
