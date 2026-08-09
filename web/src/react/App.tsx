import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  ApiError,
  analyzeVariation,
  cancelJob,
  claimGuestReview,
  createVariation,
  createGuestSession,
  deleteVariation,
  extendVariation,
  finalizeBrowserAnalysis,
  importGameObservable,
  loadChessComProfile,
  loadChessComSync,
  listGames,
  loadDrills,
  listVariations,
  loadDiagnostics,
  loadGame,
  loadImportResolution,
  loadJobs,
  loadProfile,
  loadRuntimeSettings,
  resetVariation,
  savePlayerIdentity,
  setVariationCursor,
  startChessComSync,
  startBrowserAnalysis,
  startAnalysis,
  submitBrowserObservation,
  submitReviewAttempt,
} from "../api";
import { buildExploreEntries, inferPlayerName, ratingDelta, ratingHistory, reviewArc, type ExploreSection } from "../insights";
import { clearGuestSession, loadGuestSession, saveGuestSession } from "../guest-session";
import { bindGuestSession, loadGuestTrial, markGuestAnalysisUsed, releaseGuestAnalysis, reserveGuestAnalysis, type GuestTrialState } from "../guest-trial";
import { browserEngineProfiles, normalizeBrowserEngineProfile, type BrowserEngineProfile } from "../engine-profile";
import { BrowserEngineError, createBrowserEngine } from "../browser-engine";
import { runGuestBrowserReview, type GuestBrowserReviewProgress } from "../guest-browser-review";
import { authProviderLabel, clearAuthIntent, consumeAuthRedirectMessage, currentAuthSnapshot, initializeAuth, loadAuthIntent, saveAuthIntent, signInWithProvider, signOut, subscribeAuth, type AuthProvider, type AuthSnapshot } from "../auth-session";
import { autoplayDelay, blockingClassifications, completePlaybackDwell, isPlaying, pauseForSelectedMove, startPlayback, type ReviewMode } from "../review";
import type { BoardOrientation } from "../chess";
import type { Diagnostics, Drill, Job, MoveAssessment, PlayerIdentity, Profile, ProgressSocketMessage, RuntimeSettings, StoredGame, Variation, VariationAnalysis } from "../types";
import { eventProtocols, eventUrl } from "../config/runtime";
import { ChessBoard, EvaluationBar, formatEval } from "./Board";
import { Icon } from "./Icon";
import { HomeView } from "./HomeView";
import { LandingView } from "./LandingView";
import { AppShell, SoftButton, TopBar, type Route } from "./Shell";
import { AccountPrompt } from "./AccountPrompt";

type Theme = "system" | "light" | "dark";
type InspectorTab = "summary" | "line" | "method";
type IdentityPromptState = { gameId: string; names: string[]; source: PlayerIdentity["source"] };

const initialFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const routes = new Set<Route>(["landing", "home", "recent", "analysis", "explore", "progress", "settings"]);

