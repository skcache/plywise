import {
  buildExploreEntries,
  homeContinueReview,
  homeFocus,
  homeRecentGames,
  homeWeek,
  inferPlayerName,
  ratingDelta,
  ratingHistory,
  reviewArc,
} from "../src/insights";
import type { Drill, MoveAssessment, Profile, StoredGame } from "../src/types";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

const move = (ply: number, phase: string, tags: string[], loss = 0): MoveAssessment => ({
  ply, move_number: Math.floor(ply / 2) + 1, side: ply % 2 ? "black" : "white", played_uci: "e2e4", played_san: "e4", san: "e4",
  fen_before: "8/8/8/8/8/8/4P3/4K2k w - - 0 1", fen_after: "8/8/8/8/4P3/8/8/4K2k b - - 0 1", best_uci: "e2e4", best_san: "e4",
  evaluation_before: 0, evaluation_after: 0, evaluation_after_best: 0, loss: 0, expected_points_before: .5, expected_points_after: .5,
  expected_points_loss: loss, quality: "Good", classification: loss > .2 ? "Blunder" : "Good", classification_state: "final",
  classification_reasons: [], tactical_tags: tags, principal_variation: [], acceptable_alternatives: [], phase, best_response: "", book_source: "", book_version: "",
  depth: 18, nodes: 1, time_ms: 1, multipv: 1, engine_version: "test", classification_model_version: "test",
});

const stored = (date: string, rating: string, moves: MoveAssessment[]): StoredGame => ({
  game: { id: date, tags: { White: "Alex", Black: "Rival", WhiteElo: rating, BlackElo: "1400", UTCDate: date, Result: "1-0" }, plies: [] },
  source_url: "", import_method: "manual", analysis_status: "complete",
  analysis: { game_id: date, moves, mistakes: [], eco: "C20", opening: "King's Pawn Game", book_ply: 2, departure_ply: 2, opening_book_version: "2026.1", accuracy: 92, white_accuracy: 94, black_accuracy: 90, accuracy_sample_size: moves.length, accuracy_version: "tutor-expected-points-squared-v1" },
});

const games = [stored("2026.07.01", "1400", [move(0, "opening", []), move(1, "middlegame", ["fork"]), move(2, "endgame", [], .3)]), stored("2026.07.15", "1432", [move(0, "opening", [])])];
assert(ratingHistory(games, "Alex").map((point) => point.rating), [1400, 1432], "rating evidence order");
assert(ratingDelta(ratingHistory(games, "Alex")), 32, "thirty-day delta");
assert(buildExploreEntries(games).map((entry) => entry.section), ["Openings", "Middlegames", "Endgames"], "all Explore sections have evidence");
assert(reviewArc(games)[1]?.largestSwing, .3, "review arc uses largest expected-points swing");
assert(homeRecentGames(games).map((game) => game.game.id), ["2026.07.15", "2026.07.01"], "home recency is evidence ordered");
assert(homeContinueReview(games)?.moments, 1, "home review counts attention-worthy moves");
assert(inferPlayerName(null, games), "", "untagged profile does not infer player identity");

const weakness = {
  category: "Piece safety", occurrences: 5, games: 3, attempts: 0, correct: 0, occurrences_7_days: 2, occurrences_30_days: 5,
  drill_accuracy: 0, average_loss_cp: 180, recurrence_rate: .6, repeated_interval_days: 3, phases: { middlegame: 5 },
};
const drill = {
  id: "piece-safety-1", source_game_id: games[0]!.game.id, source_ply: 2, fen: move(2, "middlegame", []).fen_before,
  category: "Piece safety", phase: "middlegame", explanation: "", punishment: "", solutions: ["e2e4"], difficulty: 1,
  impact_cp: 180, attempts: [], played_move: "", fen_after_mistake: "", fen_after_punishment: "", session_hint_level: 0,
  session_started_at_ms: 0, hint_level: 0, available_hint_level: 0, changed_threat: "", attacked_pieces: [],
  opponent_response: "", source_type: "personal_game", provenance: "", corpus_version: "", validation_evidence: [],
  schedule: { state: "due", next_review_ms: 0, success_rate: 0, retention: 0, priority: 2 },
} satisfies Drill;
const profile = {
  projection_version: "test", player_name: "Alex", latest_rating: 1432, rating_observations: 2, games_imported: 2,
  games_analyzed: 2, games_shallow_analyzed: 2, games_analyzed_7_days: 1, games_analyzed_30_days: 2, total_mistakes: 5,
  total_positions: 4, drill_attempts: 4, drill_correct: 3, retention_reviews: 0, retained_reviews: 0,
  analysis_completion_rate: 1, drill_accuracy: .75, retention_rate: 0, average_centipawn_loss: 20, weaknesses: [weakness],
  openings: [], activity_trend: [
    { day_start_ms: 10 * 86_400_000, games_analyzed: 2, mistakes: 2, drill_attempts: 4, drill_correct: 3 },
    { day_start_ms: 3 * 86_400_000, games_analyzed: 1, mistakes: 4, drill_attempts: 1, drill_correct: 0 },
  ],
  endgame_conversion: { numerator: 0, denominator: 0, rate: null, statistically_meaningful: false },
  king_safety_violations: { numerator: 0, denominator: 0, rate: null, statistically_meaningful: false },
  time_management_failures: { numerator: 0, denominator: 0, rate: null, statistically_meaningful: false },
} satisfies Profile;
assert(homeFocus(profile, [drill])?.drills.length, 1, "home focus joins profile evidence to persisted drills");
assert(homeWeek(profile, 14 * 86_400_000), { analyzed: 2, practiced: 4, mistakeTrend: -50 }, "home week uses current and previous evidence windows");

console.log("insights tests passed");
