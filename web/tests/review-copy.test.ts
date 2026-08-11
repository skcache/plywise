import type { MoveAssessment } from "../src/types";
import { humanMoveExplanation, needsBetterMove, sameMove } from "../src/review-copy";

function assert(actual: unknown, expected: unknown, label: string): void {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

const baseMove = (overrides: Partial<MoveAssessment> = {}) => ({
  ply: 0,
  move_number: 1,
  side: "white",
  played_uci: "e2e4",
  played_san: "e4",
  san: "e4",
  best_uci: "e2e4",
  best_san: "e4",
  classification: "Book",
  classification_reasons: [],
  expected_points_loss: 0,
  ...overrides,
} as MoveAssessment);

const book = baseMove();
assert(sameMove(book), true, "book move matches recommendation");
assert(needsBetterMove(book), false, "book move has no better section");
assert(humanMoveExplanation(book), "A standard move from this opening.", "book copy");

const best = baseMove({ classification: "Best", played_san: "Nf3", best_san: "Nf3", played_uci: "g1f3", best_uci: "g1f3" });
assert(needsBetterMove(best), false, "strong move has no better section");
assert(humanMoveExplanation(best).includes("Exactly right"), true, "strong move copy");

const mistake = baseMove({ classification: "Mistake", played_san: "a3", best_san: "Nf3", played_uci: "a2a3", best_uci: "g1f3" });
assert(sameMove(mistake), false, "mistake differs from recommendation");
assert(needsBetterMove(mistake), true, "mistake keeps a better path");
assert(humanMoveExplanation(mistake).includes("gives up"), true, "mistake copy");

console.log("review copy tests passed");