function routeFromHash(): Route {
  const candidate = window.location.hash.replace(/^#\/?/, "") as Route;
  return candidate && routes.has(candidate) ? candidate : "landing";
}

export default function App() {
  const [route, setRoute] = useState<Route>(routeFromHash);
  const [games, setGames] = useState<StoredGame[]>([]);
  const [jobs, setJobs] = useState<Job[]>([]);
  const [profile, setProfile] = useState<Profile | null>(null);
  const [drills, setDrills] = useState<Drill[]>([]);
  const [chessComConnected, setChessComConnected] = useState<boolean | null>(null);
  const [diagnostics, setDiagnostics] = useState<Diagnostics | null>(null);
  const [runtimeSettings, setRuntimeSettings] = useState<RuntimeSettings | null>(null);
  const [selectedGameId, setSelectedGameId] = useState("");
  const [selectedPly, setSelectedPly] = useState(0);
  const [selectedGames, setSelectedGames] = useState<Set<string>>(new Set());
  const [orientation, setOrientation] = useState<BoardOrientation>("white");
  const [reviewMode, setReviewMode] = useState<ReviewMode>("manual");
  const [highlightedUci, setHighlightedUci] = useState("");
  const [trySource, setTrySource] = useState("");
  const [tryMessage, setTryMessage] = useState("");
  const [variation, setVariation] = useState<Variation | null>(null);
  const [variationMessage, setVariationMessage] = useState("");
  const [variationAnalysis, setVariationAnalysis] = useState<VariationAnalysis | null>(null);
  const [variationBusy, setVariationBusy] = useState(false);
  const [variationReturn, setVariationReturn] = useState<{ mode: ReviewMode; highlight: string }>({ mode: "manual", highlight: "" });
  const [moveListExpanded, setMoveListExpanded] = useState(false);
  const [overviewOpen, setOverviewOpen] = useState(false);
  const [moreOpen, setMoreOpen] = useState(false);
  const [inspectorTab, setInspectorTab] = useState<InspectorTab>("summary");
  const [exploreSection, setExploreSection] = useState<ExploreSection>("Openings");
  const [selectedExploreId, setSelectedExploreId] = useState("");
  const [theme, setTheme] = useState<Theme>(() => (localStorage.getItem("pct-theme") as Theme | null) ?? "system");
  const [engineLinesDefault, setEngineLinesDefault] = useState(() => localStorage.getItem("pct-engine-lines-default") === "true");
  const [browserProfile, setBrowserProfile] = useState<BrowserEngineProfile>(() => normalizeBrowserEngineProfile(localStorage.getItem("pct-browser-engine-profile")));
  const [importOpen, setImportOpen] = useState(false);
  const [importBusy, setImportBusy] = useState(false);
  const [importStage, setImportStage] = useState("");
  const [identityPrompt, setIdentityPrompt] = useState<IdentityPromptState | null>(null);
  const [identityBusy, setIdentityBusy] = useState(false);
  const [identityError, setIdentityError] = useState("");
  const [error, setError] = useState("");
  const [refreshBusy, setRefreshBusy] = useState(false);
  const [refreshMessage, setRefreshMessage] = useState("");
  const [accountPromptContext, setAccountPromptContext] = useState<"landing" | "save">("landing");
  const [accountPromptOpen, setAccountPromptOpen] = useState(false);
  const [authSnapshot, setAuthSnapshot] = useState<AuthSnapshot>(currentAuthSnapshot);
  const [authMessage, setAuthMessage] = useState("");
  const [authBusy, setAuthBusy] = useState<AuthProvider | null>(null);
  const [authRevision, setAuthRevision] = useState(0);
  const [guestTrial, setGuestTrial] = useState<GuestTrialState>(() => loadGuestTrial());
  const [guestMessage, setGuestMessage] = useState("");
  const [browserAnalysisProgress, setBrowserAnalysisProgress] = useState<GuestBrowserReviewProgress | null>(null);
  const autoplayTimer = useRef<number | null>(null);
  const browserEngineRef = useRef<ReturnType<typeof createBrowserEngine> | null>(null);
  const browserAnalysisAbort = useRef<AbortController | null>(null);
  const pendingClaim = useRef<{ guestToken: string; idempotencyKey: string } | null>(null);
  const pendingAccountContext = useRef<"landing" | "save">("landing");
  const accountFlowRequested = useRef(false);
  const claimInFlight = useRef(false);

  const selectedGame = useMemo(() => games.find((game) => game.game.id === selectedGameId) ?? null, [games, selectedGameId]);
  const selectedMove = selectedGame?.analysis?.moves[selectedPly];
  const selectedJob = useMemo(() => jobs.filter((job) => job.game_id === selectedGameId).sort((a, b) => b.id - a.id)[0] ?? null, [jobs, selectedGameId]);

  const refreshGame = useCallback(async (gameId: string) => {
    const game = await loadGame(gameId);
    setGames((current) => [game, ...current.filter((item) => item.game.id !== gameId)]);
    return game;
  }, []);

  const refreshRuntime = useCallback(async () => {
    try {
      const [nextDiagnostics, nextSettings] = await Promise.all([loadDiagnostics(), loadRuntimeSettings()]);
      setDiagnostics(nextDiagnostics);
      setRuntimeSettings(nextSettings);
    } catch {
      // Runtime disclosure is optional; review data remains primary.
    }
  }, []);

  const refreshAccountLibrary = useCallback(async () => {
    const [listed, jobState, nextProfile, nextDrills, chessCom] = await Promise.all([
      listGames(),
      loadJobs(),
      loadProfile().catch(() => null),
      loadDrills().catch(() => []),
      loadChessComProfile().catch(() => null),
    ]);
    const loaded = await Promise.all(listed.map((game) => loadGame(game.game.id)));
    setGames(loaded);
    setJobs(jobState.jobs);
    setProfile(nextProfile);
    setDrills(nextDrills);
    setChessComConnected(chessCom?.connected ?? false);
    setSelectedGameId((current) => current && loaded.some((game) => game.game.id === current)
      ? current
      : loaded.find((game) => game.analysis_status === "complete")?.game.id ?? loaded[0]?.game.id ?? "");
    await refreshRuntime();
  }, [refreshRuntime]);

  const claimPendingReview = useCallback(async (): Promise<boolean> => {
    const pending = pendingClaim.current;
    if (!pending || claimInFlight.current || !authSnapshot.session) return false;
    claimInFlight.current = true;
    setAuthMessage("Keeping this review with your account…");
    let claimed = false;
    try {
      const receipt = await claimGuestReview(pending.guestToken, pending.idempotencyKey);
      pendingClaim.current = null;
      clearAuthIntent();
      clearGuestSession();
      setGuestMessage(receipt.already_claimed ? "This review is already in your account." : "Review saved to your account.");
      setAccountPromptOpen(false);
      setRoute("recent");
      claimed = true;
    } catch (claimError) {
      const message = claimError instanceof ApiError && claimError.status === 401
        ? "Your account session expired. Sign in again to save this review."
        : claimError instanceof ApiError && claimError.status === 404
          ? "This guest review has expired. It cannot be saved now."
          : claimError instanceof Error
            ? claimError.message
            : "Could not save this review yet.";
      setAuthMessage(message);
    } finally {
      claimInFlight.current = false;
    }
    if (claimed) {
      try {
        await refreshAccountLibrary();
      } catch (refreshError) {
        setError(refreshError instanceof Error ? refreshError.message : "Review saved, but the account library could not refresh yet.");
      }
    }
    return claimed;
  }, [authSnapshot.session, refreshAccountLibrary]);

  const handleAuthenticatedSession = useCallback(async () => {
    if (!authSnapshot.session) return;
    setAuthBusy(null);
    setAuthMessage("");
    const claimed = await claimPendingReview();
    if (!claimed && !pendingClaim.current) {
      try {
        await refreshAccountLibrary();
      } catch (refreshError) {
        setError(refreshError instanceof Error ? refreshError.message : "Your account library could not refresh yet.");
      }
    }
    if (!pendingClaim.current) {
      if (accountFlowRequested.current && pendingAccountContext.current === "landing") {
        setAccountPromptOpen(false);
        setRoute("home");
      }
      if (accountFlowRequested.current) clearAuthIntent();
      accountFlowRequested.current = false;
    }
  }, [authSnapshot.session, claimPendingReview, refreshAccountLibrary]);

  useEffect(() => {
    const intent = loadAuthIntent();
    if (intent) {
      accountFlowRequested.current = true;
      pendingAccountContext.current = intent.context;
      setAccountPromptContext(intent.context);
      setAccountPromptOpen(true);
      if (intent.context === "save") {
        const guestSession = loadGuestSession();
        pendingClaim.current = guestSession && intent.idempotencyKey
          ? { guestToken: guestSession.token, idempotencyKey: intent.idempotencyKey }
          : null;
      }
    }
    const redirectMessage = consumeAuthRedirectMessage();
    if (redirectMessage) setAuthMessage(redirectMessage);
    const unsubscribe = subscribeAuth((snapshot) => {
      setAuthSnapshot(snapshot);
      if (snapshot.event) setAuthRevision((value) => value + 1);
      if (snapshot.event === "SIGNED_OUT") {
        accountFlowRequested.current = false;
        clearAuthIntent();
        pendingClaim.current = null;
        setGames([]);
        setJobs([]);
        setProfile(null);
        setDrills([]);
        setIdentityPrompt(null);
        setIdentityError("");
        setSelectedGameId("");
        setRoute("landing");
        setAccountPromptOpen(false);
        setAuthBusy(null);
        setAuthMessage(snapshot.message);
      }
    });
    void initializeAuth().then(setAuthSnapshot);
    return unsubscribe;
  }, []);

  useEffect(() => () => {
    browserAnalysisAbort.current?.abort();
    browserEngineRef.current?.dispose();
  }, []);

  useEffect(() => {
    if (authSnapshot.session) void handleAuthenticatedSession();
  }, [authSnapshot.session?.access_token, handleAuthenticatedSession]);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      try {
        let guestSession = loadGuestSession();
        if (!guestSession) {
          try {
            const created = await createGuestSession();
            guestSession = saveGuestSession({
              guestId: created.guest_id,
              token: created.token,
              expiresAtMs: created.expires_at_ms,
            });
          } catch (sessionError) {
            // The local reference runtime intentionally has no hosted session endpoint.
            if (!(sessionError instanceof ApiError) || ![404, 503].includes(sessionError.status)) throw sessionError;
          }
        }
        if (guestSession) setGuestTrial(bindGuestSession(guestSession.guestId, guestSession.expiresAtMs));
        const [listed, jobState, nextProfile, nextDrills, chessCom] = await Promise.all([
          listGames(),
          loadJobs(),
          loadProfile().catch(() => null),
          loadDrills().catch(() => []),
          loadChessComProfile().catch(() => null),
        ]);
        const loaded = await Promise.all(listed.map((game) => loadGame(game.game.id)));
        if (cancelled) return;
        setGames(loaded);
        setJobs(jobState.jobs);
        setProfile(nextProfile);
        setDrills(nextDrills);
        setChessComConnected(chessCom?.connected ?? false);
        const initialGame = loaded.find((game) => game.analysis_status === "complete") ?? loaded[0];
        setSelectedGameId((current) => current || initialGame?.game.id || "");
        setSelectedPly(initialGame ? reviewLandingPly(initialGame) : 0);
        await refreshRuntime();
      } catch (loadError) {
        if (!cancelled) setError(loadError instanceof Error ? loadError.message : "Local service is unavailable.");
      }
    })();
    return () => { cancelled = true; };
  }, [refreshRuntime]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem("pct-theme", theme);
  }, [theme]);

  useEffect(() => {
    localStorage.setItem("pct-browser-engine-profile", browserProfile);
  }, [browserProfile]);

  useEffect(() => {
    const expected = route === "landing" ? "#/" : `#/${route}`;
    if (window.location.hash !== expected) window.history.replaceState(null, "", expected);
  }, [route]);

  useEffect(() => {
    const onHashChange = () => setRoute(routeFromHash());
    window.addEventListener("hashchange", onHashChange);
    return () => window.removeEventListener("hashchange", onHashChange);
  }, []);

  useEffect(() => {
    let reconnect = 0;
    let socket: WebSocket | null = null;
    const connect = () => {
      socket = new WebSocket(eventUrl("/ws"), eventProtocols());
      socket.addEventListener("message", (event) => {
        const message = JSON.parse(String(event.data)) as ProgressSocketMessage;
        if (message.type === "jobs_snapshot") setJobs(message.jobs);
        if (message.type === "job_update") {
          setJobs((current) => [...current.filter((job) => job.id !== message.job.id), message.job]);
          if (message.job.status === "complete") void refreshGame(message.job.game_id);
          if (message.job.status === "running") void refreshRuntime();
        }
      });
      socket.addEventListener("close", () => { reconnect = window.setTimeout(connect, 1500); });
    };
    connect();
    return () => { window.clearTimeout(reconnect); socket?.close(); };
  }, [authRevision, refreshGame, refreshRuntime]);

  const resetTransient = useCallback((ply = selectedPly) => {
    setSelectedPly(ply);
    setReviewMode(pauseForSelectedMove(selectedGame?.analysis?.moves[ply]));
    setHighlightedUci("");
    setTrySource("");
    setTryMessage("");
    setVariation(null);
    setVariationMessage("");
    setVariationAnalysis(null);
    setVariationBusy(false);
    setMoreOpen(false);
  }, [selectedGame, selectedPly]);

  const openGame = useCallback((gameId: string, ply = 0) => {
    const game = games.find((item) => item.game.id === gameId);
    if (!game) return;
    const nextPly = Math.max(0, Math.min(ply, game.game.plies.length - 1));
    setSelectedGameId(gameId);
    setSelectedPly(nextPly);
    setReviewMode(pauseForSelectedMove(game.analysis?.moves[nextPly]));
    setHighlightedUci("");
    setVariation(null);
    setTrySource("");
    setMoveListExpanded(false);
    setOverviewOpen(false);
    setRoute("analysis");
  }, [games]);

  const startServerFallback = useCallback(async (gameId: string) => {
    const job = await startAnalysis(gameId);
    setJobs((current) => [...current.filter((item) => item.id !== job.id), job]);
    return "Browser analysis is unavailable here, so the hosted fallback is running.";
  }, []);

  const analyzeGame = useCallback(async (gameId: string) => {
    const game = games.find((item) => item.game.id === gameId);
    if (!game) return;
    openGame(gameId, 0);
    if (game.analysis_status === "complete") return;
    // Account reviews keep the existing hosted job path. Guests use the browser worker so the
    // free first review does not require server Stockfish capacity.
    if (authSnapshot.session) {
      try {
        const job = await startAnalysis(gameId);
        setJobs((current) => [...current.filter((item) => item.id !== job.id), job]);
        setGuestTrial(markGuestAnalysisUsed(gameId));
        setGuestMessage("");
        setError("");
      } catch (analysisError) {
        setGuestTrial(releaseGuestAnalysis(gameId));
        setError(analysisError instanceof Error ? analysisError.message : "Could not start analysis.");
      }
      return;
    }

    const reservation = reserveGuestAnalysis(gameId);
    setGuestTrial(reservation.state);
    if (!reservation.ok) {
      setGuestMessage(reservation.reason === "already_reserved" ? "Your guest review is already in progress." : "Your one free guest review is complete. Account saving is coming next.");
      return;
    }

    const abortController = new AbortController();
    const engine = createBrowserEngine();
    browserAnalysisAbort.current = abortController;
    browserEngineRef.current = engine;
    setBrowserAnalysisProgress({
      gameId,
      profile: browserProfile,
      stage: "starting",
      complete: 0,
      total: Math.max(1, game.game.plies.length * 2),
      ply: 0,
      position: "before",
      message: `Starting ${browserProfile === "balanced" ? "Balanced" : "Quick"} browser analysis`,
    });
    try {
      await runGuestBrowserReview(game, {
        profile: browserProfile,
        engine,
        api: {
          start: startBrowserAnalysis,
          submit: submitBrowserObservation,
          finalize: finalizeBrowserAnalysis,
        },
        signal: abortController.signal,
        onProgress: setBrowserAnalysisProgress,
      });
      setGuestTrial(markGuestAnalysisUsed(gameId));
      setGuestMessage("Your browser review is ready.");
      setError("");
      try {
        await refreshGame(gameId);
      } catch (refreshError) {
        setError(refreshError instanceof Error ? refreshError.message : "Review is ready, but the game could not refresh yet.");
      }
    } catch (analysisError) {
      const fallback = isBrowserFallback(analysisError);
      let message = analysisError instanceof BrowserEngineError && analysisError.code === "cancelled"
        ? "Browser analysis cancelled. Your free review is still available."
        : analysisError instanceof ApiError &&
          analysisError.code === "invalid_argument" &&
          analysisError.message.toLowerCase().includes("completed game")
          ? "Only completed games can be analyzed. Import a finished game to continue."
          : fallback
          ? "Browser analysis is unavailable here."
            : analysisError instanceof Error ? analysisError.message : "Could not start analysis.";
      let fallbackStarted = false;
      if (fallback) {
        try {
          message = await startServerFallback(gameId);
          setGuestTrial(markGuestAnalysisUsed(gameId));
          fallbackStarted = true;
        } catch (fallbackError) {
          setGuestTrial(releaseGuestAnalysis(gameId));
          message = fallbackError instanceof Error ? fallbackError.message : "Could not start analysis.";
        }
      } else {
        setGuestTrial(releaseGuestAnalysis(gameId));
      }
      setGuestMessage(message);
      if (!fallbackStarted) setError(message);
    } finally {
      browserAnalysisAbort.current = null;
      browserEngineRef.current?.dispose();
      browserEngineRef.current = null;
      setBrowserAnalysisProgress(null);
    }
  }, [authSnapshot.session, browserProfile, games, openGame, refreshGame, startServerFallback]);

  const cancelBrowserAnalysis = useCallback(() => {
    browserAnalysisAbort.current?.abort();
    void browserEngineRef.current?.cancel();
  }, []);

  const refreshGames = useCallback(async () => {
    if (refreshBusy) return;
    setRefreshBusy(true);
    setRefreshMessage("Checking your Chess.com archive…");
    try {
      const connection = await loadChessComProfile();
      setChessComConnected(connection.connected);
      if (!connection.connected) {
        setImportOpen(true);
        setRefreshMessage("No Chess.com profile is connected. Import a public game to continue.");
        return;
      }
      const before = games.length;
      let sync = await startChessComSync({ days: 7 });
      while (sync.status === "queued" || sync.status === "running") {
        setRefreshMessage(sync.current_month ? `Checking ${sync.current_month}…` : "Refreshing recent games…");
        await delay(350);
        sync = await loadChessComSync(sync.id);
      }
      if (sync.status !== "succeeded") throw new Error(sync.error || "Refresh did not complete.");
      const listed = await listGames();
      const loaded = await Promise.all(listed.map((game) => loadGame(game.game.id)));
      const [nextProfile, nextDrills] = await Promise.all([loadProfile().catch(() => null), loadDrills().catch(() => [])]);
      setGames(loaded);
      setProfile(nextProfile);
      setDrills(nextDrills);
      const imported = Math.max(0, loaded.length - before);
      setRefreshMessage(imported ? `${imported} new game${imported === 1 ? "" : "s"} imported` : "Games are up to date");
      setError("");
    } catch (refreshError) {
      const message = refreshError instanceof Error ? refreshError.message : "Could not refresh games.";
      setRefreshMessage(message);
      setError(message);
    } finally {
      setRefreshBusy(false);
    }
  }, [games.length, refreshBusy]);

  const navigate = useCallback((action: "first" | "previous" | "next" | "last") => {
    const last = Math.max(0, (selectedGame?.game.plies.length ?? 1) - 1);
    let next = selectedPly;
    if (action === "first") next = 0;
    if (action === "previous") next = Math.max(0, selectedPly - 1);
    if (action === "next") next = Math.min(last, selectedPly + 1);
    if (action === "last") next = last;
    resetTransient(next);
  }, [resetTransient, selectedGame, selectedPly]);

  useEffect(() => {
    if (autoplayTimer.current) window.clearTimeout(autoplayTimer.current);
    if (!isPlaying(reviewMode)) return;
    const moves = selectedGame?.analysis?.moves ?? [];
    const delayMs = reviewMode === "transitioning_from_key_move" ? 700 : autoplayDelay(moves[selectedPly]);
    if (delayMs === null || document.hidden) {
      setReviewMode("manual");
      return;
    }
    autoplayTimer.current = window.setTimeout(() => {
      const transition = completePlaybackDwell(reviewMode, selectedPly, moves);
      setSelectedPly(transition.selectedPly);
      setReviewMode(transition.mode);
      setHighlightedUci("");
    }, delayMs);
    return () => { if (autoplayTimer.current) window.clearTimeout(autoplayTimer.current); };
  }, [reviewMode, selectedGame, selectedPly]);

  const togglePlayback = useCallback(() => {
    if (isPlaying(reviewMode)) { setReviewMode("manual"); return; }
    const transition = startPlayback(reviewMode, selectedPly, selectedGame?.analysis?.moves ?? []);
    setReviewMode(transition.mode);
    setSelectedPly(transition.selectedPly);
  }, [reviewMode, selectedGame, selectedPly]);

  const startVariation = useCallback(async (rootPosition: "before" | "after") => {
    if (!selectedGame) return;
    setVariationReturn({ mode: reviewMode, highlight: highlightedUci });
    try {
      const saved = await listVariations(selectedGame.game.id);
      const next = [...saved].reverse().find((item) => item.root_ply === selectedPly && item.root_position === rootPosition)
        ?? await createVariation(selectedGame.game.id, selectedPly, rootPosition);
      setVariation(next);
      setReviewMode("variation");
      setHighlightedUci("");
      setTrySource("");
      setVariationMessage(next.nodes.length > 1 ? "Saved branch restored." : "Variation started from the canonical position.");
      setVariationAnalysis(null);
      setMoreOpen(false);
    } catch (variationError) {
      setVariationMessage(variationError instanceof Error ? variationError.message : "Could not start variation.");
    }
  }, [highlightedUci, reviewMode, selectedGame, selectedPly]);

  const variationNode = useCallback((id: number | undefined) => variation?.nodes.find((node) => node.id === id), [variation]);

  const chooseSquare = useCallback(async (square: string) => {
    if (reviewMode !== "try_move" && reviewMode !== "variation") return;
    if (!trySource) {
      setTrySource(square);
      reviewMode === "variation" ? setVariationMessage(`Selected ${square}. Choose a destination.`) : setTryMessage(`Selected ${square}. Choose a destination.`);
      return;
    }
    const uci = `${trySource}${square}`.toLowerCase();
    setTrySource("");
    if (reviewMode === "try_move" && selectedGame) {
      try {
        const result = await submitReviewAttempt(selectedGame.game.id, selectedPly, uci);
        setTryMessage(result.accepted ? "Correct. C++ confirmed this engine-backed candidate." : "Legal, but another move changes the forcing sequence more precisely.");
      } catch (attemptError) {
        setTryMessage(attemptError instanceof ApiError && attemptError.code === "illegal_move" ? "That move is illegal in this position." : attemptError instanceof Error ? attemptError.message : "Could not validate the move.");
      }
    }
    if (reviewMode === "variation" && selectedGame && variation) {
      try {
        const next = await extendVariation(selectedGame.game.id, variation.id, variation.current_node_id, uci);
        setVariation(next);
        const node = next.nodes.find((item) => item.id === next.current_node_id);
        setVariationMessage(`${node?.san || node?.uci || uci} added to this branch.`);
        setVariationAnalysis(null);
      } catch (variationError) {
        setVariationMessage(variationError instanceof ApiError && variationError.code === "illegal_move" ? "That move is illegal in this position." : variationError instanceof Error ? variationError.message : "Could not extend variation.");
      }
    }
  }, [reviewMode, selectedGame, selectedPly, trySource, variation]);

  const submitRetry = useCallback(async (uci: string) => {
    if (!selectedGame) return;
    try {
      const result = await submitReviewAttempt(selectedGame.game.id, selectedPly, uci.trim().toLowerCase());
      setTryMessage(result.accepted ? "Correct. C++ confirmed this engine-backed candidate." : "Legal, but another move changes the forcing sequence more precisely.");
    } catch (attemptError) {
      setTryMessage(attemptError instanceof ApiError && attemptError.code === "illegal_move" ? "That move is illegal in this position." : attemptError instanceof Error ? attemptError.message : "Could not validate the move.");
    }
  }, [selectedGame, selectedPly]);

  const leaveVariation = useCallback(async (remove = false) => {
    if (remove && variation && selectedGame) {
      if (!window.confirm("Delete this saved variation and all branches?")) return;
      await deleteVariation(selectedGame.game.id, variation.id);
    }
    setVariation(null);
    setVariationAnalysis(null);
    setVariationMessage("");
    setReviewMode(variationReturn.mode);
    setHighlightedUci(variationReturn.highlight);
    setTrySource("");
  }, [selectedGame, variation, variationReturn]);

  const resetActiveVariation = useCallback(async () => {
    if (!selectedGame || !variation) return;
    const next = await resetVariation(selectedGame.game.id, variation.id);
    setVariation(next);
    setVariationAnalysis(null);
    setVariationMessage("Variation reset to its canonical root.");
  }, [selectedGame, variation]);

  const stepVariationBack = useCallback(async () => {
    if (!selectedGame || !variation) return;
    const parentId = variationNode(variation.current_node_id)?.parent_id ?? -1;
    if (parentId < 0) return;
    const next = await setVariationCursor(selectedGame.game.id, variation.id, parentId);
    setVariation(next);
    setVariationAnalysis(null);
    setVariationMessage(parentId === 0 ? "Back at the variation root." : "Previous branch position restored.");
  }, [selectedGame, variation, variationNode]);

  const analyzeActiveVariation = useCallback(async () => {
    if (!selectedGame || !variation || variationBusy) return;
    setVariationBusy(true);
    setVariationMessage("Stockfish is evaluating this branch at background priority.");
    try {
      setVariationAnalysis(await analyzeVariation(selectedGame.game.id, variation.id));
      setVariationMessage("Branch evaluation ready. The canonical game is unchanged.");
    } catch (variationError) {
      setVariationMessage(variationError instanceof Error ? variationError.message : "Could not analyze variation.");
    } finally { setVariationBusy(false); }
  }, [selectedGame, variation, variationBusy]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (route !== "analysis" || event.metaKey || event.ctrlKey || event.altKey) return;
      const target = event.target as HTMLElement | null;
      if (target?.matches("input, textarea, select")) return;
      if (event.key === "ArrowLeft") { event.preventDefault(); navigate("previous"); }
      if (event.key === "ArrowRight") { event.preventDefault(); navigate("next"); }
      if (event.key === "Home") { event.preventDefault(); navigate("first"); }
      if (event.key === "End") { event.preventDefault(); navigate("last"); }
      if (event.key === " ") { event.preventDefault(); togglePlayback(); }
      if (event.key.toLowerCase() === "f") setOrientation((value) => value === "white" ? "black" : "white");
      if (event.key.toLowerCase() === "r" && selectedMove) { setReviewMode("try_move"); setHighlightedUci(""); }
      if (event.key.toLowerCase() === "v" && selectedMove) void startVariation("before");
      if (event.key.toLowerCase() === "b" && selectedMove) { setReviewMode("revealed_move"); setHighlightedUci(selectedMove.best_uci); }
      if (event.key === "Escape") { if (variation) void leaveVariation(); else resetTransient(); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [leaveVariation, navigate, resetTransient, route, selectedMove, startVariation, togglePlayback, variation]);

  const runImport = useCallback(async (url: string, pgn: string) => {
    if (!url.trim() && !pgn.trim()) { setError("Paste a Chess.com game URL or PGN."); return; }
    setImportBusy(true);
    setImportStage("Reading game");
    setError("");
    try {
      const result = await importGameObservable(url.trim() ? { url: url.trim() } : { pgn: pgn.trim() });
      let gameId = result.status === "resolving" ? "" : result.game_id;
      let identitySource: IdentityPromptState["source"] = url.trim() ? "public_page" : "pgn";
      if (result.status === "resolving") {
        setImportStage("Finding public archive");
        let resolution = result.resolution;
        while (resolution.status === "queued" || resolution.status === "running") {
          await delay(250);
          resolution = await loadImportResolution(result.resolution_id);
        }
        if (resolution.status !== "resolved" || !resolution.imported_game_id) throw new Error(resolution.error || "Chess.com import could not be resolved.");
        gameId = resolution.imported_game_id;
        identitySource = resolution.source === "profile_archive" || resolution.source === "local_archive" ? "profile_archive" : "public_page";
      }
      setImportStage("Reconstructing positions");
      const game = await refreshGame(gameId);
      setSelectedGameId(game.game.id);
      setSelectedPly(0);
      setImportOpen(false);
      setRoute("recent");
      const names = [game.game.tags.White, game.game.tags.Black]
        .map((name) => name?.trim() ?? "")
        .filter((name) => name && name !== "?" && !["unknown", "anonymous"].includes(name.toLowerCase()))
        .filter((name, index, values) => values.indexOf(name) === index);
      setIdentityError("");
      setIdentityPrompt({ gameId: game.game.id, names, source: identitySource });
    } catch (importError) {
      setError(importError instanceof Error ? importError.message : "Import failed.");
    } finally {
      setImportBusy(false);
      setImportStage("");
    }
  }, [refreshGame]);

  const decideImportedIdentity = useCallback(async (decision: PlayerIdentity["decision"], playerName: string) => {
    if (!identityPrompt) return;
    setIdentityBusy(true);
    setIdentityError("");
    try {
      await savePlayerIdentity({
        game_id: identityPrompt.gameId,
        player_name: playerName,
        source: identityPrompt.source,
        decision,
      });
      const nextProfile = await loadProfile().catch(() => null);
      if (nextProfile) setProfile(nextProfile);
      setIdentityPrompt(null);
    } catch (identityFailure) {
      setIdentityError(identityFailure instanceof Error ? identityFailure.message : "Could not save that identity decision.");
    } finally {
      setIdentityBusy(false);
    }
  }, [identityPrompt]);

  const setAppRoute = useCallback((next: Route) => {
    setRoute(next);
    setGuestMessage("");
    setMoreOpen(false);
    setOverviewOpen(false);
    if (next === "settings") void refreshRuntime();
  }, [refreshRuntime]);

  const startLandingReview = useCallback(() => {
    setRoute("home");
    setImportOpen(true);
  }, []);

  const canSaveGuestReview = !authSnapshot.session &&
    guestTrial.status === "used" &&
    guestTrial.gameId === selectedGameId &&
    Boolean(loadGuestSession());

  const openAccountPrompt = useCallback((context: "landing" | "save") => {
    accountFlowRequested.current = true;
    pendingAccountContext.current = context;
    setAccountPromptContext(context);
    setAccountPromptOpen(true);
    if (context === "save") {
      const guestSession = loadGuestSession();
      pendingClaim.current = guestSession
        ? { guestToken: guestSession.token, idempotencyKey: createClaimIdempotencyKey() }
        : null;
      if (!guestSession) {
        setAuthMessage("This guest review expired on this device. It cannot be saved now.");
      } else {
        setAuthMessage("");
      }
      if (authSnapshot.session) void claimPendingReview();
    } else {
      pendingClaim.current = null;
      setAuthMessage("");
    }
  }, [authSnapshot.session, claimPendingReview]);

  const startProviderSignIn = useCallback(async (provider: AuthProvider) => {
    setAuthBusy(provider);
    setAuthMessage(`Connecting to ${authProviderLabel(provider)}…`);
    saveAuthIntent({
      context: pendingAccountContext.current,
      idempotencyKey: pendingClaim.current?.idempotencyKey,
    });
    const result = await signInWithProvider(provider);
    setAuthMessage(result.message);
    if (!result.ok) {
      clearAuthIntent();
      setAuthBusy(null);
    }
  }, []);

  const handleSignOut = useCallback(async () => {
    setAuthBusy(null);
    const result = await signOut();
    setAuthMessage(result.message);
    if (!result.ok) setError(result.message);
  }, []);

  const closeAccountPrompt = useCallback(() => {
    if (!claimInFlight.current) {
      pendingClaim.current = null;
      accountFlowRequested.current = false;
      clearAuthIntent();
    }
    setAccountPromptOpen(false);
  }, []);

  const accountPrompt = accountPromptOpen && <AccountPrompt
    context={accountPromptContext}
    auth={authSnapshot}
    busyProvider={authBusy}
    message={authMessage}
    onProvider={(provider) => void startProviderSignIn(provider)}
    onSignOut={() => void handleSignOut()}
    onClose={closeAccountPrompt}
  />;

  if (route === "landing") {
    return <><LandingView onStart={startLandingReview} onSignIn={() => openAccountPrompt("landing")}/>{accountPrompt}</>;
  }

  const shared = { games, profile, selectedGame, selectedPly, selectedMove, jobs, selectedJob };
  let view: ReactNode;
  let header: ReactNode;

  if (route === "home") {
    const greeting = dayGreeting(new Date());
    const libraryMessage = refreshMessage || (games.length ? `${games.length} game${games.length === 1 ? "" : "s"} available locally` : "Your local chess intelligence console");
    header = <TopBar
      icon="home"
      title={greeting}
      detail={libraryMessage}
      meta={chessComConnected ? "Chess.com connected" : undefined}
      actions={<SoftButton icon="retry" disabled={refreshBusy} onClick={() => void refreshGames()}>
        {refreshBusy ? "Refreshing" : "Refresh games"}
      </SoftButton>}
    />;
    view = <HomeView
      games={games}
      jobs={jobs}
      profile={profile}
      drills={drills}
      refreshBusy={refreshBusy}
      refreshMessage={refreshMessage}
      guestTrial={guestTrial}
      guestMessage={guestMessage}
      onOpen={openGame}
      onRecent={() => setAppRoute("recent")}
      onImport={() => setImportOpen(true)}
      onPractice={(drill) => openGame(drill.source_game_id, drill.source_ply)}
      onSave={() => openAccountPrompt("save")}
    />;
  } else if (route === "recent") {
    header = <TopBar title="Recent Games" detail={`${games.length} local game${games.length === 1 ? "" : "s"}`} actions={<SoftButton icon="import" onClick={() => setImportOpen(true)}>Import game</SoftButton>}/>;
    view = <RecentView
      games={games}
      jobs={jobs}
      profile={profile}
      selected={selectedGames}
      onSelect={(id) => setSelectedGames((current) => { const next = new Set(current); next.has(id) ? next.delete(id) : next.add(id); return next; })}
      onClear={() => setSelectedGames(new Set())}
      onOpen={openGame}
      onAnalyze={(id) => void analyzeGame(id)}
      onAnalyzeSelected={() => void Promise.all([...selectedGames].map((id) => analyzeGame(id))).then(() => setSelectedGames(new Set()))}
      onImport={() => setImportOpen(true)}
    />;
  } else if (route === "analysis") {
    const tags = selectedGame?.game.tags ?? {};
    const gameName = selectedGame ? `${tags.White ?? "White"} vs. ${tags.Black ?? "Black"}` : "No game selected";
    const opening = selectedGame?.analysis ? `${selectedGame.analysis.opening} · ${selectedGame.analysis.eco}` : "Choose a recent game";
    const activeBrowserProgress = selectedGame && browserAnalysisProgress?.gameId === selectedGame.game.id ? browserAnalysisProgress : null;
    header = <TopBar title={gameName} detail={opening} meta={selectedGame?.analysis ? `${selectedGame.analysis.accuracy.toFixed(1)} accuracy` : undefined} actions={<>
      {canSaveGuestReview && <SoftButton icon="book" onClick={() => openAccountPrompt("save")}>Save review</SoftButton>}
      {selectedGame && selectedGame.analysis_status !== "complete" && <SoftButton disabled={Boolean(activeBrowserProgress)} onClick={() => void analyzeGame(selectedGame.game.id)}>{activeBrowserProgress ? `${activeBrowserProgress.complete}/${activeBrowserProgress.total}` : selectedJob?.status === "running" ? `${selectedJob.progress.message} ${selectedJob.progress.complete}/${selectedJob.progress.total}` : "Analyze"}</SoftButton>}
      <SoftButton icon="overview" onClick={() => setOverviewOpen((value) => !value)}>Overview</SoftButton>
      <div className="more-wrap"><SoftButton icon="more" aria-label="More analysis actions" onClick={() => setMoreOpen((value) => !value)}/>{moreOpen && <div className="action-menu">
        <button onClick={() => { setReviewMode("try_move"); setHighlightedUci(""); setMoreOpen(false); }}><Icon name="retry"/>Retry this move</button>
        <button onClick={() => void startVariation("before")}><Icon name="branch"/>Explore variation</button>
        <button onClick={() => { setReviewMode("revealed_move"); setHighlightedUci(selectedMove?.best_uci ?? ""); setMoreOpen(false); }}><Icon name="star"/>Show best move</button>
        <button onClick={() => { setOrientation((value) => value === "white" ? "black" : "white"); setMoreOpen(false); }}><Icon name="flip"/>Flip board</button>
      </div>}</div>
    </>}/>;
    view = <AnalysisView
      {...shared}
      orientation={orientation}
      reviewMode={reviewMode}
      highlightedUci={highlightedUci}
      trySource={trySource}
      tryMessage={tryMessage}
      variation={variation}
      variationMessage={variationMessage}
      variationAnalysis={variationAnalysis}
      variationBusy={variationBusy}
      overviewOpen={overviewOpen}
      inspectorTab={inspectorTab}
      moveListExpanded={moveListExpanded}
      runtimeSettings={runtimeSettings}
      diagnostics={diagnostics}
      error={error}
      browserProgress={activeBrowserProgress}
      onSelectPly={resetTransient}
      onNavigate={navigate}
      onTogglePlayback={togglePlayback}
      onFlip={() => setOrientation((value) => value === "white" ? "black" : "white")}
      onSquare={(square) => void chooseSquare(square)}
      onRetry={() => { setReviewMode("try_move"); setHighlightedUci(""); setTryMessage(""); }}
      onRetrySubmit={(uci) => void submitRetry(uci)}
      onVariation={() => void startVariation("before")}
      onReturn={() => variation ? void leaveVariation() : resetTransient()}
      onVariationBack={() => void stepVariationBack()}
      onVariationReset={() => void resetActiveVariation()}
      onVariationAnalyze={() => void analyzeActiveVariation()}
      onVariationDelete={() => void leaveVariation(true)}
      onToggleMoves={() => setMoveListExpanded((value) => !value)}
      onCloseOverview={() => setOverviewOpen(false)}
      onInspectorTab={setInspectorTab}
      onCancelJob={() => selectedJob && void cancelJob(selectedJob.id).then((job) => setJobs((current) => [...current.filter((item) => item.id !== job.id), job]))}
      onCancelBrowserAnalysis={cancelBrowserAnalysis}
    />;
  } else if (route === "explore") {
    const entries = buildExploreEntries(games);
    header = <TopBar title="Explore" detail="Your position library" meta={`${entries.length} concepts`}/>;
    view = <ExploreView games={games} section={exploreSection} selectedId={selectedExploreId} onSection={setExploreSection} onSelect={setSelectedExploreId} onOpen={openGame}/>;
  } else if (route === "progress") {
    const player = inferPlayerName(profile, games);
    header = <TopBar title="Progress" detail={player || "Local player profile"} meta={`${profile?.games_analyzed ?? games.filter((game) => game.analysis).length} analyzed`}/>;
    view = <ProgressView games={games} profile={profile} onOpen={openGame}/>;
  } else {
    header = <TopBar title="Settings" detail="Local preferences"/>;
    view = <SettingsView theme={theme} onTheme={setTheme} engineLinesDefault={engineLinesDefault} onEngineLines={(value) => { setEngineLinesDefault(value); setInspectorTab(value ? "line" : "summary"); localStorage.setItem("pct-engine-lines-default", String(value)); }} browserProfile={browserProfile} onBrowserProfile={setBrowserProfile} runtime={runtimeSettings} diagnostics={diagnostics}/>;
  }

  return <>
    <AppShell route={route} onRoute={setAppRoute} header={header}>{view}</AppShell>
    {importOpen && (
      <ImportModal busy={importBusy} stage={importStage} error={error} onClose={() => !importBusy && setImportOpen(false)} onSubmit={(url, pgn) => void runImport(url, pgn)}/>
    )}
    {identityPrompt && (
      <PlayerIdentityPrompt
        prompt={identityPrompt}
        busy={identityBusy}
        error={identityError}
        onClose={() => !identityBusy && setIdentityPrompt(null)}
        onDecision={(decision, playerName) => void decideImportedIdentity(decision, playerName)}
      />
    )}
    {accountPrompt}
  </>;
}

