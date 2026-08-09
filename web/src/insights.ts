import type { Drill, Profile, StoredGame, Weakness } from "./types";

export type ExploreSection = "Openings" | "Middlegames" | "Endgames";

export interface ExploreEntry {
  id: string;
  section: ExploreSection;
  title: string;
  purpose: string;
  source: string;
  fen: string;
  gameId: string;
  ply: number;
  tags: string[];
}

export interface RatingPoint {
  timestamp: number;
  rating: number;
  gameId: string;
  result: string;
  opponent: string;
  timeControl: string;
}

export interface ReviewArcEntry {
  gameId: string;
  timestamp: number | null;
  title: string;
  result: string;
  opening: string;
  largestSwing: number;
  largestSwingPly: number;
}

export interface HomeReview {
  gameId: string;
  ply: number;
  title: string;
  opening: string;
  moments: number;
}

export interface HomeFocus {
  weakness: Weakness;
  drills: Drill[];
}

export interface HomeWeek {
  analyzed: number;
  practiced: number;
  mistakeTrend: number | null;
}

const motifPurpose: Record<string, string> = {
  capture: "Track exchanges by asking what remains attacked after the capture.",
  recapture: "Compare automatic recapture with forcing alternatives first.",
  check: "Use checks to control move order, not merely because they are forcing.",
  pin: "Count whether the pinned piece is truly unable to move and what attacks the pinning piece.",
  fork: "Look for one move that attacks two targets of unequal defensive priority.",
  skewer: "Force the more valuable piece to move before collecting the piece behind it.",
  development: "Develop with purpose: improve a piece while contesting a central square.",
  material_sacrifice: "Measure the initiative and concrete continuation before giving material.",
  missed_opportunity: "Recheck forcing moves after the opponent changes the position.",
};

export function buildExploreEntries(games: StoredGame[]): ExploreEntry[] {
  const result: ExploreEntry[] = [];
  const seen = new Set<string>();
  for (const stored of games) {
    const analysis = stored.analysis;
    if (!analysis?.moves.length) continue;
    const openingKey = `opening:${analysis.eco}:${analysis.opening}`;
    if (!seen.has(openingKey)) {
      seen.add(openingKey);
      const departure = analysis.departure_ply ?? Math.max(0, analysis.book_ply - 1);
      const move = analysis.moves[Math.min(departure, analysis.moves.length - 1)] ?? analysis.moves[0];
      result.push({
        id: openingKey,
        section: "Openings",
        title: [analysis.eco, analysis.opening].filter(Boolean).join(" · ") || "Unclassified opening",
        purpose: analysis.departure_ply === null
          ? "Review the plans that kept this game inside the local opening reference."
          : `Study the position where the game left known play on move ${Math.floor(analysis.departure_ply / 2) + 1}.`,
        source: `Local opening book ${analysis.opening_book_version || "unversioned"} · your analyzed game`,
        fen: move.fen_before,
        gameId: stored.game.id,
        ply: move.ply,
        tags: ["opening", analysis.eco].filter(Boolean),
      });
    }
    for (const move of analysis.moves) {
      if (move.phase === "middlegame") {
        for (const rawTag of move.tactical_tags) {
          const tag = rawTag.toLowerCase();
          const key = `middle:${tag}`;
          if (seen.has(key)) continue;
          seen.add(key);
          result.push({
            id: key,
            section: "Middlegames",
            title: titleCase(tag.replaceAll("_", " ")),
            purpose: motifPurpose[tag] ?? "Reconstruct the candidate moves and compare the engine-backed continuation.",
            source: `Tutor Classification Model 1 tag · ${gameTitle(stored)}`,
            fen: move.fen_before,
            gameId: stored.game.id,
            ply: move.ply,
            tags: [tag, move.classification],
          });
        }
      }
      if (move.phase === "endgame") {
        const material = materialSignature(move.fen_before);
        const key = `endgame:${material}`;
        if (seen.has(key)) continue;
        seen.add(key);
        result.push({
          id: key,
          section: "Endgames",
          title: `${material} endgame`,
          purpose: "Use king activity, passed pawns, and forcing exchanges to turn evaluation into a plan.",
          source: `Position from ${gameTitle(stored)} · local Stockfish review`,
          fen: move.fen_before,
          gameId: stored.game.id,
          ply: move.ply,
          tags: ["endgame", move.classification],
        });
      }
    }
  }
  return result;
}

export function ratingHistory(games: StoredGame[], playerName: string): RatingPoint[] {
  const normalized = playerName.trim().toLowerCase();
  if (!normalized) return [];
  return games.flatMap((stored): RatingPoint[] => {
    const tags = stored.game.tags;
    const white = (tags.White ?? "").toLowerCase();
    const black = (tags.Black ?? "").toLowerCase();
    const isWhite = white === normalized;
    if (!isWhite && black !== normalized) return [];
    const rating = Number(isWhite ? tags.WhiteElo : tags.BlackElo);
    const timestamp = gameTimestamp(tags);
    if (!Number.isFinite(rating) || rating <= 0 || timestamp === null) return [];
    return [{
      timestamp,
      rating,
      gameId: stored.game.id,
      result: tags.Result ?? "*",
      opponent: isWhite ? tags.Black ?? "Unknown" : tags.White ?? "Unknown",
      timeControl: tags.TimeControl ?? "Unspecified time control",
    }];
  }).sort((left, right) => left.timestamp - right.timestamp);
}

