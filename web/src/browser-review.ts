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

export type BrowserReviewPosition = "before" | "after";

export type BrowserReviewProgress = {
  readonly gameId: string;
  readonly profile: BrowserEngineProfile;
  readonly stage: "starting" | "analyzing" | "submitting" | "finalizing";
  readonly complete: number;
  readonly total: number;
  readonly ply: number;
  readonly position: BrowserReviewPosition;
  readonly depth?: number;
  readonly targetDepth?: number;
  readonly message: string;
};

export type BrowserReviewEngine = Pick<BrowserEngine, "start" | "analyze" | "cancel" | "dispose">;

export type BrowserReviewStartResponse = BrowserAnalysisRequest & {
  readonly status: "collecting";
  readonly expectedObservations: number;
};

export type BrowserReviewObservationResponse = {
  readonly status: "accepted" | "duplicate";
  readonly staging: true;
  readonly analysisRunId: string;
  readonly gameId: string;
  readonly ply: number;
  readonly sequence: number;
};

export type BrowserReviewFinalizationResponse = {
  readonly status: "complete";
  readonly staging: false;
  readonly analysisRunId: string;
  readonly gameId: string;
  readonly analysis: NonNullable<StoredGame["analysis"]>;
};

export type BrowserReviewApi = {
  readonly start: (request: BrowserAnalysisRequest) => Promise<BrowserReviewStartResponse>;
  readonly submit: (payload: BrowserObservationPayload) => Promise<BrowserReviewObservationResponse>;
  readonly finalize: (gameId: string, analysisRunId: string) => Promise<BrowserReviewFinalizationResponse>;
};

export type BrowserReviewOptions = {
  readonly profile: BrowserEngineProfile;
  readonly engine: BrowserReviewEngine;
  readonly api: BrowserReviewApi;
  readonly runId?: string;
  readonly signal?: AbortSignal;
  readonly onProgress?: (progress: BrowserReviewProgress) => void;
};

/**
 * Runs one account review through the browser worker and C++ observation boundary.
 * The worker only supplies evidence; final classifications come back from C++.
 */
export async function runBrowserReview(
  stored: StoredGame,
  options: BrowserReviewOptions,
): Promise<BrowserReviewFinalizationResponse> {
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
  let currentPosition: BrowserReviewPosition = "before";
  let abortListener: (() => void) | null = null;

  const report = (progress: Omit<BrowserReviewProgress, "gameId" | "profile">) => {
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
      message: `Starting ${profileLabel(options.profile)} browser analysis`,
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
  api: BrowserReviewApi,
  observation: BrowserEngineObservation,
  context: { analysisRunId: string; gameId: string; ply: number; sequence: number; fen: string },
): Promise<BrowserReviewObservationResponse> {
  return api.submit(createBrowserObservationPayload(observation, context));
}

function createRunId(): string {
  const suffix = typeof crypto !== "undefined" && typeof crypto.randomUUID === "function"
    ? crypto.randomUUID()
    : `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  return `browser-${suffix}`;
}

function profileLabel(profile: BrowserEngineProfile): string {
  return profile[0].toUpperCase() + profile.slice(1);
}