function RecentView({ games, jobs, profile, selected, onSelect, onClear, onOpen, onAnalyze, onAnalyzeSelected, onImport }: {
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
}) {
  const player = inferPlayerName(profile, games).toLowerCase();
  return <section className="soft-surface recent-surface">
    <header className="surface-heading"><div><span>Game library</span><h1>Continue where you left off.</h1></div>{selected.size > 0 && <div className="selection-actions"><strong>{selected.size} selected</strong><button onClick={onAnalyzeSelected}>Analyze selected</button><button onClick={onClear}>Clear</button></div>}</header>
    <div className="game-list" role="list" aria-label="Recent games">
      {games.map((stored) => {
        const tags = stored.game.tags;
        const white = tags.White ?? "White";
        const black = tags.Black ?? "Black";
        const isWhite = player && white.toLowerCase() === player;
        const isBlack = player && black.toLowerCase() === player;
        const opponent = isWhite ? black : isBlack ? white : `${white} vs. ${black}`;
        const latestJob = jobs.filter((job) => job.game_id === stored.game.id).sort((a, b) => b.id - a.id)[0];
        const active = latestJob?.status === "running" || latestJob?.status === "queued";
        const status = active ? latestJob.status === "running" ? "Analyzing" : "Queued" : latestJob?.status === "failed" ? "Failed" : stored.analysis_status === "complete" ? "Reviewed" : stored.analysis_status === "shallow" ? "Partial" : "Ready";
        return <article key={stored.game.id} className={`game-row ${selected.has(stored.game.id) ? "selected" : ""}`} role="listitem">
          <label className="select-control"><input type="checkbox" checked={selected.has(stored.game.id)} onChange={() => onSelect(stored.game.id)}/><span /></label>
          <button className="game-name" onClick={() => onOpen(stored.game.id, reviewLandingPly(stored))}><strong>{opponent}</strong><small>{stored.analysis?.opening || "Opening appears after analysis"}</small></button>
          <div className="game-result"><strong>{tags.Result ?? "*"}</strong><span>{tags.TimeControl || "—"}</span></div>
          <div className="game-date"><strong>{gameDate(tags)}</strong><span>{[tags.WhiteElo, tags.BlackElo].filter(Boolean).join(" / ") || "Ratings unavailable"}</span></div>
          <span className={`status-text status-${status.toLowerCase()}`}>{status}</span>
          <button className="row-button" disabled={active} onClick={() => stored.analysis_status === "complete" ? onOpen(stored.game.id, reviewLandingPly(stored)) : onAnalyze(stored.game.id)}>{stored.analysis_status === "complete" ? "Open review" : active ? status : "Analyze"}</button>
        </article>;
      })}
      {!games.length && <div className="empty-state"><Icon name="import"/><h2>No games yet</h2><p>Import a public Chess.com game or PGN. Analysis begins only when you choose it.</p><SoftButton icon="import" onClick={onImport}>Import your first game</SoftButton></div>}
    </div>
  </section>;
}

