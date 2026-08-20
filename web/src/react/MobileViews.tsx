import { useState, type ReactNode } from "react";
import { browserEngineProfiles, type BrowserEngineProfile } from "../engine-profile";
import { buildExploreEntries, gameTimestamp, homeContinueReview, homeFocus, homeRecentGames, homeWeek, inferPlayerName, ratingDelta, ratingHistory, reviewArc, type ExploreEntry, type ExploreSection } from "../insights";
import type { Diagnostics, Drill, Job, Profile, RuntimeSettings, StoredGame } from "../types";
import { ChessBoard } from "./Board";
import { Icon } from "./Icon";

type Theme = "system" | "light" | "dark";

export function MobilePageHeader({ title, detail, action, onSettings, onBack }: {
  title: string;
  detail?: string;
  action?: ReactNode;
  onSettings?: () => void;
  onBack?: () => void;
}) {
  return <header className="mobile-page-header">
    <div className="mobile-page-heading">
      {onBack ? <button className="mobile-page-back" aria-label="Back" onClick={onBack}><Icon name="previous"/></button> : null}
      <div><h1>{title}</h1>{detail ? <p>{detail}</p> : null}</div>
    </div>
    <div className="mobile-page-actions">{action}{onSettings ? <button className="mobile-page-icon" aria-label="Open settings" onClick={onSettings}><Icon name="settings"/></button> : null}</div>
  </header>;
}

export function MobileHomeView({ games, profile, drills, refreshBusy, refreshMessage, onOpen, onRecent, onImport, onPractice, onRefresh, onSettings }: {
  games: StoredGame[];
  profile: Profile | null;
  drills: Drill[];
  refreshBusy: boolean;
  refreshMessage: string;
  onOpen: (id: string, ply?: number) => void;
  onRecent: () => void;
  onImport: () => void;
  onPractice: (drill: Drill) => void;
  onRefresh: () => void;
  onSettings: () => void;
}) {
  const continueReview = homeContinueReview(games);
  const focus = homeFocus(profile, drills);
  const recent = homeRecentGames(games, 3);
  const week = homeWeek(profile);
  return <div className="mobile-route-view mobile-home-view">
    <MobilePageHeader title={dayGreeting()} detail={refreshMessage || undefined} onSettings={onSettings} action={<button className="mobile-text-action" disabled={refreshBusy} onClick={onRefresh}>{refreshBusy ? "Refreshing…" : "Refresh"}</button>}/>
    <div className="mobile-page-content">
      <section className="mobile-home-next" aria-labelledby="mobile-home-next-title">
        <div className="mobile-section-heading"><div><span>Continue</span><h2 id="mobile-home-next-title">Your next review</h2></div></div>
        {continueReview ? <button className="mobile-recommendation" onClick={() => onOpen(continueReview.gameId, continueReview.ply)}>
          <div><strong>{continueReview.title}</strong><p>{continueReview.opening} · {continueReview.moments ? `${continueReview.moments} moment${continueReview.moments === 1 ? "" : "s"} to revisit` : "Continue this review"}</p></div><span aria-hidden="true">Review <b>›</b></span>
        </button> : <div className="mobile-empty-row"><div><strong>Start with a game</strong><p>Import a public link or PGN and choose Analyze when you’re ready.</p></div><button onClick={onImport}>Import</button></div>}
      </section>

      <section className="mobile-home-focus" aria-labelledby="mobile-home-focus-title">
        <div className="mobile-section-heading"><div><span>Today’s focus</span><h2 id="mobile-home-focus-title">One useful thing to work on</h2></div></div>
        {focus ? <div className="mobile-focus-card"><div><strong>{humanLabel(focus.weakness.category)}</strong><p>{focus.weakness.occurrences} recent moment{focus.weakness.occurrences === 1 ? "" : "s"} across {focus.weakness.games} game{focus.weakness.games === 1 ? "" : "s"}.</p></div>{focus.drills[0] ? <button onClick={() => onPractice(focus.drills[0])}>Practice <b>›</b></button> : <span className="mobile-muted-action">Review in your games</span>}</div> : <div className="mobile-empty-row"><div><strong>Your focus will appear here</strong><p>Analyze a few games and Plywise will surface a recurring pattern.</p></div></div>}
      </section>

      <section className="mobile-home-recent" aria-labelledby="mobile-home-recent-title">
        <div className="mobile-section-heading"><div><span>Recent</span><h2 id="mobile-home-recent-title">Latest games</h2></div><button className="mobile-inline-action" onClick={onRecent}>View all <b>›</b></button></div>
        {recent.length ? <div className="mobile-home-game-list">{recent.map((game) => <MobileGameRow key={game.game.id} game={game} onOpen={onOpen}/>)}</div> : <p className="mobile-empty-copy">Your latest games will appear here after you import one.</p>}
      </section>

      <section className="mobile-home-week" aria-labelledby="mobile-home-week-title">
        <div className="mobile-section-heading"><div><span>This week</span><h2 id="mobile-home-week-title">Your rhythm</h2></div></div>
        {week.analyzed || week.practiced ? <div className="mobile-week-metrics"><Metric value={week.analyzed} label="analyzed"/><Metric value={week.practiced} label="practiced"/><Metric value={week.mistakeTrend === null ? "—" : `${week.mistakeTrend > 0 ? "+" : ""}${week.mistakeTrend}%`} label="mistake trend"/></div> : <p className="mobile-empty-copy">Your weekly activity will appear after your first review.</p>}
      </section>
    </div>
  </div>;
}

