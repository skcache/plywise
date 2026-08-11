import type { MoveAssessment } from "./types";

const strongClassifications = new Set(["Book", "Good", "Excellent", "Best", "Great", "Brilliant"]);
const attentionClassifications = new Set(["Inaccuracy", "Mistake", "Miss", "Blunder"]);

export function isStrongMove(move?: MoveAssessment): boolean {
  return Boolean(move && strongClassifications.has(move.classification));
}

export function needsBetterMove(move?: MoveAssessment): boolean {
  return Boolean(move && attentionClassifications.has(move.classification) && !sameMove(move));
}

export function sameMove(move?: MoveAssessment): boolean {
  if (!move) return false;
  const played = normalizeMove(move.played_san || move.san);
  const best = normalizeMove(move.best_san);
  return Boolean(played && best && played === best) || Boolean(move.played_uci && move.best_uci && move.played_uci === move.best_uci);
}

export function humanMoveExplanation(move: MoveAssessment): string {
  switch (move.classification) {
    case "Book": return "A standard move from this opening.";
    case "Brilliant": return "A sharp move that finds the point of the position.";
    case "Great": return "A strong move that keeps the initiative.";
    case "Best": return "Exactly right. This keeps your position on track.";
    case "Excellent": return "A precise move that keeps your position together.";
    case "Good": return "A solid move that keeps your options open.";
    case "Inaccuracy": return "This gives your opponent a little more room to improve.";
    case "Mistake": return "This gives up an important part of your position.";
    case "Miss": return "A useful chance was available here.";
    case "Blunder": return "This lets your opponent take control of the position.";
    default: return "Analysis will explain this move when the review is ready.";
  }
}

export function betterMoveExplanation(_move: MoveAssessment): string {
  return "A stronger continuation that keeps the position together.";
}

function normalizeMove(value: string): string {
  return value
    .replace(/[+#?!]/g, "")
    .replace(/[0-9]+\.{1,3}/g, "")
    .replace(/\s+/g, "")
    .trim()
    .toLowerCase();
}