type AnalysisProps = {
  games: StoredGame[]; profile: Profile | null; selectedGame: StoredGame | null; selectedPly: number; selectedMove?: MoveAssessment; jobs: Job[]; selectedJob: Job | null;
  orientation: BoardOrientation; reviewMode: ReviewMode; highlightedUci: string; trySource: string; tryMessage: string; variation: Variation | null; variationMessage: string; variationAnalysis: VariationAnalysis | null; variationBusy: boolean;
  overviewOpen: boolean; inspectorTab: InspectorTab; moveListExpanded: boolean; runtimeSettings: RuntimeSettings | null; diagnostics: Diagnostics | null; error: string; browserProgress: GuestBrowserReviewProgress | null;
  onSelectPly: (ply: number) => void; onNavigate: (action: "first" | "previous" | "next" | "last") => void; onTogglePlayback: () => void; onFlip: () => void; onSquare: (square: string) => void;
  onRetry: () => void; onRetrySubmit: (uci: string) => void; onVariation: () => void; onReturn: () => void; onVariationBack: () => void; onVariationReset: () => void; onVariationAnalyze: () => void; onVariationDelete: () => void;
  onToggleMoves: () => void; onCloseOverview: () => void; onInspectorTab: (tab: InspectorTab) => void; onCancelJob: () => void; onCancelBrowserAnalysis: () => void;
};