export function MobileRecentView({ games, jobs, profile, selected, onSelect, onClear, onOpen, onAnalyze, onAnalyzeSelected, onImport, onSettings }: {
  games: StoredGame[];
  jobs: Job[];
  profile: Profile | null;
  selected: Set<string>;
  onSelect: (id: string) => void;
  onClear: () => void;
  onOpen: (id: string, ply?: number) => void;
  onAnalyze: (id: string) => void;
  onAnalyzeSelected: () => void;
  onImport: () => void;
  onSettings: () => void;
}) {
  const [selecting, setSelecting] = useState(false);
  const player = inferPlayerName(profile, games);
  const groups = groupRecentGames(games);
  const leaveSelection = () => { setSelecting(false); onClear(); };
  return <div className="mobile-route-view mobile-recent-view">
    <MobilePageHeader title="Recent games" detail={`${games.length} game${games.length === 1 ? "" : "s"}`} onSettings={onSettings} action={selecting ? <button className="mobile-text-action" onClick={leaveSelection}>Done</button> : <><button className="mobile-text-action" onClick={() => setSelecting(true)}>Select</button><button className="mobile-header-action" aria-label="Import game" onClick={onImport}><Icon name="import"/></button></>}/>
    <div className="mobile-page-content">
      {selecting && selected.size ? <div className="mobile-selection-bar"><span>{selected.size} selected</span><button onClick={onAnalyzeSelected}>Analyze selected</button></div> : null}
      {groups.length ? groups.map(([label, items]) => <section key={label} className="mobile-recent-group" aria-labelledby={`recent-${label}`}><h2 id={`recent-${label}`}>{label}</h2><div className="mobile-recent-list">{items.map((game) => <MobileRecentRow key={game.game.id} game={game} jobs={jobs} player={player} selecting={selecting} selected={selected.has(game.game.id)} onSelect={onSelect} onOpen={onOpen} onAnalyze={onAnalyze}/>)}</div></section>) : <div className="mobile-empty-state"><Icon name="import"/><h2>No games yet</h2><p>Import a public Chess.com game or PGN to start a review.</p><button onClick={onImport}>Import a game</button></div>}
    </div>
  </div>;
}

