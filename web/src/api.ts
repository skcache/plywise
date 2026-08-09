import type {
  ApiFailure,
  BatchProgress,
  ChessComProfileResponse,
  Diagnostics,
  Drill,
  DrillAttempt,
  ImportGameResponse,
  ImportResolution,
  IngestSync,
  Job,
  Profile,
  ResourceRecommendation,
  RuntimeSettings,
  ReviewAttempt,
  StoredGame,
  Variation,
  VariationAnalysis,
} from "./types";
import { apiUrl } from "./config/runtime";
import { loadGuestSession } from "./guest-session";

export class ApiError extends Error {
  readonly status: number;
  readonly code?: string;
  readonly actions: NonNullable<ApiFailure["actions"]>;

  constructor(status: number, body: ApiFailure) {
    super(body.error || `Request failed with HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.code = body.code;
    this.actions = body.actions ?? [];
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const headers = new Headers(init?.headers);
  const guestSession = loadGuestSession();
  if (guestSession && !headers.has("Authorization")) {
    headers.set("Authorization", `Bearer ${guestSession.token}`);
  }
  if (init?.body != null && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  const response = await fetch(apiUrl(path), {
    ...init,
    headers,
    redirect: "error",
  });
  const contentType = response.headers.get("content-type") ?? "";
  if (!contentType.includes("application/json")) {
    throw new Error("Plywise API is unavailable.");
  }
  const body = (await response.json()) as T & Partial<ApiFailure>;
  if (!response.ok) {
    throw new ApiError(response.status, {
      error: body.error ?? `Request failed with HTTP ${response.status}`,
      code: body.code,
      actions: body.actions,
    });
  }
  return body;
}

export type GuestSessionResponse = {
  guest_id: string;
  token: string;
  expires_at_ms: number;
};

export type GuestClaimResponse = {
  guest_id: string;
  account_id: string;
  transferred_games: number;
  already_claimed: boolean;
};

export function createGuestSession(): Promise<GuestSessionResponse> {
  return request("/api/guest/session", { method: "POST", body: "{}" });
}

export function claimGuestReview(guestToken: string, idempotencyKey: string): Promise<GuestClaimResponse> {
  return request("/api/guest/claim", {
    method: "POST",
    body: JSON.stringify({ guest_token: guestToken, idempotency_key: idempotencyKey }),
  });
}

export async function listGames(): Promise<StoredGame[]> {
  const response = await request<{ games: StoredGame[] }>("/api/games");
  return response.games.map(normalizeStoredGame);
}

export async function loadGame(id: string): Promise<StoredGame> {
  return normalizeStoredGame(await request<StoredGame>(`/api/games/${encodeURIComponent(id)}`));
}

export function createVariation(gameId: string, rootPly: number, rootPosition: "before" | "after"): Promise<Variation> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations`, {
    method: "POST",
    body: JSON.stringify({ root_ply: rootPly, root_position: rootPosition }),
  });
}

export async function listVariations(gameId: string): Promise<Variation[]> {
  const response = await request<{ variations: Variation[] }>(`/api/games/${encodeURIComponent(gameId)}/variations`);
  return response.variations;
}

export function extendVariation(gameId: string, variationId: string, nodeId: number, uci: string): Promise<Variation> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations/${encodeURIComponent(variationId)}/moves`, {
    method: "POST",
    body: JSON.stringify({ node_id: nodeId, uci }),
  });
}

export function setVariationCursor(gameId: string, variationId: string, nodeId: number): Promise<Variation> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations/${encodeURIComponent(variationId)}/cursor`, {
    method: "POST",
    body: JSON.stringify({ node_id: nodeId }),
  });
}

export function resetVariation(gameId: string, variationId: string): Promise<Variation> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations/${encodeURIComponent(variationId)}/reset`, {
    method: "POST",
    body: "{}",
  });
}

export function deleteVariation(gameId: string, variationId: string): Promise<{ deleted: boolean; id: string }> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations/${encodeURIComponent(variationId)}`, {
    method: "DELETE",
  });
}

export function analyzeVariation(gameId: string, variationId: string): Promise<VariationAnalysis> {
  return request(`/api/games/${encodeURIComponent(gameId)}/variations/${encodeURIComponent(variationId)}/analysis`, {
    method: "POST",
    body: "{}",
  });
}

export function submitReviewAttempt(gameId: string, ply: number, uci: string): Promise<ReviewAttempt> {
  return request(`/api/games/${encodeURIComponent(gameId)}/moves/${ply}/retry`, {
    method: "POST",
    body: JSON.stringify({ uci }),
  });
}

function normalizeStoredGame(game: StoredGame): StoredGame {
  for (const move of game.analysis?.moves ?? []) {
    const value = String(move.classification || move.quality || "Good");
    move.classification = `${value.charAt(0).toUpperCase()}${value.slice(1).toLowerCase()}` as typeof move.classification;
  }
  return game;
}