function AnalysisView(props: AnalysisProps) {
  const { selectedGame: game, selectedMove: move, selectedPly, reviewMode, variation } = props;
  if (!game) return <section className="soft-surface analysis-empty"><Icon name="analysis"/><h1>Choose a game to analyze.</h1><p>Open a game from Recent Games. Its canonical moves remain unchanged while you review or branch.</p></section>;
  const ply = game.game.plies[selectedPly];
  const currentVariationNode = variation?.nodes.find((node) => node.id === variation.current_node_id);
  const fen = reviewMode === "variation" ? currentVariationNode?.fen ?? variation?.root_fen ?? initialFen : reviewMode === "try_move" || reviewMode === "revealed_move" ? move?.fen_before ?? ply?.fen_before ?? initialFen : ply?.fen_after ?? initialFen;
  const activeUci = reviewMode === "variation" ? currentVariationNode?.uci ?? "" : props.highlightedUci || ply?.uci || "";
  return <div className="analysis-layout">
    {props.browserProgress ? <div className="analysis-progress"><span>{props.browserProgress.message}</span><progress aria-label="Browser analysis progress" max={props.browserProgress.total} value={props.browserProgress.complete}/><strong>{props.browserProgress.complete}/{props.browserProgress.total}</strong><button onClick={props.onCancelBrowserAnalysis}>Cancel</button></div> : props.selectedJob && (props.selectedJob.status === "running" || props.selectedJob.status === "queued") && <div className="analysis-progress"><span>{props.selectedJob.progress.message}</span><progress aria-label="Analysis progress" max={props.selectedJob.progress.total || 100} value={props.selectedJob.progress.total ? props.selectedJob.progress.complete : 8}/><strong>{props.selectedJob.progress.complete}/{props.selectedJob.progress.total}</strong><button onClick={props.onCancelJob}>Cancel</button></div>}
    <section className={`board-surface ${reviewMode === "variation" ? "variation-active" : ""}`}>
      <EvaluationBar value={move?.evaluation_after}/>
      <div className="board-holder"><ChessBoard fen={fen} orientation={props.orientation} activeUci={activeUci} sourceSquare={props.trySource} interactive={reviewMode === "try_move" || reviewMode === "variation"} showArrow={reviewMode === "revealed_move"} onSquare={props.onSquare}/></div>
    </section>
    <aside className="review-rail">
      <ReviewCard move={move}/>
      <BestMoveCard {...props}/>
      <MoveList game={game} selectedPly={selectedPly} expanded={props.moveListExpanded} onSelect={props.onSelectPly} onToggle={props.onToggleMoves}/>
    </aside>
    <Playback game={game} selectedPly={selectedPly} playing={isPlaying(reviewMode)} onNavigate={props.onNavigate} onPlay={props.onTogglePlayback} onFlip={props.onFlip}/>
    {props.overviewOpen && (
      <OverviewDrawer {...props}/>
    )}
  </div>;
}