export function MobileExploreView({ games, profile, onSection, onOpen, onSettings }: {
  games: StoredGame[];
  profile: Profile | null;
  onSection: (section: ExploreSection) => void;
  onOpen: (id: string, ply?: number) => void;
  onSettings: () => void;
}) {
  const [tab, setTab] = useState<MobileExploreTab>("For You");
  const [selected, setSelected] = useState<ExploreEntry | null>(null);
  const entries = buildExploreEntries(games);
  const activeSection = tab === "Openings" ? "Openings" : tab === "Middlegame" ? "Middlegames" : tab === "Endgame" ? "Endgames" : null;
  const categoryEntries = activeSection ? entries.filter((entry) => entry.section === activeSection) : entries;
  const weaknesses = profile?.weaknesses.filter((item) => item.occurrences > 0).slice(0, 3) ?? [];
  const selectTab = (next: MobileExploreTab) => { setTab(next); setSelected(null); if (next === "Openings" || next === "Middlegame" || next === "Endgame") onSection(next === "Middlegame" ? "Middlegames" : next === "Endgame" ? "Endgames" : "Openings"); };
  if (selected) return <MobileExploreDetail entry={selected} onBack={() => setSelected(null)} onOpen={onOpen}/>;
  return <div className="mobile-route-view mobile-explore-view">
    <MobilePageHeader title="Explore" detail="Learn from your games." onSettings={onSettings}/>
    <div className="mobile-page-content">
      <nav className="mobile-explore-tabs" aria-label="Explore categories">{(["For You", "Openings", "Middlegame", "Endgame", "Patterns"] as MobileExploreTab[]).map((item) => <button key={item} className={tab === item ? "active" : ""} onClick={() => selectTab(item)}>{item}</button>)}</nav>
      {tab === "For You" ? <>
        {weaknesses.length ? <section className="mobile-explore-section"><div className="mobile-section-heading"><div><span>From your reviews</span><h2>Good places to start</h2></div></div><div className="mobile-lesson-list">{weaknesses.map((item) => <button key={item.category} className="mobile-lesson-row" onClick={() => setTab("Patterns")}><div><strong>{humanLabel(item.category)}</strong><p>{item.occurrences} moment{item.occurrences === 1 ? "" : "s"} across {item.games} game{item.games === 1 ? "" : "s"}.</p></div><span>Learn <b>›</b></span></button>)}</div></section> : null}
        {entries.slice(0, 3).map((entry) => <ExploreCard key={entry.id} entry={entry} onSelect={setSelected}/>) }
        {!weaknesses.length && !entries.length ? <ExploreEmpty/> : null}
      </> : <section className="mobile-explore-section"><div className="mobile-section-heading"><div><span>{tab}</span><h2>{tab === "Patterns" ? "Patterns to notice" : `Your ${tab.toLowerCase()} positions`}</h2></div></div>{tab === "Patterns" ? weaknesses.length ? <div className="mobile-lesson-list">{weaknesses.map((item) => <div key={item.category} className="mobile-lesson-row mobile-pattern-row"><div><strong>{humanLabel(item.category)}</strong><p>{item.occurrences} moment{item.occurrences === 1 ? "" : "s"} across {item.games} game{item.games === 1 ? "" : "s"}.</p></div><span>Keep noticing</span></div>)}</div> : <ExploreEmpty category="patterns"/> : categoryEntries.length ? <div className="mobile-lesson-list">{categoryEntries.map((entry) => <ExploreCard key={entry.id} entry={entry} onSelect={setSelected}/>)}</div> : <ExploreEmpty category={tab}/>}</section>}
    </div>
  </div>;
}

