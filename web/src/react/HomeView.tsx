import { homeContinueReview, homeFocus, homeRecentGames, homeWeek, inferPlayerName } from "../insights";
import type { Drill, Job, Profile, StoredGame } from "../types";
import { Icon } from "./Icon";

export function HomeView({
  games,
  jobs,
  profile,
  drills,
  refreshBusy,
  refreshMessage,
  onOpen,
  onRecent,
  onImport,
  onPractice,
}: {
  games: StoredGame[];
  jobs: Job[];
  profile: Profile | null;
  drills: Drill[];
  refreshBusy: boolean;
  refreshMessage: string;
  onOpen: (gameId: string, ply?: number) => void;
  onRecent: () => void;
  onImport: () => void;
  onPractice: (drill: Drill) => void;
}) {
  const review = homeContinueReview(games);
  const focus = homeFocus(profile, drills);
  const recent = homeRecentGames(games);
  const week = homeWeek(profile);
  const player = inferPlayerName(profile, games).toLowerCase();
  const trend = week.mistakeTrend;

  return <section className="soft-surface home-console">
    <h1 className="sr-only">Home</h1>
    <div className="home-primary">
      <article className="home-module continue-module">
        <div className="home-module-icon"><Icon name="analysis"/></div>
        <div className="home-module-copy">
          <span>Continue Review</span>
          {review ? <>
            <h2>{review.title}</h2>
            <p>{review.opening}</p>
            <small>{review.moments ? `${review.moments} key moment${review.moments === 1 ? "" : "s"} to revisit` : "Review is ready"}</small>
          </> : <>
            <h2>Your next review starts here.</h2>
            <p>Analyze a game to create a board-first review.</p>
          </>}
        </div>
        <button className="home-action" onClick={() => review ? onOpen(review.gameId, review.ply) : onRecent()}>
          {review ? "Continue" : "Choose a game"} <span aria-hidden="true">→</span>
        </button>
      </article>

      <article className="home-module focus-module">
        <div className="home-module-icon"><Icon name="star"/></div>
        <div className="home-module-copy">
          <span>Today’s Focus</span>
          {focus ? <>
            <h2>{focus.weakness.category}</h2>
            <p>Repeated in {focus.weakness.games} of {profile?.games_analyzed ?? games.length} analyzed games</p>
            <small>{focus.weakness.occurrences} evidence-backed occurrence{focus.weakness.occurrences === 1 ? "" : "s"}</small>
          </> : <>
            <h2>Build your player profile.</h2>
            <p>Recurring themes appear after several analyzed games.</p>
          </>}
        </div>
        {focus?.drills.length ? <button className="home-action" onClick={() => onPractice(focus.drills[0]!)}>
          Practice {focus.drills.length} position{focus.drills.length === 1 ? "" : "s"} <span aria-hidden="true">→</span>
        </button> : <button className="home-action" onClick={onRecent}>
          {focus ? "Review evidence" : "Analyze games"} <span aria-hidden="true">→</span>
        </button>}
      </article>
    </div>

    <section className="home-recent" aria-labelledby="home-recent-title">
      <header>
        <div><span>Local library</span><h2 id="home-recent-title">Recent Games</h2></div>
        <button onClick={onRecent}>View all <span aria-hidden="true">→</span></button>
      </header>
      <div className="home-game-list">
        {recent.map((stored) => {
          const tags = stored.game.tags;
          const white = tags.White ?? "White";
          const black = tags.Black ?? "Black";
          const opponent = player && white.toLowerCase() === player ? black
            : player && black.toLowerCase() === player ? white
            : `${white} vs. ${black}`;
          const job = jobs.filter((item) => item.game_id === stored.game.id).sort((left, right) => right.id - left.id)[0];
          const active = job?.status === "queued" || job?.status === "running";
          const status = active ? job.status === "running" ? "Analyzing" : "Queued"
            : stored.analysis_status === "complete" ? "Reviewed"
            : stored.analysis_status === "shallow" ? "Partial" : "Ready";
          return <button key={stored.game.id} className="home-game-row" onClick={() => onOpen(stored.game.id, reviewPly(stored))}>
            <span className="home-result">{resultLabel(tags.Result)}</span>
            <span className="home-opponent"><strong>{opponent}</strong><small>{stored.analysis?.opening || tags.TimeControl || "Imported game"}</small></span>
            <span className="home-date">{formatDate(tags)}</span>
            <span className={`home-status home-status-${status.toLowerCase()}`}>{status}</span>
            <span className="home-row-arrow" aria-hidden="true">→</span>
          </button>;
        })}
        {!recent.length && <div className="home-games-empty">
          <Icon name="recent"/>
          <p>No local games yet.</p>
          <button onClick={onImport}>Import your first game</button>
        </div>}
      </div>
    </section>

    <footer className="home-week">
      <strong>This week</strong>
      <span>{week.analyzed} analyzed</span>
      <i aria-hidden="true"/>
      <span>{week.practiced} practiced</span>
      <i aria-hidden="true"/>
      <span className={trend === null ? "" : trend <= 0 ? "trend-good" : "trend-alert"}>
        {trend === null ? "More history needed for a trend" : `Mistakes ${trend <= 0 ? "↓" : "↑"} ${Math.abs(trend)}%`}
      </span>
      {refreshBusy && <small role="status">{refreshMessage || "Refreshing local games…"}</small>}
    </footer>
  </section>;
}

function reviewPly(game: StoredGame): number {
  return Math.max(0, game.analysis?.moves.find((move) =>
    ["Inaccuracy", "Mistake", "Miss", "Blunder"].includes(move.classification),
  )?.ply ?? 0);
}

function resultLabel(result?: string): string {
  if (result === "1-0") return "1";
  if (result === "0-1") return "0";
  if (result === "1/2-1/2") return "½";
  return "•";
}

function formatDate(tags: Record<string, string>): string {
  const raw = tags.UTCDate || tags.Date || "";
  if (!raw || raw.includes("?")) return "Date unavailable";
  const parsed = new Date(`${raw.replaceAll(".", "-")}T00:00:00Z`);
  return Number.isNaN(parsed.getTime()) ? raw : parsed.toLocaleDateString(undefined, { month: "short", day: "numeric" });
}