function ReviewCard({ move }: { move?: MoveAssessment }) {
  if (!move) return <section className="review-card"><header>Current Move</header><div className="card-empty">Analysis appears here when ready.</div></section>;
  return <section className={`review-card current-card class-${classificationClass(move.classification)}`}>
    <header><span>Current Move</span><small>{move.move_number}{move.side === "white" ? "." : "…"}</small></header>
    <div className="verdict-reading"><span className="class-orb"><Icon name={needsAttention(move.classification) ? "warning" : "check"}/></span><div><b>{move.classification}</b><strong>{move.move_number}{move.side === "white" ? "." : "…"} {move.played_san || move.san}</strong><p>{moveExplanation(move)}</p></div></div>
  </section>;
}

function BestMoveCard(props: AnalysisProps) {
  const move = props.selectedMove;
  if (!move) return <section className="review-card best-card"><header>Best Move</header><div className="card-empty">Waiting for analysis.</div></section>;
  if (props.reviewMode === "try_move") return <section className="review-card mode-card"><header><span>Retry Move</span><button onClick={props.onReturn}>Return</button></header><div className="mode-content"><Icon name="retry"/><h3>Find a stronger move</h3><p>The board is restored to the position before {move.played_san || move.san}. Choose two squares or enter UCI.</p><RetryForm message={props.tryMessage} onSubmit={props.onRetrySubmit}/></div></section>;
  if (props.reviewMode === "variation") return <section className="review-card mode-card variation-card"><header><span>Variation</span><button onClick={props.onReturn}>Return</button></header><div className="mode-content"><Icon name="branch"/><h3>Explore this branch</h3><p>{props.variationMessage || "Choose a source and destination on the board."}</p>{props.variationAnalysis && <div className="variation-eval"><strong>{props.variationAnalysis.best_move || "—"}</strong><code>{props.variationAnalysis.lines[0]?.moves.join(" ")}</code></div>}<div className="mode-actions"><button onClick={props.onVariationBack}>Back</button><button onClick={props.onVariationReset}>Reset</button><button disabled={props.variationBusy} onClick={props.onVariationAnalyze}>{props.variationBusy ? "Analyzing…" : "Analyze"}</button><button className="danger-text" onClick={props.onVariationDelete}>Delete</button></div></div></section>;
  return <section className="review-card best-card class-best"><header><span>Best Move</span><small>{formatEval(move.evaluation_after_best)}</small></header><div className="verdict-reading"><span className="class-orb"><Icon name="star"/></span><div><b>Best Move</b><strong>{move.move_number}{move.side === "white" ? "." : "…"} {move.best_san || move.best_uci}</strong><p>Maintains the position at {formatEval(move.evaluation_after_best)}, compared with {formatEval(move.evaluation_after)} after the played move.</p><div className="quiet-actions"><button onClick={props.onRetry}><Icon name="retry"/>Retry</button><button onClick={props.onVariation}><Icon name="branch"/>Explore</button></div></div></div></section>;
}

function RetryForm({ message, onSubmit }: { message: string; onSubmit: (uci: string) => void }) {
  const [value, setValue] = useState("");
  return <form className="retry-form" onSubmit={(event) => { event.preventDefault(); onSubmit(value); }}><label><span className="sr-only">Move in UCI</span><input value={value} onChange={(event) => setValue(event.target.value)} pattern="[a-h][1-8][a-h][1-8][qrbn]?" placeholder="e2e4" required/></label><button>Check</button>{message && <small role="status">{message}</small>}</form>;
}

function MoveList({ game, selectedPly, expanded, onSelect, onToggle }: { game: StoredGame; selectedPly: number; expanded: boolean; onSelect: (ply: number) => void; onToggle: () => void }) {
  const moves = game.analysis?.moves ?? [];
  const plies = game.game.plies;
  const start = expanded ? 0 : Math.max(0, selectedPly - 3);
  const end = expanded ? plies.length : Math.min(plies.length, selectedPly + 4);
  return <section className="review-card moves-card"><header><span>Move List</span><small>{plies.length} plies</small></header><div className="move-ledger">{plies.slice(start, end).map((ply, offset) => {
    const index = start + offset;
    const assessment = moves[index];
    return <button key={index} className={index === selectedPly ? "current" : ""} onClick={() => onSelect(index)}><span>{Math.floor(index / 2) + 1}{index % 2 ? "…" : "."}</span><strong>{ply.san}</strong><em>{assessment?.classification || "Pending"}</em>{index === selectedPly && <i className={`mini-class class-${classificationClass(assessment?.classification || "pending")}`}>{needsAttention(assessment?.classification ?? "") ? "!" : ""}</i>}</button>;
  })}</div>{plies.length > 7 && <button className="ledger-toggle" onClick={onToggle}>{expanded ? "Show nearby moves" : "Show all moves"}<span>⌄</span></button>}</section>;
}