export function MobileProgressView({ games, profile, onOpen, onSettings }: { games: StoredGame[]; profile: Profile | null; onOpen: (id: string, ply?: number) => void; onSettings: () => void }) {
  const player = inferPlayerName(profile, games);
  const ratings = ratingHistory(games, player);
  const delta = ratingDelta(ratings);
  const latest = ratings.at(-1)?.rating ?? (profile?.latest_rating ? profile.latest_rating : null);
  const opportunity = profile?.weaknesses.filter((item) => item.occurrences > 0).sort((a, b) => b.games - a.games || b.occurrences - a.occurrences)[0];
  const improving = profile?.weaknesses.filter((item) => item.occurrences_30_days > 0 && item.occurrences_7_days < item.occurrences_30_days).sort((a, b) => (a.occurrences_7_days / a.occurrences_30_days) - (b.occurrences_7_days / b.occurrences_30_days))[0];
  const revisit = reviewArc(games).slice(0, 3);
  const analyzedThisMonth = profile?.games_analyzed_30_days ?? games.filter((game) => game.analysis && (gameTimestamp(game.game.tags) ?? 0) >= Date.now() - 30 * 86_400_000).length;
  const positionsReviewed = profile?.total_positions ?? games.reduce((sum, game) => sum + (game.analysis?.moves.length ?? 0), 0);
  return <div className="mobile-route-view mobile-progress-view">
    <MobilePageHeader title="Progress" detail="See what is changing in your game." onSettings={onSettings}/>
    <div className="mobile-page-content">
      <section className="mobile-progress-lead"><span>Current rating</span><strong>{latest ?? "—"}</strong><p>{delta === null ? "Keep importing rated games to see a trend." : `${delta >= 0 ? "+" : ""}${delta} in the last 30 days`}</p>{ratings.length > 1 ? <RatingChart ratings={ratings}/> : <div className="mobile-rating-empty"><strong>Rating trend</strong><p>Not enough history yet. Your trend will appear after Plywise has a few rated games to compare.</p></div>}</section>
      <section className="mobile-progress-section"><div className="mobile-section-heading"><div><span>This month</span><h2>Your review rhythm</h2></div></div><div className="mobile-progress-metrics"><Metric value={analyzedThisMonth} label="games analyzed"/><Metric value={positionsReviewed} label="positions reviewed"/><Metric value={profile?.drill_attempts ? `${Math.round(profile.drill_accuracy * 100)}%` : "—"} label="practice accuracy"/></div></section>
      <section className="mobile-progress-section"><div className="mobile-section-heading"><div><span>Your biggest opportunity</span><h2>{opportunity ? humanLabel(opportunity.category) : "Still learning"}</h2></div></div>{opportunity ? <div className="mobile-insight-copy"><p>{opportunity.occurrences} moment{opportunity.occurrences === 1 ? "" : "s"} across {opportunity.games} game{opportunity.games === 1 ? "" : "s"}. Review these positions to make the pattern easier to spot next time.</p></div> : <p className="mobile-empty-copy">Analyze a few games and Plywise will show the pattern that is costing you most often.</p>}</section>
      <section className="mobile-progress-section mobile-progress-split"><div><div className="mobile-section-heading"><div><span>Improving</span><h2>{improving ? humanLabel(improving.category) : "Your strengths will appear here"}</h2></div></div>{improving ? <p>{improving.occurrences_30_days - improving.occurrences_7_days} fewer recent moments than the last 30 days.</p> : <p>Keep reviewing games to see which patterns are fading.</p>}</div><div><div className="mobile-section-heading"><div><span>Needs work</span><h2>{opportunity ? humanLabel(opportunity.category) : "Not enough evidence yet"}</h2></div></div>{opportunity ? <p>Keep an eye on this pattern in your next review.</p> : <p>Your first few reviews will give this section something useful to say.</p>}</div></section>
      <section className="mobile-progress-section"><div className="mobile-section-heading"><div><span>Positions to revisit</span><h2>Keep the lesson close</h2></div></div>{revisit.length ? <div className="mobile-lesson-list">{revisit.map((item) => <button key={item.gameId} className="mobile-lesson-row" onClick={() => onOpen(item.gameId, item.largestSwingPly)}><div><strong>{item.title}</strong><p>{reviewLabel(games.find((game) => game.game.id === item.gameId), item.largestSwingPly)} · {item.opening}</p></div><span>Review <b>›</b></span></button>)}</div> : <p className="mobile-empty-copy">Analyzed positions will appear here when there is a useful lesson to revisit.</p>}</section>
    </div>
  </div>;
}

export function MobileSettingsView({ theme, onTheme, engineLinesDefault, onEngineLines, browserProfile, onBrowserProfile, accountLabel, onSignOut, onBack }: { theme: Theme; onTheme: (theme: Theme) => void; engineLinesDefault: boolean; onEngineLines: (value: boolean) => void; browserProfile: BrowserEngineProfile; onBrowserProfile: (profile: BrowserEngineProfile) => void; runtime?: RuntimeSettings | null; diagnostics?: Diagnostics | null; accountLabel: string; onSignOut: () => void; onBack: () => void }) {
  return <div className="mobile-route-view mobile-settings-view">
    <MobilePageHeader title="Settings" detail="How Plywise should behave." onBack={onBack}/>
    <div className="mobile-page-content">
      <section className="mobile-setting-section"><div className="mobile-section-heading"><div><span>Appearance</span><h2>Choose a workspace</h2></div></div><div className="mobile-setting-options" role="radiogroup" aria-label="Theme">{(["system", "light", "dark"] as Theme[]).map((item) => <label key={item} className={theme === item ? "active" : ""}><input type="radio" name="mobile-theme" checked={theme === item} onChange={() => onTheme(item)}/><span>{titleCase(item)}</span></label>)}</div></section>
      <section className="mobile-setting-section"><div className="mobile-section-heading"><div><span>Review</span><h2>What opens first</h2></div></div><label className="mobile-setting-toggle"><span><strong>Show the engine line first</strong><small>Keep the summary first unless you want technical detail up front.</small></span><input type="checkbox" checked={engineLinesDefault} onChange={(event) => onEngineLines(event.target.checked)}/></label></section>
      <section className="mobile-setting-section"><div className="mobile-section-heading"><div><span>Browser analysis</span><h2>Choose a pass</h2></div></div><div className="mobile-engine-options">{browserEngineProfiles().map((item) => <label key={item.id} className={browserProfile === item.id ? "active" : ""}><input type="radio" name="mobile-browser-profile" checked={browserProfile === item.id} onChange={() => onBrowserProfile(item.id)}/><span><strong>{item.label}</strong><small>{item.description}</small></span></label>)}</div></section>
      <section className="mobile-setting-section"><div className="mobile-section-heading"><div><span>Account</span><h2>{accountLabel}</h2></div></div><button className="mobile-signout" type="button" onClick={onSignOut}>Sign out</button></section>
    </div>
  </div>;
}

