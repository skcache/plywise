export interface Ply {
  ply: number;
  san: string;
  uci: string;
  fen_before: string;
  fen_after: string;
  clock_ms: number | null;
  elapsed_ms: number | null;
}

export interface GameData {
  id: string;
  tags: Record<string, string>;
  plies: Ply[];
}

export interface EngineLine {
  multipv: number;
  depth: number;
  centipawns: number | null;
  mate: number | null;
  nodes: number;
  time_ms: number;
  moves: string[];
}

export interface Mistake {
  rank: number;
  ply: number;
  san: string;
  fen: string;
  evaluation_before: number;
  evaluation_after: number;
  loss: number;
  phase: string;
  opening: string;
  category: string;
  explanation: string;
  punishment: string;
  better_moves: string[];
  engine: {
    best_move: string;
    ponder_move: string;
    lines: EngineLine[];
  };
  evidence: string[];
  confidence: "proven" | "suggestive";
  classifier_version: string;
}

export interface MoveAssessment {
  ply: number;
  move_number: number;
  side: "white" | "black";
  played_uci: string;
  played_san: string;
  san: string;
  fen_before: string;
  fen_after: string;
  best_uci: string;
  best_san: string;
  evaluation_before: number;
  evaluation_after: number;
  evaluation_after_best: number;
  loss: number;
  expected_points_before: number;
  expected_points_after: number;
  expected_points_loss: number;
  quality: string;
  classification: "Brilliant" | "Great" | "Best" | "Excellent" | "Good" | "Book" | "Inaccuracy" | "Mistake" | "Miss" | "Blunder";
  classification_state: "pending" | "provisional" | "final";
  classification_reasons: string[];
  tactical_tags: string[];
  principal_variation: string[];
  acceptable_alternatives: string[];
  phase: string;
  best_response: string;
  book_source: string;
  book_version: string;
  depth: number;
  nodes: number;
  time_ms: number;
  multipv: number;
  engine_version: string;
  classification_model_version: string;
}

export interface StoredGame {
  game: GameData;
  source_url: string;
  import_method: string;
  analysis_status: "pending" | "shallow" | "complete";
  analysis: {
    game_id: string;
    moves: MoveAssessment[];
    mistakes: Mistake[];
    eco: string;
    opening: string;
    book_ply: number;
    departure_ply: number | null;
    opening_book_version: string;
    accuracy: number;
    white_accuracy: number;
    black_accuracy: number;
    accuracy_sample_size: number;
    accuracy_version: string;
    /** Optional for reviews written before engine provenance was added. */
    requested_engine_profile?: string;
    actual_engine_profile?: string;
    engine_name?: string;
    engine_source?: string;
    engine_hash?: string;
  } | null;
  pgn?: string;
}

export interface VariationNode {
  id: number;
  parent_id: number;
  uci: string;
  san: string;
  fen: string;
  children: number[];
}

export interface Variation {
  id: string;
  game_id: string;
  root_ply: number;
  root_position: "before" | "after";
  root_fen: string;
  current_node_id: number;
  next_node_id: number;
  engine_configuration: string;
  updated_at_ms: number;
  nodes: VariationNode[];
}

export interface VariationAnalysis {
  best_move: string;
  ponder_move: string;
  lines: EngineLine[];
}

export interface ReviewAttempt {
  id: number;
  game_id: string;
  ply: number;
  uci: string;
  accepted: boolean;
  attempted_at_ms: number;
}

export interface DrillAttempt {
  id: number;
  attempted_at_ms: number;
  correct: boolean;
  move: string;
  response_time_ms: number;
  hint_level: number;
  retries: number;
}

export interface Drill {
  id: string;
  source_game_id: string;
  source_ply: number;
  fen: string;
  category: string;
  phase: string;
  explanation: string;
  punishment: string;
  solutions: string[];
  difficulty: number;
  impact_cp: number;
  attempts: DrillAttempt[];
  played_move: string;
  fen_after_mistake: string;
  fen_after_punishment: string;
  session_hint_level: number;
  session_started_at_ms: number;
  hint_level: number;
  available_hint_level: number;
  changed_threat: string;
  attacked_pieces: string[];
  opponent_response: string;
  source_type: "personal_game" | "public_corpus";
  provenance: string;
  corpus_version: string;
  validation_evidence: string[];
  schedule: {
    state: "new" | "due" | "upcoming" | "mastered";
    next_review_ms: number;
    success_rate: number;
    retention: number;
    priority: number;
  };
}

export interface Weakness {
  category: string;
  occurrences: number;
  games: number;
  attempts: number;
  correct: number;
  occurrences_7_days: number;
  occurrences_30_days: number;
  drill_accuracy: number;
  average_loss_cp: number;
  recurrence_rate: number;
  repeated_interval_days: number | null;
  phases: Record<string, number>;
}