export function importGame(input: { url: string } | { pgn: string }): Promise<{
  duplicate: boolean;
  game_id: string;
}> {
  return request("/api/import", { method: "POST", body: JSON.stringify(input) });
}

/**
 * Models synchronous local/PGN import and asynchronous Chess.com resolution.
 * Import persists a canonical game; analysis is a separate explicit action.
 */
export function importGameObservable(
  input: { url: string; username?: string } | { pgn: string },
): Promise<ImportGameResponse> {
  return request("/api/import", { method: "POST", body: JSON.stringify(input) });
}

export function startAnalysis(id: string): Promise<Job> {
  return request(`/api/games/${encodeURIComponent(id)}/analysis`, { method: "POST" });
}

export function loadJob(id: number): Promise<Job> {
  return request(`/api/jobs/${id}`);
}

export async function loadJobs(): Promise<{ jobs: Job[]; paused: boolean }> {
  return request("/api/jobs");
}

export function cancelJob(id: number): Promise<Job> {
  return request(`/api/jobs/${id}`, { method: "DELETE" });
}

/** Re-submits analysis through the public job route; it does not expose engine internals. */
export function retryAnalysis(job: Pick<Job, "game_id">): Promise<Job> {
  return startAnalysis(job.game_id);
}

export function loadDiagnostics(): Promise<Diagnostics> {
  return request("/api/diagnostics");
}

export function loadRuntimeSettings(): Promise<RuntimeSettings> {
  return request("/api/settings");
}

export function loadChessComProfile(): Promise<ChessComProfileResponse> {
  return request("/api/chesscom/profile");
}

export function configureChessComProfile(input: {
  username: string;
  time_controls?: string[];
}): Promise<ChessComProfileResponse> {
  return request("/api/chesscom/profile", { method: "PUT", body: JSON.stringify(input) });
}

export function resolveImport(input: { url: string; username?: string }): Promise<ImportResolution> {
  return request("/api/import/resolve", { method: "POST", body: JSON.stringify(input) });
}

export function loadImportResolution(id: string): Promise<ImportResolution> {
  return request(`/api/import/resolutions/${encodeURIComponent(id)}`);
}

export function cancelImportResolution(id: string): Promise<ImportResolution> {
  return request(`/api/import/resolutions/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function retryImportResolution(
  resolution: Pick<ImportResolution, "url" | "username">,
): Promise<ImportResolution> {
  return resolveImport({ url: resolution.url, ...(resolution.username ? { username: resolution.username } : {}) });
}

export function startChessComSync(input: {
  days: 7 | 30 | 90;
  username?: string;
}): Promise<IngestSync> {
  return request("/api/chesscom/sync", { method: "POST", body: JSON.stringify(input) });
}

export function loadChessComSync(id = "current"): Promise<IngestSync> {
  return request(`/api/chesscom/sync/${encodeURIComponent(id)}`);
}

export function cancelChessComSync(id = "current"): Promise<IngestSync> {
  return request(`/api/chesscom/sync/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export async function loadDrills(): Promise<Drill[]> {
  return (await request<{ drills: Drill[] }>("/api/drills")).drills;
}

export function generateSupplementalDrills(): Promise<{ added: number; drills: Drill[] }> {
  return request("/api/drills/supplemental", { method: "POST", body: "{}" });
}

export function loadProfile(): Promise<Profile> {
  return request<Profile>("/api/profile");
}

export async function loadResources(): Promise<ResourceRecommendation[]> {
  return (await request<{ resources: ResourceRecommendation[] }>("/api/resources")).resources;
}

export function submitDrillAttempt(id: string, move: string, responseTimeMs: number, hintLevel: number): Promise<{ attempt: DrillAttempt; drill: Drill }> {
  return request(`/api/drills/${encodeURIComponent(id)}/attempt`, {
    method: "POST",
    body: JSON.stringify({ move, response_time_ms: responseTimeMs, hint_level: hintLevel }),
  });
}

export function beginDrillSession(id: string): Promise<Drill> {
  return request(`/api/drills/${encodeURIComponent(id)}/session`, {
    method: "POST",
    body: "{}",
  });
}

export function advanceDrillHint(id: string): Promise<Drill> {
  return request(`/api/drills/${encodeURIComponent(id)}/hint`, {
    method: "POST",
    body: "{}",
  });
}

export function completeResource(id: string): Promise<{ completed: boolean }> {
  return request(`/api/resources/${encodeURIComponent(id)}/complete`, { method: "POST", body: "{}" });
}

export function importBatch(pgns: string[], urls: string[]): Promise<{ discovered: number; imported: number; duplicates: number; queued: number; failed: number }> {
  return request("/api/import/batch", { method: "POST", body: JSON.stringify({ pgns, urls }) });
}

export function loadBatches(): Promise<{ batches: BatchProgress[]; paused: boolean; cache_hits: number }> {
  return request("/api/batches");
}

export function setQueuePaused(paused: boolean): Promise<{ paused: boolean }> {
  return request(`/api/jobs/${paused ? "pause" : "resume"}`, { method: "POST", body: "{}" });
}