export function ratingDelta(points: RatingPoint[], days = 30): number | null {
  if (points.length < 2) return null;
  const latest = points[points.length - 1];
  if (!latest) return null;
  const cutoff = latest.timestamp - days * 86_400_000;
  const baseline = points.find((point) => point.timestamp >= cutoff);
  return baseline && baseline !== latest ? latest.rating - baseline.rating : null;
}

export function reviewArc(games: StoredGame[]): ReviewArcEntry[] {
  return games.flatMap((stored): ReviewArcEntry[] => {
    const moves = stored.analysis?.moves ?? [];
    if (!moves.length) return [];
    let largest = moves[0];
    for (const move of moves) if ((move?.expected_points_loss ?? 0) > (largest?.expected_points_loss ?? 0)) largest = move;
    return [{
      gameId: stored.game.id,
      timestamp: gameTimestamp(stored.game.tags),
      title: gameTitle(stored),
      result: stored.game.tags.Result ?? "*",
      opening: stored.analysis?.opening || "Unclassified opening",
      largestSwing: largest?.expected_points_loss ?? 0,
      largestSwingPly: largest?.ply ?? 0,
    }];
  }).sort((left, right) => (right.timestamp ?? 0) - (left.timestamp ?? 0));
}

export function homeRecentGames(games: StoredGame[], limit = 3): StoredGame[] {
  return [...games]
    .sort((left, right) => (gameTimestamp(right.game.tags) ?? 0) - (gameTimestamp(left.game.tags) ?? 0))
    .slice(0, limit);
}

export function homeContinueReview(games: StoredGame[]): HomeReview | null {
  const analyzed = homeRecentGames(games.filter((game) => game.analysis?.moves.length), games.length);
  const latest = analyzed.find((game) => game.analysis?.moves.some((move) =>
    ["Inaccuracy", "Mistake", "Miss", "Blunder"].includes(move.classification),
  )) ?? analyzed[0];
  if (!latest?.analysis) return null;
  const attention = latest.analysis.moves.filter((move) =>
    ["Inaccuracy", "Mistake", "Miss", "Blunder"].includes(move.classification),
  );
  const first = attention[0] ?? latest.analysis.moves[0];
  if (!first) return null;
  return {
    gameId: latest.game.id,
    ply: first.ply,
    title: gameTitle(latest),
    opening: latest.analysis.opening || "Unclassified opening",
    moments: attention.length,
  };
}

export function homeFocus(profile: Profile | null, drills: Drill[]): HomeFocus | null {
  const weakness = profile?.weaknesses
    .filter((item) => item.occurrences > 0)
    .sort((left, right) =>
      right.games - left.games
      || right.occurrences - left.occurrences
      || right.average_loss_cp - left.average_loss_cp,
    )[0];
  if (!weakness) return null;
  const normalized = weakness.category.trim().toLowerCase();
  const matching = drills
    .filter((drill) => drill.category.trim().toLowerCase() === normalized)
    .sort((left, right) => right.schedule.priority - left.schedule.priority)
    .slice(0, 3);
  return { weakness, drills: matching };
}

export function homeWeek(profile: Profile | null, now = Date.now()): HomeWeek {
  if (!profile) return { analyzed: 0, practiced: 0, mistakeTrend: null };
  const day = 86_400_000;
  const currentStart = now - 7 * day;
  const previousStart = now - 14 * day;
  const current = profile.activity_trend.filter((item) => item.day_start_ms >= currentStart && item.day_start_ms <= now);
  const previous = profile.activity_trend.filter((item) => item.day_start_ms >= previousStart && item.day_start_ms < currentStart);
  const analyzed = current.reduce((sum, item) => sum + item.games_analyzed, 0);
  const practiced = current.reduce((sum, item) => sum + item.drill_attempts, 0);
  const currentMistakes = current.reduce((sum, item) => sum + item.mistakes, 0);
  const previousMistakes = previous.reduce((sum, item) => sum + item.mistakes, 0);
  const mistakeTrend = previousMistakes > 0
    ? Math.round((currentMistakes - previousMistakes) / previousMistakes * 100)
    : null;
  return { analyzed, practiced, mistakeTrend };
}

export function inferPlayerName(profile: Profile | null, _games: StoredGame[]): string {
  return profile?.player_name ?? "";
}

export function gameTimestamp(tags: Record<string, string>): number | null {
  const raw = tags.UTCDate || tags.Date;
  if (!raw || !/^\d{4}\.\d{2}\.\d{2}$/.test(raw)) return null;
  const time = /^\d{2}:\d{2}:\d{2}$/.test(tags.UTCTime ?? "") ? tags.UTCTime : "00:00:00";
  const timestamp = Date.parse(`${raw.replaceAll(".", "-")}T${time}Z`);
  return Number.isFinite(timestamp) ? timestamp : null;
}

function gameTitle(stored: StoredGame): string {
  return `${stored.game.tags.White ?? "White"} vs. ${stored.game.tags.Black ?? "Black"}`;
}

function materialSignature(fen: string): string {
  const placement = fen.split(" ")[0]?.toLowerCase() ?? "";
  const counts = { q: 0, r: 0, b: 0, n: 0 };
  for (const piece of placement) if (piece in counts) counts[piece as keyof typeof counts] += 1;
  if (counts.q) return "Queen";
  if (counts.r) return "Rook";
  if (counts.b && counts.n) return "Minor-piece";
  if (counts.b) return "Bishop";
  if (counts.n) return "Knight";
  return "Pawn";
}

function titleCase(value: string): string {
  return value.replace(/\b\w/g, (letter) => letter.toUpperCase());
}