type MobileExploreTab = "For You" | "Openings" | "Middlegame" | "Endgame" | "Patterns";

function MobileExploreDetail({ entry, onBack, onOpen }: { entry: ExploreEntry; onBack: () => void; onOpen: (id: string, ply?: number) => void }) {
  return <div className="mobile-route-view mobile-explore-detail"><MobilePageHeader title={entry.title} detail="A position from your games." onBack={onBack}/><div className="mobile-page-content"><div className="mobile-detail-board"><ChessBoard fen={entry.fen} orientation="white" compact/></div><section className="mobile-detail-copy"><span>{entry.section.slice(0, -1)}</span><h2>{entry.title}</h2><p>{explorePurpose(entry)}</p><small>From an analyzed game</small><button onClick={() => onOpen(entry.gameId, entry.ply)}>Open in Analysis <b>›</b></button></section></div></div>;
}

function ExploreCard({ entry, onSelect }: { entry: ExploreEntry; onSelect: (entry: ExploreEntry) => void }) {
  return <button className="mobile-lesson-row" onClick={() => onSelect(entry)}><div><strong>{entry.title}</strong><p>{explorePurpose(entry)}</p></div><span>Learn <b>›</b></span></button>;
}

function ExploreEmpty({ category = "your games" }: { category?: string }) {
  return <div className="mobile-empty-state"><Icon name="book"/><h2>Nothing here yet</h2><p>Analyze a few {category === "For You" ? "games" : category.toLowerCase()} and Plywise will build this from real positions.</p></div>;
}

function MobileRecentRow({ game, jobs, player, selecting, selected, onSelect, onOpen, onAnalyze }: { game: StoredGame; jobs: Job[]; player: string; selecting: boolean; selected: boolean; onSelect: (id: string) => void; onOpen: (id: string, ply?: number) => void; onAnalyze: (id: string) => void }) {
  const info = gameInfo(game, player);
  const active = activeJob(game.game.id, jobs);
  const status = active ? "Working" : info.status;
  const action = game.analysis_status === "complete" ? "Review" : active ? "Working…" : "Analyze";
  return <article className={`mobile-recent-row ${selecting ? "selecting" : ""} ${selected ? "selected" : ""}`}>
    {selecting ? <label className="mobile-row-checkbox"><input type="checkbox" checked={selected} onChange={() => onSelect(game.game.id)}/><span aria-hidden="true"/></label> : null}
    <button className="mobile-recent-main" onClick={() => selecting ? onSelect(game.game.id) : onOpen(game.game.id, reviewPly(game))}>
      <strong>{info.opponent}</strong><span>{info.result} · {info.opening}</span><small>{info.timeControl} <i>·</i> {status}</small>
    </button>
    <button className="mobile-row-action" disabled={Boolean(active)} onClick={() => game.analysis_status === "complete" ? onOpen(game.game.id, reviewPly(game)) : onAnalyze(game.game.id)}>{action} <b>›</b></button>
  </article>;
}

function MobileGameRow({ game, onOpen }: { game: StoredGame; onOpen: (id: string, ply?: number) => void }) {
  const info = gameInfo(game, "");
  return <button className="mobile-home-game-row" onClick={() => onOpen(game.game.id, reviewPly(game))}><div><strong>{info.opponent}</strong><span>{info.result} · {info.opening}</span></div><b>›</b></button>;
}