function Playback({ game, selectedPly, playing, onNavigate, onPlay, onFlip }: { game: StoredGame; selectedPly: number; playing: boolean; onNavigate: AnalysisProps["onNavigate"]; onPlay: () => void; onFlip: () => void }) {
  const selected = game.game.plies[selectedPly];
  return <footer className="playback-bar"><div className="playback-buttons"><SoftButton icon="first" aria-label="First move" onClick={() => onNavigate("first")}/><SoftButton icon="previous" aria-label="Previous move" onClick={() => onNavigate("previous")}/><SoftButton className="primary-play" icon={playing ? "pause" : "play"} aria-label={playing ? "Pause review" : "Play review"} onClick={onPlay}/><SoftButton icon="next" aria-label="Next move" onClick={() => onNavigate("next")}/><SoftButton icon="last" aria-label="Last move" onClick={() => onNavigate("last")}/></div><button className="move-selector"><span>{selected ? `${Math.floor(selectedPly / 2) + 1}${selectedPly % 2 ? "…" : "."} ${selected.san}` : "Starting position"}</span><b>⌄</b></button><SoftButton className="playback-flip" icon="flip" onClick={onFlip}>Flip</SoftButton></footer>;
}

function OverviewDrawer(props: AnalysisProps) {
  const move = props.selectedMove;
  const analysis = props.selectedGame?.analysis;
  return <aside className="overview-drawer"><header><div><span>Game Overview</span><strong>Evidence behind this review</strong></div><button aria-label="Close overview" onClick={props.onCloseOverview}><Icon name="close"/></button></header><nav>{(["summary", "line", "method"] as InspectorTab[]).map((tab) => <button key={tab} className={props.inspectorTab === tab ? "active" : ""} onClick={() => props.onInspectorTab(tab)}>{titleCase(tab)}</button>)}</nav><div className="drawer-body">
    {props.inspectorTab === "summary" && <><div className="summary-hero"><strong>{analysis?.accuracy.toFixed(1) ?? "—"}</strong><span>review accuracy</span></div><dl className="evidence-list"><div><dt>Opening</dt><dd>{[analysis?.eco, analysis?.opening].filter(Boolean).join(" · ") || "Unclassified"}</dd></div><div><dt>Book depth</dt><dd>{analysis?.book_ply ?? 0} plies</dd></div><div><dt>Selected evaluation</dt><dd>{formatEval(move?.evaluation_after)}</dd></div><div><dt>Classification</dt><dd>{move?.classification || "Pending"}</dd></div></dl></>}
    {props.inspectorTab === "line" && <><div className="engine-line"><span>Principal variation</span><strong>{move?.best_san || move?.best_uci || "—"}</strong><code>{move?.principal_variation.join(" ") || "No line available"}</code></div><dl className="evidence-list"><div><dt>Depth</dt><dd>{move?.depth || "—"}</dd></div><div><dt>Nodes</dt><dd>{move?.nodes?.toLocaleString() || "—"}</dd></div><div><dt>Workers</dt><dd>{props.diagnostics?.engine_workers ?? "—"}</dd></div><div><dt>Deep target</dt><dd>{props.runtimeSettings?.deep_depth ?? "—"}</dd></div></dl></>}
    {props.inspectorTab === "method" && <div className="method-copy"><h3>Local, reproducible evidence</h3><p>C++ reconstructs each position, Stockfish evaluates candidates, and the versioned classification model assigns the displayed label. This panel exposes recorded evidence, not hidden reasoning.</p><dl className="evidence-list"><div><dt>Requested profile</dt><dd>{engineProfileLabel(analysis?.requested_engine_profile)}</dd></div><div><dt>Actual profile</dt><dd>{engineProfileLabel(analysis?.actual_engine_profile)}</dd></div><div><dt>Engine</dt><dd>{analysis?.engine_name || move?.engine_version || "Stockfish local"}</dd></div><div><dt>Source</dt><dd>{engineSourceLabel(analysis?.engine_source)}</dd></div><div><dt>Engine build</dt><dd>{move?.engine_version || "Not recorded"}</dd></div><div><dt>Classifier</dt><dd>{move?.classification_model_version || "Tutor model"}</dd></div></dl></div>}
  </div></aside>;
}

function ExploreView({ games, section, selectedId, onSection, onSelect, onOpen }: { games: StoredGame[]; section: ExploreSection; selectedId: string; onSection: (section: ExploreSection) => void; onSelect: (id: string) => void; onOpen: (id: string, ply: number) => void }) {
  const entries = buildExploreEntries(games);
  const visible = entries.filter((entry) => entry.section === section);
  const selected = visible.find((entry) => entry.id === selectedId) ?? visible[0];
  return <section className="soft-surface explore-surface"><aside className="explore-index"><header><span>Personal library</span><h1>Study positions that came from your games.</h1></header><nav>{(["Openings", "Middlegames", "Endgames"] as ExploreSection[]).map((item) => <button key={item} className={section === item ? "active" : ""} onClick={() => onSection(item)}><span>{item}</span><small>{entries.filter((entry) => entry.section === item).length}</small></button>)}</nav><div className="concept-list">{visible.map((entry) => <button key={entry.id} className={selected?.id === entry.id ? "active" : ""} onClick={() => onSelect(entry.id)}><strong>{entry.title}</strong><span>{entry.tags.slice(0, 2).join(" · ")}</span></button>)}</div></aside>{selected ? <article className="concept-detail"><div className="concept-board"><ChessBoard fen={selected.fen} orientation="white" compact/></div><div className="concept-copy"><span>{selected.section.slice(0, -1)} concept</span><h2>{selected.title}</h2><p>{selected.purpose}</p><small>{selected.source}</small><SoftButton icon="analysis" onClick={() => onOpen(selected.gameId, selected.ply)}>Open in Analysis</SoftButton></div></article> : <div className="empty-state"><Icon name="book"/><h2>No {section.toLowerCase()} yet</h2><p>Analyze more games and this library will assemble itself from real positions.</p></div>}</section>;
}

function ProgressView({ games, profile, onOpen }: { games: StoredGame[]; profile: Profile | null; onOpen: (id: string, ply: number) => void }) {
  const player = inferPlayerName(profile, games);
  const ratings = ratingHistory(games, player);
  const delta = ratingDelta(ratings);
  const latest = ratings[ratings.length - 1];
  const arc = reviewArc(games);
  const min = ratings.length ? Math.min(...ratings.map((point) => point.rating)) : 0;
  const max = ratings.length ? Math.max(...ratings.map((point) => point.rating)) : 1;
  const range = Math.max(1, max - min);
  const line = ratings.map((point, index) => `${ratings.length === 1 ? 50 : index / (ratings.length - 1) * 100},${42 - (point.rating - min) / range * 32}`).join(" ");
  return <section className="soft-surface progress-surface"><header className="progress-heading"><div><span>Rating profile</span><strong>{latest?.rating ?? profile?.latest_rating ?? "—"}</strong><small>{delta === null ? "More dated games needed for a 30-day change" : `${delta >= 0 ? "+" : ""}${delta} over the latest 30-day window`}</small></div><div className="rating-chart">{ratings.length > 1 ? <svg viewBox="0 0 100 48" preserveAspectRatio="none" aria-label="Rating history"><path d="M0 42H100"/><polyline points={line}/>{ratings.map((point, index) => <circle key={point.gameId} cx={index / (ratings.length - 1) * 100} cy={42 - (point.rating - min) / range * 32} r="1.3"/>)}</svg> : <p>Import dated games with rating tags to build this history.</p>}</div></header><div className="progress-grid"><section className="profile-block"><span>Review evidence</span><div className="metric-row"><div><strong>{profile?.games_analyzed ?? games.filter((game) => game.analysis).length}</strong><small>analyzed games</small></div><div><strong>{profile?.total_positions ?? games.flatMap((game) => game.analysis?.moves ?? []).length}</strong><small>classified positions</small></div><div><strong>{profile ? Math.round(profile.drill_accuracy * 100) : "—"}%</strong><small>retry accuracy</small></div></div></section><section className="profile-block weaknesses"><span>Recurring weaknesses</span>{profile?.weaknesses.slice(0, 4).map((item) => <div key={item.category}><strong>{item.category}</strong><small>{item.occurrences} occurrences · {item.average_loss_cp.toFixed(0)} average CP loss</small><em>{Math.round(item.recurrence_rate * 100)}%</em></div>) ?? <p>Analyze multiple games to reveal repeated evidence.</p>}</section><section className="profile-block learning-positions"><span>Positions worth revisiting</span>{arc.slice(0, 5).map((item) => <button key={item.gameId} onClick={() => onOpen(item.gameId, item.largestSwingPly)}><div><strong>{item.title}</strong><small>{item.opening}</small></div><em>{(item.largestSwing * 100).toFixed(1)}% swing</em></button>)}</section></div></section>;
}