export interface Profile {
  projection_version: string;
  player_name: string;
  latest_rating: number;
  rating_observations: number;
  games_imported: number;
  games_analyzed: number;
  games_shallow_analyzed: number;
  games_analyzed_7_days: number;
  games_analyzed_30_days: number;
  total_mistakes: number;
  total_positions: number;
  drill_attempts: number;
  drill_correct: number;
  retention_reviews: number;
  retained_reviews: number;
  analysis_completion_rate: number;
  drill_accuracy: number;
  retention_rate: number;
  average_centipawn_loss: number;
  weaknesses: Weakness[];
  openings: Array<{ eco: string; name: string; games: number; mistakes: number; average_centipawn_loss: number }>;
  activity_trend: Array<{ day_start_ms: number; games_analyzed: number; mistakes: number; drill_attempts: number; drill_correct: number }>;
  endgame_conversion: RateMetric;
  king_safety_violations: RateMetric;
  time_management_failures: RateMetric;
}

export interface RateMetric {
  numerator: number;
  denominator: number;
  rate: number | null;
  statistically_meaningful: boolean;
}

export interface ResourceRecommendation {
  id: string;
  title: string;
  kind: string;
  locator: string;
  phase: string;
  opening: string;
  evidence: string;
  completed: boolean;
}

export interface BatchProgress {
  id: string;
  discovered: number;
  imported: number;
  duplicates: number;
  failed: number;
  queued: number;
  completed: number;
  job_failures: number;
  remaining: number;
  paused: boolean;
  positions_analyzed: number;
  positions_remaining: number;
}

export interface Job {
  id: number;
  game_id: string;
  status: "queued" | "running" | "complete" | "failed" | "cancelled";
  progress: {
    stage: "parsing" | "shallow_scan" | "deep_analysis" | "complete";
    complete: number;
    total: number;
    message: string;
  };
  error: string;
}

export type AnalysisStage = Job["progress"]["stage"];
export type JobStatus = Job["status"];

/**
 * Runtime facts reported by the engine/job system. These are operational
 * counters, not a representation of private reasoning or hidden thought.
 */
export interface Diagnostics {
  engine_workers: number;
  engine_submitted: number;
  engine_completed: number;
  engine_failed: number;
  engine_retried: number;
  engine_rejected: number;
  engine_active: number;
  queued_interactive: number;
  queued_current_game: number;
  queued_historical: number;
  maximum_queue_latency_ms: number;
  job_workers: number;
  jobs_queued: number;
  job_queue_capacity: number;
  analysis_cache_hits: number;
  analysis_cache_misses: number;
  analysis_cache_evictions: number;
  analysis_cache_entries: number;
  analysis_cache_capacity: number;
}

export interface RuntimeSettings {
  bind_address: string;
  shallow_depth: number;
  deep_depth: number;
  top_mistakes: number;
  job_workers: number;
  job_queue_capacity: number;
  storage: string;
}

export type RecoveryAction =
  | "configure_profile"
  | "search_date_window"
  | "retry"
  | "paste_pgn"
  | "cancel_current"
  | (string & {});

export interface ApiFailure {
  error: string;
  code?: string;
  actions?: RecoveryAction[];
}

export interface ChessComProfile {
  username: string;
  normalized_username: string;
  selected_time_controls: string[];
  archive_cursor: string;
  last_successful_sync_ms: number;
  last_error: string;
}

export interface ChessComProfileResponse {
  connected: boolean;
  profile: ChessComProfile | null;
}

export type ImportResolutionStatus =
  | "queued"
  | "running"
  | "resolved"
  | "needs_recovery"
  | "cancelled";

export interface ImportResolution {
  id: string;
  status: ImportResolutionStatus;
  url: string;
  canonical_url: string;
  game_id: string;
  username: string;
  source: "" | "local_archive" | "profile_archive" | "public_page";
  imported_game_id: string;
  code: string;
  error: string;
  actions: RecoveryAction[];
  created_at_ms: number;
  updated_at_ms: number;
}

export interface ImportedGameResponse {
  status: "imported";
  duplicate: boolean;
  game_id: string;
}

export interface ResolvingImportResponse {
  status: "resolving";
  resolution_id: string;
  resolution: ImportResolution;
}

export type ImportGameResponse = ImportedGameResponse | ResolvingImportResponse;

export type IngestSyncStatus = "queued" | "running" | "succeeded" | "failed" | "cancelled";

export interface IngestSync {
  id: string;
  status: IngestSyncStatus;
  username: string;
  days: 7 | 30 | 90;
  max_months: number;
  cutoff_ms: number;
  upper_bound_ms: number;
  current_month: string;
  months_completed: number;
  games_indexed: number;
  code: string;
  error: string;
  created_at_ms: number;
  updated_at_ms: number;
}

export interface IngestSnapshot {
  resolutions: ImportResolution[];
  syncs: IngestSync[];
  queued_interactive: number;
  queued_historical: number;
  queue_capacity: number;
}

export type ProgressSocketMessage =
  | { type: "job_update"; job: Job }
  | { type: "jobs_snapshot"; jobs: Job[] }
  | { type: "ingest_update"; event: "resolution"; payload: ImportResolution }
  | { type: "ingest_update"; event: "sync"; payload: IngestSync }
  | { type: "ingest_update"; event: "profile"; payload: ChessComProfile }
  | { type: "ingest_snapshot"; ingest: IngestSnapshot };