function RatingChart({ ratings }: { ratings: Array<{ rating: number }> }) {
  const min = Math.min(...ratings.map((point) => point.rating));
  const max = Math.max(...ratings.map((point) => point.rating));
  const range = Math.max(1, max - min);
  const points = ratings.map((point, index) => `${ratings.length === 1 ? 50 : index / (ratings.length - 1) * 100},${42 - ((point.rating - min) / range) * 30}`).join(" ");
  return <svg className="mobile-rating-chart" viewBox="0 0 100 48" preserveAspectRatio="none" role="img" aria-label="Rating trend"><path d="M0 42H100"/><polyline points={points}/></svg>;
}

function Metric({ value, label }: { value: string | number; label: string }) {
  return <div className="mobile-metric"><strong>{value}</strong><span>{label}</span></div>;
}

function groupRecentGames(games: StoredGame[]): Array<[string, StoredGame[]]> {
  const todayStart = new Date(); todayStart.setHours(0, 0, 0, 0);
  const sorted = [...games].sort((left, right) => (gameTimestamp(right.game.tags) ?? 0) - (gameTimestamp(left.game.tags) ?? 0));
  const today = sorted.filter((game) => { const timestamp = gameTimestamp(game.game.tags); return timestamp !== null && timestamp >= todayStart.getTime(); });
  const earlier = sorted.filter((game) => !today.includes(game));
  return ([ ["Today", today], ["Earlier", earlier] ] as Array<[string, StoredGame[]]>).filter(([, items]) => items.length);
}

function gameInfo(game: StoredGame, player: string) {
  const tags = game.game.tags;
  const normalized = player.toLowerCase();
  const white = tags.White ?? "White";
  const black = tags.Black ?? "Black";
  const isWhite = normalized && white.toLowerCase() === normalized;
  const isBlack = normalized && black.toLowerCase() === normalized;
  const opponent = isWhite ? black : isBlack ? white : `${white} vs. ${black}`;
  const result = resultLabel(tags.Result, Boolean(isWhite), Boolean(isBlack));
  const opening = game.analysis?.opening || "Opening unknown";
  const timeControl = formatTimeControl(tags.TimeControl);
  return { opponent, result, opening, timeControl, status: game.analysis_status === "complete" ? "Reviewed" : "Ready" };
}

function activeJob(gameId: string, jobs: Job[]): Job | undefined {
  return jobs.filter((job) => job.game_id === gameId && (job.status === "queued" || job.status === "running")).sort((a, b) => b.id - a.id)[0];
}

function resultLabel(result: string | undefined, isWhite: boolean, isBlack: boolean) {
  if (result === "1/2-1/2") return "Draw";
  if (result === "1-0") return isBlack ? "Loss" : isWhite ? "Win" : "White won";
  if (result === "0-1") return isWhite ? "Loss" : isBlack ? "Win" : "Black won";
  return "Result pending";
}

function formatTimeControl(value?: string) {
  if (!value || value === "*") return "Time control unknown";
  const [base, increment] = value.split("+");
  if (!base || !/^\d+$/.test(base)) return value;
  const minutes = Math.floor(Number(base) / 60);
  return increment ? `${minutes || 1}+${increment}` : `${minutes || 1} min`;
}

function reviewPly(game: StoredGame) {
  return game.analysis?.moves.find((move) => ["Inaccuracy", "Mistake", "Miss", "Blunder"].includes(move.classification))?.ply ?? 0;
}

function reviewLabel(game: StoredGame | undefined, ply: number) {
  const move = game?.analysis?.moves.find((item) => item.ply === ply);
  return move ? `${move.classification} · move ${move.move_number}` : `Review · move ${Math.floor(ply / 2) + 1}`;
}

function explorePurpose(entry: ExploreEntry) {
  if (entry.section === "Openings") return "A plan that appeared in one of your games.";
  if (entry.section === "Endgames") return "A position where king activity and pawn plans matter.";
  return `A pattern to notice when ${entry.title.toLowerCase()} appears again.`;
}

function humanLabel(value: string) {
  return value.replaceAll("_", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function titleCase(value: string) {
  return value.replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function dayGreeting() {
  const hour = new Date().getHours();
  return hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
}