function SettingsView({ theme, onTheme, engineLinesDefault, onEngineLines, browserProfile, onBrowserProfile, runtime, diagnostics }: { theme: Theme; onTheme: (theme: Theme) => void; engineLinesDefault: boolean; onEngineLines: (value: boolean) => void; browserProfile: BrowserEngineProfile; onBrowserProfile: (profile: BrowserEngineProfile) => void; runtime: RuntimeSettings | null; diagnostics: Diagnostics | null }) {
  return <section className="soft-surface settings-surface">
    <header className="surface-heading"><div><span>Preferences</span><h1>Shape the workstation, not the chess truth.</h1></div></header>
    <div className="settings-list">
      <section>
        <div><h2>Appearance</h2><p>Follow macOS or keep a deliberate light or dark workspace.</p></div>
        <div className="segmented-control" role="radiogroup" aria-label="Theme">{(["system", "light", "dark"] as Theme[]).map((item) => <label key={item} className={theme === item ? "active" : ""}><input type="radio" name="theme" value={item} checked={theme === item} onChange={() => onTheme(item)}/><span>{titleCase(item)}</span></label>)}</div>
      </section>
      <section>
        <div><h2>Engine evidence</h2><p>Choose whether the technical line is the first tab when opening Overview.</p></div>
        <label className="switch-control"><input type="checkbox" checked={engineLinesDefault} onChange={(event) => onEngineLines(event.target.checked)}/><span/><strong>{engineLinesDefault ? "Shown first" : "Summary first"}</strong></label>
      </section>
      <section>
        <div><h2>Browser engine</h2><p>Choose the free analysis pass. Quick is the safe default; Balanced searches deeper and can use more battery on smaller devices.</p><small className="settings-inline-note">Saved on this device for now. Completed reviews keep the requested and actual profile plus engine source for later reference.</small><div className="settings-notices"><a href="/engine/SOURCE.stockfish.txt" target="_blank" rel="noreferrer">Source record ↗</a><a href="/engine/COPYING.stockfish.txt" target="_blank" rel="noreferrer">GPL-3.0 license ↗</a></div></div>
        <fieldset className="engine-profile-options">
          <legend className="sr-only">Browser analysis profile</legend>
          {browserEngineProfiles().map((profile) => <label key={profile.id} className={`engine-profile-option ${browserProfile === profile.id ? "active" : ""}`}>
            <input type="radio" name="browser-engine-profile" value={profile.id} checked={browserProfile === profile.id} onChange={() => onBrowserProfile(profile.id)}/>
            <span><strong>{profile.label}{profile.id === "quick" && <em>Default</em>}</strong><small>{profile.description}</small><small>Depth {profile.depth} · up to {Math.round(profile.maxAnalysisMs / 1000)} seconds</small></span>
          </label>)}
        </fieldset>
      </section>
      <section>
        <div><h2>Analysis runtime</h2><p>Read-only facts reported by the local C++ service.</p></div>
        <dl className="runtime-grid"><div><dt>Shallow depth</dt><dd>{runtime?.shallow_depth ?? "—"}</dd></div><div><dt>Deep depth</dt><dd>{runtime?.deep_depth ?? "—"}</dd></div><div><dt>Engine workers</dt><dd>{diagnostics?.engine_workers ?? "—"}</dd></div><div><dt>Queue capacity</dt><dd>{diagnostics?.job_queue_capacity ?? "—"}</dd></div></dl>
      </section>
      <section><div><h2>Coaching style</h2><p>A selectable style will appear when C++ exposes a persisted coaching-provider contract.</p></div><span className="unavailable-setting">Unavailable in this build</span></section>
    </div>
  </section>;
}

function ImportModal({ busy, stage, error, onClose, onSubmit }: { busy: boolean; stage: string; error: string; onClose: () => void; onSubmit: (url: string, pgn: string) => void }) {
  const [url, setUrl] = useState("");
  const [pgn, setPgn] = useState("");
  return <div className="modal-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}><form className="import-modal" role="dialog" aria-modal="true" aria-labelledby="import-title" onSubmit={(event) => { event.preventDefault(); onSubmit(url, pgn); }}><header><div><span>Add to Recent Games</span><h2 id="import-title">Bring in a game.</h2><p>Paste a public Chess.com link or PGN. The imported game stays canonical until you choose Analyze.</p></div><button type="button" aria-label="Close import" onClick={onClose}><Icon name="close"/></button></header><label><span>Chess.com game link</span><input type="url" value={url} onChange={(event) => setUrl(event.target.value)} placeholder="https://www.chess.com/game/live/…"/></label><div className="or-rule"><span>or</span></div><label><span>PGN</span><textarea value={pgn} onChange={(event) => setPgn(event.target.value)} placeholder={'[Event "…"]\n\n1. e4 e5 …'}/></label>{error && <p className="form-error" role="alert">{error}</p>}<footer><small>Public game data only · no Chess.com password</small><button disabled={busy}>{busy ? stage || "Importing…" : "Import game"}</button></footer></form></div>;
}

function PlayerIdentityPrompt({ prompt, busy, error, onClose, onDecision }: {
  prompt: IdentityPromptState;
  busy: boolean;
  error: string;
  onClose: () => void;
  onDecision: (decision: PlayerIdentity["decision"], playerName: string) => void;
}) {
  const [selectedName, setSelectedName] = useState(prompt.names[0] ?? "");
  const sourceLabel = prompt.source === "profile_archive" ? "public profile archive" : prompt.source === "public_page" ? "public game page" : "pasted PGN";
  return <div className="modal-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget && !busy) onClose(); }}>
    <section className="identity-modal" role="dialog" aria-modal="true" aria-labelledby="identity-title">
      <header><div><span>Keep your profile accurate</span><h2 id="identity-title">Is this you?</h2><p>We found a player name in this {sourceLabel}. Your review is already saved either way.</p></div><button type="button" aria-label="Close identity prompt" onClick={onClose} disabled={busy}><Icon name="close"/></button></header>
      {prompt.names.length === 0 ? <div className="identity-empty"><strong>No player name was included.</strong><p>You can still analyze this game. Use a tagged PGN or connect a public profile later if you want progress to include it.</p><button type="button" onClick={onClose}>Continue</button></div> : <>
        <fieldset className="identity-names"><legend>Detected player</legend>{prompt.names.map((name) => <label key={name}><input type="radio" name="imported-player" value={name} checked={selectedName === name} onChange={() => setSelectedName(name)}/><span>{name}</span></label>)}</fieldset>
        {error && <p className="form-error" role="alert">{error}</p>}
        <footer><button type="button" onClick={() => onDecision("uncertain", selectedName)} disabled={busy}>I’m not sure</button><button type="button" onClick={() => onDecision("declined", selectedName)} disabled={busy}>Not me</button><button type="button" className="identity-confirm" onClick={() => onDecision("confirmed", selectedName)} disabled={busy}>{busy ? "Saving…" : "Yes, this is me"}</button></footer>
      </>}
    </section>
  </div>;
}

function gameDate(tags: Record<string, string>) {
  const raw = tags.UTCDate || tags.Date || "";
  if (!raw || raw.includes("?")) return "Date unavailable";
  const parsed = new Date(`${raw.replaceAll(".", "-")}T00:00:00Z`);
  return Number.isNaN(parsed.getTime()) ? raw : parsed.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" });
}

function reviewLandingPly(game: StoredGame) {
  const index = game.analysis?.moves.findIndex((move) => needsAttention(move.classification)) ?? -1;
  return Math.max(0, index);
}

function needsAttention(classification: string) {
  return classification === "Inaccuracy" || blockingClassifications.has(classification);
}

function classificationClass(value: string) { return value.toLowerCase().replace(/[^a-z]+/g, "-"); }
function titleCase(value: string) { return value.replace(/\b\w/g, (letter) => letter.toUpperCase()); }
function engineProfileLabel(value?: string) {
  if (value === "quick") return "Quick";
  if (value === "balanced") return "Balanced";
  if (value === "native") return "Native C++";
  return value ? titleCase(value) : "Not recorded";
}
function engineSourceLabel(value?: string) {
  if (value === "browser") return "Browser Stockfish";
  if (value === "cpp") return "C++ service";
  return value ? titleCase(value) : "Not recorded";
}
function delay(ms: number) { return new Promise((resolve) => window.setTimeout(resolve, ms)); }

function isBrowserFallback(error: unknown): boolean {
  return (error instanceof BrowserEngineError && ["unavailable", "timeout", "failed"].includes(error.code)) ||
    (error instanceof ApiError && [404, 503].includes(error.status));
}

function createClaimIdempotencyKey() {
  return typeof crypto.randomUUID === "function"
    ? `guest-claim-${crypto.randomUUID()}`
    : `guest-claim-${Date.now()}-${Math.random().toString(36).slice(2)}`;
}

function dayGreeting(date: Date) {
  const hour = date.getHours();
  if (hour < 12) return "Good morning";
  if (hour < 18) return "Good afternoon";
  return "Good evening";
}

function moveExplanation(move: MoveAssessment) {
  if (move.classification_reasons[0]) return move.classification_reasons[0];
  const loss = `${(move.expected_points_loss * 100).toFixed(1)}%`;
  if (move.classification === "Book") return "This move stays inside the local opening reference.";
  if (["Brilliant", "Great", "Best"].includes(move.classification)) return "This is one of the engine-backed leading choices in the position.";
  if (["Excellent", "Good"].includes(move.classification)) return `The position remains healthy with ${loss} expected-points loss.`;
  if (move.classification === "Inaccuracy") return `A more precise continuation was available; the measured loss is ${loss}.`;
  if (move.classification === "Mistake") return `The move gave up ${loss} expected points and needs a concrete alternative.`;
  if (move.classification === "Miss") return "A forcing opportunity was available and the played move did not use it.";
  return `Deep verification confirmed a decisive swing of ${loss} expected points.`;
}
