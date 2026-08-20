import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  ApiError,
  analyzeVariation,
  cancelJob,
  configureChessComProfile,
  createWebSocketTicket,
  createVariation,
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
import { browserEngineProfiles, normalizeBrowserEngineProfile, type BrowserEngineProfile } from "../engine-profile";
import { BrowserEngineError, createBrowserEngine } from "../browser-engine";
import { runBrowserReview, type BrowserReviewProgress } from "../browser-review";
import { authProviderLabel, clearAuthIntent, consumeAuthRedirectMessage, currentAuthSnapshot, initializeAuth, isAuthCallbackPath, isPasswordResetPath, loadAuthIntent, requestPasswordReset, saveAuthIntent, signInWithLocalAccount, signInWithPassword, signInWithProvider, signOut, signUpWithPassword, subscribeAuth, updatePassword, type AuthEntryMode, type AuthProvider, type AuthSnapshot } from "../auth-session";
import { autoplayDelay, blockingClassifications, completePlaybackDwell, isPlaying, pauseForSelectedMove, startPlayback, type ReviewMode } from "../review";
import type { BoardOrientation } from "../chess";
import type { Diagnostics, Drill, Job, MoveAssessment, PlayerIdentity, Profile, ProgressSocketMessage, RuntimeSettings, StoredGame, Variation, VariationAnalysis } from "../types";
import { eventProtocols, eventUrl } from "../config/runtime";
import { ChessBoard, EvaluationBar, formatEval } from "./Board";
import { Icon } from "./Icon";
import { HomeView } from "./HomeView";
import { LandingView } from "./LandingView";
import { MobileExploreView, MobileHomeView, MobileProgressView, MobileRecentView, MobileSettingsView } from "./MobileViews";
import { AppShell, SoftButton, TopBar, type Route } from "./Shell";
import { AccountPrompt, PasswordResetPrompt } from "./AccountPrompt";
import { betterMoveExplanation, humanMoveExplanation, needsBetterMove } from "../review-copy";
import { isAccountEntryRoute, routeForAuthState, routeForSession, routeFromHash } from "../routes";
import { checkingRetryFeedback, completedRetryFeedback, emptyRetryFeedback, failedRetryFeedback, type RetryFeedback } from "../retry-review";

type Theme = "system" | "light" | "dark";
type InspectorTab = "summary" | "moves" | "line" | "patterns" | "method";
type IdentityPromptState = { gameId: string; names: string[]; source: PlayerIdentity["source"] };
type AnalysisIssue = { kind: "analysis" | "variation"; message: string };

const initialFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
export default function App() {
  const [route, setRoute] = useState<Route>(() => routeFromHash(window.location.hash));
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
  const [retryFeedback, setRetryFeedback] = useState<RetryFeedback>(emptyRetryFeedback);
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
  const [analysisIssue, setAnalysisIssue] = useState<AnalysisIssue | null>(null);
  const [refreshBusy, setRefreshBusy] = useState(false);
  const [refreshMessage, setRefreshMessage] = useState("");
  const [accountPromptOpen, setAccountPromptOpen] = useState(false);
  const [accountEntryMode, setAccountEntryMode] = useState<AuthEntryMode>("sign-up");
  const [authSnapshot, setAuthSnapshot] = useState<AuthSnapshot>(currentAuthSnapshot);
  const [authInitializing, setAuthInitializing] = useState(true);
  const [authMessage, setAuthMessage] = useState("");
  const [authBusy, setAuthBusy] = useState<AuthProvider | null>(null);
  const [passwordResetMessage, setPasswordResetMessage] = useState("");
  const [authRevision, setAuthRevision] = useState(0);
  const [browserAnalysisProgress, setBrowserAnalysisProgress] = useState<BrowserReviewProgress | null>(null);
  const [serviceError, setServiceError] = useState("");
  const autoplayTimer = useRef<number | null>(null);
  const browserEngineRef = useRef<ReturnType<typeof createBrowserEngine> | null>(null);
  const browserAnalysisAbort = useRef<AbortController | null>(null);
  const accountFlowRequested = useRef<"review" | "home" | null>(null);

  const selectedGame = useMemo(() => games.find((game) => game.game.id === selectedGameId) ?? null, [games, selectedGameId]);
  const selectedMove = selectedGame?.analysis?.moves[selectedPly];
  const selectedJob = useMemo(() => jobs.filter((job) => job.game_id === selectedGameId).sort((a, b) => b.id - a.id)[0] ?? null, [jobs, selectedGameId]);
  const analysisInteractionActive = Boolean(
    (browserAnalysisProgress?.gameId === selectedGameId && browserAnalysisProgress) ||
    selectedJob?.status === "queued" ||
    selectedJob?.status === "running"
  );

  useEffect(() => {
    if (selectedJob?.status === "failed" && selectedGame?.analysis_status !== "complete") {
      setAnalysisIssue((current) => current || { kind: "analysis", message: "The analysis service could not finish this review. Try again when you are ready." });
    }
  }, [selectedGame?.analysis_status, selectedJob?.id, selectedJob?.status]);

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
    setServiceError("");
  }, [refreshRuntime]);

  const handleAuthenticatedSession = useCallback(async () => {
    if (!authSnapshot.session) return;
    setAuthBusy(null);
    setAuthMessage("");
    try {
      await refreshAccountLibrary();
    } catch (refreshError) {
      setServiceError(refreshError instanceof Error ? refreshError.message : "Plywise could not reach the account service.");
    }
    if (accountFlowRequested.current) {
      const destination = accountFlowRequested.current;
      setAccountPromptOpen(false);
      setRoute("home");
      if (destination === "review") setImportOpen(true);
      clearAuthIntent();
      accountFlowRequested.current = null;
    }
  }, [authSnapshot.session, refreshAccountLibrary]);

  useEffect(() => {
    const intent = loadAuthIntent();
    if (intent) {
      accountFlowRequested.current = intent.destination;
      setAccountEntryMode(intent.mode);
      setAccountPromptOpen(true);
    } else if (route === "sign-up" || route === "sign-in") {
      accountFlowRequested.current = route === "sign-up" ? "review" : "home";
      setAccountEntryMode(route);
      setAccountPromptOpen(true);
    }
    const redirectMessage = consumeAuthRedirectMessage();
    if (redirectMessage) {
      setAuthMessage(redirectMessage);
      setAccountEntryMode("sign-in");
      setAccountPromptOpen(true);
      setRoute("sign-in");
    }
    const unsubscribe = subscribeAuth((snapshot) => {
      setAuthSnapshot(snapshot);
      if (snapshot.message && snapshot.event !== "SIGNED_OUT" && !snapshot.session) {
        setAuthMessage(snapshot.message);
        setAccountEntryMode("sign-in");
        setAccountPromptOpen(true);
      }
      if (snapshot.event) setAuthRevision((value) => value + 1);
      if (snapshot.event === "SIGNED_OUT") {
        accountFlowRequested.current = null;
        clearAuthIntent();
        setGames([]);
        setJobs([]);
        setProfile(null);
        setDrills([]);
        setIdentityPrompt(null);
        setIdentityError("");
        setSelectedGameId("");
        setRoute("landing");
        setImportOpen(false);
        setAccountPromptOpen(false);
        setAuthBusy(null);
        setServiceError("");
        setAuthMessage(snapshot.message);
      }
    });
    void initializeAuth().then((snapshot) => {
      setAuthSnapshot(snapshot);
      if (snapshot.message && snapshot.event !== "SIGNED_OUT" && !snapshot.session) {
        setAuthMessage(snapshot.message);
        setAccountEntryMode("sign-in");
        setAccountPromptOpen(true);
      }
      setAuthInitializing(false);
    });
    return unsubscribe;
  }, []);

  useEffect(() => () => {
    browserAnalysisAbort.current?.abort();
    browserEngineRef.current?.dispose();
  }, []);

  useEffect(() => {
    if (authSnapshot.session) {
      if (!isPasswordResetPath()) {
        setRoute((current) => routeForSession(current, true));
        setAccountPromptOpen(false);
      }
      void handleAuthenticatedSession();
    } else if (!authInitializing) {
      setRoute((current) => routeForSession(current, false));
      setImportOpen(false);
    }
  }, [authInitializing, authSnapshot.session?.access_token, handleAuthenticatedSession]);

  // A signed-in user should never be left on an account-entry URL, including
  // when a hash changes after the session has already been restored.
  useEffect(() => {
    if (authSnapshot.session && !accountPromptOpen && !isPasswordResetPath() && (route === "landing" || route === "sign-in" || route === "sign-up")) {
      setRoute("home");
    }
  }, [accountPromptOpen, authSnapshot.session, route]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem("pct-theme", theme);
  }, [theme]);

  useEffect(() => {
    localStorage.setItem("pct-browser-engine-profile", browserProfile);
  }, [browserProfile]);

  useEffect(() => {
    window.scrollTo({ top: 0, behavior: "auto" });
    const expected = route === "landing" ? "#/" : `#/${route}`;
    if (window.location.hash !== expected) window.history.replaceState(null, "", expected);
  }, [route]);

  useEffect(() => {
    const onHashChange = () => {
      const requested = routeFromHash(window.location.hash);
      const next = routeForAuthState(requested, Boolean(authSnapshot.session), authInitializing);

      if (!authSnapshot.session && isAccountEntryRoute(requested)) {
        accountFlowRequested.current = requested === "sign-up" ? "review" : "home";
        setAccountEntryMode(requested);
        setAuthMessage("");
        setAccountPromptOpen(true);
      } else if (!authSnapshot.session) {
        accountFlowRequested.current = null;
        setAccountPromptOpen(false);
      }

      setRoute(next);
      const expected = next === "landing" ? "#/" : `#/${next}`;
      if (window.location.hash !== expected) window.history.replaceState(null, "", expected);
    };
    window.addEventListener("hashchange", onHashChange);
    return () => window.removeEventListener("hashchange", onHashChange);
  }, [authInitializing, authSnapshot.session]);

  useEffect(() => {
    if (!authSnapshot.session) return;
    let reconnect = 0;
    let socket: WebSocket | null = null;
    let active = true;
    const connect = async () => {
      let protocols: string[];
      try {
        const ticket = await createWebSocketTicket();
        protocols = eventProtocols(ticket.ticket);
      } catch {
        if (!import.meta.env.DEV) {
          if (active) reconnect = window.setTimeout(() => void connect(), 1500);
          return;
        }
        protocols = eventProtocols();
      }
      if (!active) return;
      socket = new WebSocket(eventUrl("/ws"), protocols);
      socket.addEventListener("message", (event) => {
        const message = JSON.parse(String(event.data)) as ProgressSocketMessage;
        if (message.type === "jobs_snapshot") setJobs(message.jobs);
        if (message.type === "job_update") {
          setJobs((current) => [...current.filter((job) => job.id !== message.job.id), message.job]);
          if (message.job.status === "complete") void refreshGame(message.job.game_id);
          if (message.job.status === "running") void refreshRuntime();
        }
      });
      socket.addEventListener("close", () => {
        if (active) reconnect = window.setTimeout(() => void connect(), 1500);
      });
    };
    void connect();
    return () => {
      active = false;
      window.clearTimeout(reconnect);
      socket?.close();
    };
  }, [authSnapshot.session?.access_token, authRevision, refreshGame, refreshRuntime]);

  const resetTransient = useCallback((ply = selectedPly) => {
    setSelectedPly(ply);
    setReviewMode(pauseForSelectedMove(selectedGame?.analysis?.moves[ply]));
    setHighlightedUci("");
    setTrySource("");
    setRetryFeedback(emptyRetryFeedback);
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
    setAnalysisIssue(null);
    setRoute("analysis");
  }, [games]);

  const startServerFallback = useCallback(async (gameId: string) => {
    const job = await startAnalysis(gameId);
    setJobs((current) => [...current.filter((item) => item.id !== job.id), job]);
    return "Browser analysis is unavailable here, so the hosted fallback is running.";
  }, []);

  const analyzeGame = useCallback(async (gameId: string) => {
    const game = games.find((item) => item.game.id === gameId);
    if (!game || !authSnapshot.session) return;
    openGame(gameId, 0);
    if (game.analysis_status === "complete") return;

    setAnalysisIssue(null);
    setOverviewOpen(false);
    setMoreOpen(false);
    setReviewMode("manual");
    setHighlightedUci("");
    setTrySource("");
    setRetryFeedback(emptyRetryFeedback);
    setVariation(null);
    setVariationMessage("");
    setVariationAnalysis(null);

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
      message: `Starting ${engineProfileLabel(browserProfile)} browser analysis`,
    });
    try {
      await runBrowserReview(game, {
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
      setError("");
      setBrowserAnalysisProgress((current) => current ? { ...current, stage: "finalizing", complete: current.total, message: "Review ready ✓" } : current);
      await delay(450);
      try {
        await refreshGame(gameId);
        setAnalysisIssue(null);
      } catch (refreshError) {
        setAnalysisIssue({ kind: "analysis", message: "The review is ready, but this page could not refresh it yet. Try again in a moment." });
      }
    } catch (analysisError) {
      if (analysisError instanceof BrowserEngineError && analysisError.code === "cancelled") {
        setAnalysisIssue(null);
        return;
      }
      const fallback = isBrowserFallback(analysisError);
      let message = analysisError instanceof ApiError &&
          analysisError.code === "invalid_argument" &&
          analysisError.message.toLowerCase().includes("completed game")
          ? "Only completed games can be analyzed. Import a finished game to continue."
          : fallback
          ? "Browser analysis is unavailable here."
          : "Analysis stopped before the review was ready. Try again.";
      let fallbackStarted = false;
      if (fallback) {
        try {
          message = await startServerFallback(gameId);
          fallbackStarted = true;
        } catch {
          message = "Neither browser nor server analysis could start right now. Try again in a moment.";
        }
      }
      if (!fallbackStarted) setAnalysisIssue({ kind: "analysis", message });
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

  const retryAnalysis = useCallback(async () => {
    if (!selectedGame) return;
    setAnalysisIssue(null);
    if (selectedGame.analysis_status !== "complete") {
      await analyzeGame(selectedGame.game.id);
      return;
    }
    try {
      await refreshGame(selectedGame.game.id);
    } catch {
      setAnalysisIssue({ kind: "analysis", message: "Plywise still could not refresh this review. Try again in a moment." });
    }
  }, [analyzeGame, refreshGame, selectedGame]);

  const refreshGames = useCallback(async () => {
    if (refreshBusy || !authSnapshot.session) return;
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
  }, [authSnapshot.session, games.length, refreshBusy]);

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
    if (!selectedGame || variationBusy) return;
    setVariationReturn({ mode: reviewMode, highlight: highlightedUci });
    setMoreOpen(false);
    setAnalysisIssue(null);
    setVariationBusy(true);
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
    } catch {
      setAnalysisIssue({ kind: "variation", message: "Plywise could not open a variation from this position. Dismiss this message and try Explore again in a moment." });
    } finally {
      setVariationBusy(false);
    }
  }, [highlightedUci, reviewMode, selectedGame, selectedPly, variationBusy]);

  const variationNode = useCallback((id: number | undefined) => variation?.nodes.find((node) => node.id === id), [variation]);

  const recordRetryAttempt = useCallback(async (uci: string) => {
    if (!selectedGame || retryFeedback.status === "checking") return;
    const canonical = uci.trim().toLowerCase();
    setTrySource("");
    setHighlightedUci("");
    setRetryFeedback(checkingRetryFeedback(canonical));
    try {
      const result = await submitReviewAttempt(selectedGame.game.id, selectedPly, canonical);
      setHighlightedUci(canonical);
      setRetryFeedback(completedRetryFeedback(canonical, result.accepted));
    } catch (attemptError) {
      const illegal = attemptError instanceof ApiError && attemptError.code === "illegal_move";
      setRetryFeedback(failedRetryFeedback(illegal));
    }
  }, [retryFeedback.status, selectedGame, selectedPly]);

  const chooseSquare = useCallback(async (square: string) => {
    if ((reviewMode !== "try_move" && reviewMode !== "variation") || variationBusy) return;
    if (!trySource) {
      setTrySource(square);
      reviewMode === "variation"
        ? setVariationMessage(`Selected ${square}. Choose a destination.`)
        : setRetryFeedback({ status: "idle", uci: "", message: `Selected ${square}. Choose a destination.` });
      return;
    }
    const uci = `${trySource}${square}`.toLowerCase();
    setTrySource("");
    if (reviewMode === "try_move" && selectedGame) {
      await recordRetryAttempt(uci);
    }
    if (reviewMode === "variation" && selectedGame && variation) {
      setVariationBusy(true);
      try {
        const next = await extendVariation(selectedGame.game.id, variation.id, variation.current_node_id, uci);
        setVariation(next);
        const node = next.nodes.find((item) => item.id === next.current_node_id);
        setVariationMessage(`${node?.san || node?.uci || uci} added to this branch.`);
        setVariationAnalysis(null);
      } catch (variationError) {
        setVariationMessage(variationError instanceof ApiError && variationError.code === "illegal_move" ? "That move is illegal in this position." : "Plywise could not add that move to the branch. Try again.");
      } finally {
        setVariationBusy(false);
      }
    }
  }, [recordRetryAttempt, reviewMode, selectedGame, trySource, variation, variationBusy]);

  const submitRetry = useCallback(async (uci: string) => {
    await recordRetryAttempt(uci);
  }, [recordRetryAttempt]);

  const leaveVariation = useCallback(async (remove = false) => {
    if (variationBusy) return;
    if (remove && variation && selectedGame) {
      if (!window.confirm("Delete this saved variation and all branches?")) return;
      setVariationBusy(true);
      try {
        await deleteVariation(selectedGame.game.id, variation.id);
      } catch {
        setVariationMessage("Plywise could not delete this variation. The branch is still saved.");
        return;
      } finally {
        setVariationBusy(false);
      }
    }
    setVariation(null);
    setVariationAnalysis(null);
    setVariationMessage("");
    setReviewMode(variationReturn.mode);
    setHighlightedUci(variationReturn.highlight);
    setTrySource("");
  }, [selectedGame, variation, variationBusy, variationReturn]);

  const resetActiveVariation = useCallback(async () => {
    if (!selectedGame || !variation || variationBusy) return;
    setVariationBusy(true);
    try {
      const next = await resetVariation(selectedGame.game.id, variation.id);
      setVariation(next);
      setVariationAnalysis(null);
      setVariationMessage("Variation reset to its canonical root.");
    } catch {
      setVariationMessage("Plywise could not reset this branch. Your current variation is unchanged.");
    } finally {
      setVariationBusy(false);
    }
  }, [selectedGame, variation, variationBusy]);

  const stepVariationBack = useCallback(async () => {
    if (!selectedGame || !variation || variationBusy) return;
    const parentId = variationNode(variation.current_node_id)?.parent_id ?? -1;
    if (parentId < 0) return;
    setVariationBusy(true);
    try {
      const next = await setVariationCursor(selectedGame.game.id, variation.id, parentId);
      setVariation(next);
      setVariationAnalysis(null);
      setVariationMessage(parentId === 0 ? "Back at the variation root." : "Previous branch position restored.");
    } catch {
      setVariationMessage("Plywise could not move back in this branch. Your current position is unchanged.");
    } finally {
      setVariationBusy(false);
    }
  }, [selectedGame, variation, variationBusy, variationNode]);

  const analyzeActiveVariation = useCallback(async () => {
    if (!selectedGame || !variation || variationBusy) return;
    setVariationBusy(true);
    setVariationMessage("Stockfish is evaluating this branch at background priority.");
    try {
      setVariationAnalysis(await analyzeVariation(selectedGame.game.id, variation.id));
      setVariationMessage("Branch evaluation ready. The canonical game is unchanged.");
    } catch {
      setVariationMessage("Plywise could not analyze this branch right now. The variation is still saved.");
    } finally { setVariationBusy(false); }
  }, [selectedGame, variation, variationBusy]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (route !== "analysis" || analysisInteractionActive || event.defaultPrevented || event.isComposing || event.metaKey || event.ctrlKey || event.altKey) return;
      const target = event.target instanceof Element ? event.target : null;
      if (target?.closest("button, a[href], input, textarea, select, summary, .action-menu, [role='dialog'], [contenteditable='true']")) return;
      if (event.key === "ArrowLeft") { event.preventDefault(); navigate("previous"); }
      if (event.key === "ArrowRight") { event.preventDefault(); navigate("next"); }
      if (event.key === "Home") { event.preventDefault(); navigate("first"); }
      if (event.key === "End") { event.preventDefault(); navigate("last"); }
      if (event.key === " ") { event.preventDefault(); togglePlayback(); }
      if (event.key.toLowerCase() === "f") setOrientation((value) => value === "white" ? "black" : "white");
      if (event.key.toLowerCase() === "r" && selectedMove) { setReviewMode("try_move"); setHighlightedUci(""); setTrySource(""); setRetryFeedback(emptyRetryFeedback); }
      if (event.key.toLowerCase() === "v" && selectedMove) void startVariation("before");
      if (event.key.toLowerCase() === "b" && selectedMove) { setReviewMode("revealed_move"); setHighlightedUci(selectedMove.best_uci); }
      if (event.key === "Escape") { if (variation) void leaveVariation(); else resetTransient(); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [analysisInteractionActive, leaveVariation, navigate, resetTransient, route, selectedMove, startVariation, togglePlayback, variation]);

  const openAccountPrompt = useCallback((mode: AuthEntryMode = "sign-up") => {
    accountFlowRequested.current = mode === "sign-up" ? "review" : "home";
    setAccountEntryMode(mode);
    setAccountPromptOpen(true);
    setRoute(mode);
    setAuthMessage("");
  }, []);

  const runImport = useCallback(async (url: string, pgn: string) => {
    if (!authSnapshot.session) {
      setImportOpen(false);
      openAccountPrompt("sign-up");
      return;
    }
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
  }, [authSnapshot.session, openAccountPrompt, refreshGame]);

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
      if (decision === "confirmed" && identityPrompt.source !== "pgn") {
        const connection = await configureChessComProfile({ username: playerName });
        setChessComConnected(connection.connected);
      }
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
    if (!authSnapshot.session) {
      openAccountPrompt();
      return;
    }
    setRoute(next);
    setMoreOpen(false);
    setOverviewOpen(false);
    if (next === "settings") void refreshRuntime();
  }, [authSnapshot.session, openAccountPrompt, refreshRuntime]);

  const startLandingReview = useCallback(() => {
    openAccountPrompt("sign-up");
  }, [openAccountPrompt]);

  const startProviderSignIn = useCallback(async (provider: AuthProvider) => {
    setAuthBusy(provider);
    setAuthMessage(accountEntryMode === "sign-up"
      ? `Setting up your account with ${authProviderLabel(provider)}…`
      : `Connecting to ${authProviderLabel(provider)}…`);
    saveAuthIntent({
      context: "landing",
      mode: accountEntryMode,
      destination: accountEntryMode === "sign-up" ? "review" : "home",
    });
    const result = await signInWithProvider(provider, accountEntryMode);
    setAuthMessage(result.message);
    if (!result.ok) {
      clearAuthIntent();
      setAuthBusy(null);
    }
  }, [accountEntryMode]);

  const startPasswordSignUp = useCallback(async (name: string, email: string, password: string) => {
    const result = await signUpWithPassword(name, email, password);
    setAuthMessage(result.message);
    return result;
  }, []);

  const startLocalSignIn = useCallback(async () => {
    const result = await signInWithLocalAccount();
    setAuthMessage(result.message);
    if (!result.ok) setError(result.message);
  }, []);

  const startPasswordSignIn = useCallback(async (email: string, password: string) => {
    const result = await signInWithPassword(email, password);
    setAuthMessage(result.message);
    return result;
  }, []);

  const startPasswordReset = useCallback(async (email: string) => {
    const result = await requestPasswordReset(email);
    setAuthMessage(result.message);
    return result;
  }, []);

  const completePasswordReset = useCallback(async (password: string) => {
    const result = await updatePassword(password);
    setPasswordResetMessage(result.message);
    return result;
  }, []);

  const finishPasswordReset = useCallback(() => {
    window.history.replaceState(null, "", "/");
    setPasswordResetMessage("");
    setRoute("home");
  }, []);

  const handleSignOut = useCallback(async () => {
    setAuthBusy(null);
    const result = await signOut();
    setAuthMessage(result.message);
    if (!result.ok) setError(result.message);
  }, []);

  const cancelPasswordReset = useCallback(() => {
    window.history.replaceState(null, "", "/");
    setPasswordResetMessage("");
    void handleSignOut();
  }, [handleSignOut]);

  const closeAccountPrompt = useCallback(() => {
    accountFlowRequested.current = null;
    clearAuthIntent();
    setAccountPromptOpen(false);
    if (authSnapshot.session) {
      if (route === "sign-in" || route === "sign-up") setRoute("home");
    } else {
      setRoute("landing");
    }
  }, [authSnapshot.session, route]);

  const accountPrompt = accountPromptOpen && <AccountPrompt
    auth={authSnapshot}
    mode={accountEntryMode}
    presentation={authSnapshot.session ? "modal" : "page"}
    busyProvider={authBusy}
    message={authMessage}
    onProvider={(provider) => void startProviderSignIn(provider)}
    onLocalSignIn={() => void startLocalSignIn()}
    onModeChange={(mode) => { setAccountEntryMode(mode); setAuthMessage(""); setRoute(mode); accountFlowRequested.current = mode === "sign-up" ? "review" : "home"; }}
    onPasswordSignUp={startPasswordSignUp}
    onPasswordSignIn={startPasswordSignIn}
    onPasswordReset={startPasswordReset}
    onSignOut={() => void handleSignOut()}
    onClose={closeAccountPrompt}
  />;

  if (authInitializing && (isAuthCallbackPath() || isPasswordResetPath())) {
    return <main className="account-page"><div className="account-page-shell"><p className="account-loading" role="status" aria-live="polite">Finishing your account session…</p></div></main>;
  }

  if (isPasswordResetPath() && authSnapshot.session) {
    return <PasswordResetPrompt auth={authSnapshot} message={passwordResetMessage} onSubmit={completePasswordReset} onCancel={cancelPasswordReset} onComplete={finishPasswordReset} />;
  }

  if (!authSnapshot.session) {
    return accountPromptOpen || route === "sign-up" || route === "sign-in"
      ? accountPrompt
      : <LandingView onStart={startLandingReview} onSignIn={() => openAccountPrompt("sign-in")}/>;
  }

  if (serviceError) {
    return <><ServiceUnavailableView message={serviceError} onRetry={() => { setServiceError(""); void handleAuthenticatedSession(); }} onSignOut={() => void handleSignOut()}/>{accountPrompt}</>;
  }

  if (route === "landing") {
    return <><LandingView onStart={startLandingReview} onSignIn={() => openAccountPrompt("sign-in")}/>{accountPrompt}</>;
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
    view = <>
      <div className="desktop-route-view"><HomeView
        games={games}
        jobs={jobs}
        profile={profile}
        drills={drills}
        refreshBusy={refreshBusy}
        refreshMessage={refreshMessage}
        onOpen={openGame}
        onRecent={() => setAppRoute("recent")}
        onImport={() => setImportOpen(true)}
        onPractice={(drill) => openGame(drill.source_game_id, drill.source_ply)}
      /></div>
      <MobileHomeView
        games={games}
        profile={profile}
        drills={drills}
        refreshBusy={refreshBusy}
        refreshMessage={refreshMessage}
        onOpen={openGame}
        onRecent={() => setAppRoute("recent")}
        onImport={() => setImportOpen(true)}
        onPractice={(drill) => openGame(drill.source_game_id, drill.source_ply)}
        onRefresh={() => void refreshGames()}
        onSettings={() => setAppRoute("settings")}
      />
    </>;
  } else if (route === "recent") {
    header = <TopBar title="Recent Games" detail={`${games.length} local game${games.length === 1 ? "" : "s"}`} icon="recent"/>;
    view = <>
      <div className="desktop-route-view"><RecentView
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
      /></div>
      <MobileRecentView
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
        onSettings={() => setAppRoute("settings")}
      />
    </>;
  } else if (route === "analysis") {
    const tags = selectedGame?.game.tags ?? {};
    const gameName = selectedGame ? `${tags.White ?? "White"} vs. ${tags.Black ?? "Black"}` : "No game selected";
    const opening = selectedGame?.analysis ? `${selectedGame.analysis.opening} · ${selectedGame.analysis.eco}` : "Choose a recent game";
    const activeBrowserProgress = selectedGame && browserAnalysisProgress?.gameId === selectedGame.game.id ? browserAnalysisProgress : null;
    header = <TopBar icon="analysis" title={gameName} detail={opening} meta={analysisInteractionActive || analysisIssue ? undefined : selectedGame?.analysis ? formatAccuracy(selectedGame.analysis.accuracy, 1, " accuracy") : undefined} actions={!analysisInteractionActive && !analysisIssue ? <>
      {selectedGame && selectedGame.analysis_status !== "complete" && <SoftButton onClick={() => void analyzeGame(selectedGame.game.id)}>Analyze</SoftButton>}
      <SoftButton icon="overview" aria-expanded={overviewOpen} aria-controls="analysis-overview" onClick={() => { setInspectorTab("summary"); setOverviewOpen((value) => !value); }}>Overview</SoftButton>
      <div className="more-wrap"><SoftButton icon="more" aria-label="More analysis actions" aria-expanded={moreOpen} onClick={() => setMoreOpen((value) => !value)}/>{moreOpen && <div className="action-menu">
        <button onClick={() => { setReviewMode("try_move"); setHighlightedUci(""); setTrySource(""); setRetryFeedback(emptyRetryFeedback); setMoreOpen(false); }}><Icon name="retry"/>Retry this move</button>
        <button onClick={() => void startVariation("before")}><Icon name="branch"/>Explore variation</button>
        <button onClick={() => { setReviewMode("revealed_move"); setHighlightedUci(selectedMove?.best_uci ?? ""); setMoreOpen(false); }}><Icon name="star"/>Show best move</button>
        <button onClick={() => { setOrientation((value) => value === "white" ? "black" : "white"); setMoreOpen(false); }}><Icon name="flip"/>Flip board</button>
      </div>}</div>
    </> : undefined}/>;
    view = <AnalysisView
      {...shared}
      orientation={orientation}
      reviewMode={reviewMode}
      highlightedUci={highlightedUci}
      trySource={trySource}
      retryFeedback={retryFeedback}
      variation={variation}
      variationMessage={variationMessage}
      variationAnalysis={variationAnalysis}
      variationBusy={variationBusy}
      overviewOpen={overviewOpen}
      inspectorTab={inspectorTab}
      moveListExpanded={moveListExpanded}
      runtimeSettings={runtimeSettings}
      diagnostics={diagnostics}
      analysisActive={analysisInteractionActive}
      analysisIssue={analysisIssue}
      browserProgress={activeBrowserProgress}
      onSelectPly={resetTransient}
      onNavigate={navigate}
      onTogglePlayback={togglePlayback}
      onFlip={() => setOrientation((value) => value === "white" ? "black" : "white")}
      onSquare={(square) => void chooseSquare(square)}
      onRetry={() => { setReviewMode("try_move"); setHighlightedUci(""); setTrySource(""); setRetryFeedback(emptyRetryFeedback); }}
      onRetrySubmit={(uci) => void submitRetry(uci)}
      onRetryAgain={() => { setHighlightedUci(""); setTrySource(""); setRetryFeedback(emptyRetryFeedback); }}
      onRevealRetry={() => { setReviewMode("revealed_move"); setHighlightedUci(selectedMove?.best_uci ?? ""); setTrySource(""); setRetryFeedback(emptyRetryFeedback); }}
      onContinueRetry={() => navigate("next")}
      onVariation={() => void startVariation("before")}
      onReturn={() => variation ? void leaveVariation() : resetTransient()}
      onVariationBack={() => void stepVariationBack()}
      onVariationReset={() => void resetActiveVariation()}
      onVariationAnalyze={() => void analyzeActiveVariation()}
      onVariationDelete={() => void leaveVariation(true)}
      onToggleMoves={() => setMoveListExpanded((value) => !value)}
      onCloseOverview={() => setOverviewOpen(false)}
      onOpenOverview={() => { setInspectorTab("summary"); setOverviewOpen(true); }}
      onInspectorTab={setInspectorTab}
      moreOpen={moreOpen}
      onToggleMore={() => setMoreOpen((value) => !value)}
      onCloseMore={() => setMoreOpen(false)}
      onBack={() => setAppRoute("recent")}
      onShowBestMove={() => { setReviewMode("revealed_move"); setHighlightedUci(selectedMove?.best_uci ?? ""); setMoreOpen(false); }}
      onCancelJob={() => selectedJob && void cancelJob(selectedJob.id).then((job) => setJobs((current) => [...current.filter((item) => item.id !== job.id), job]))}
      onCancelBrowserAnalysis={cancelBrowserAnalysis}
      onRetryAnalysis={() => void retryAnalysis()}
      onDismissAnalysisError={() => setAnalysisIssue(null)}
    />;
  } else if (route === "explore") {
    const entries = buildExploreEntries(games);
    header = <TopBar title="Explore" detail="Your position library" meta={`${entries.length} concepts`} icon="explore"/>;
    view = <>
      <div className="desktop-route-view"><ExploreView games={games} section={exploreSection} selectedId={selectedExploreId} onSection={setExploreSection} onSelect={setSelectedExploreId} onOpen={openGame}/></div>
      <MobileExploreView games={games} profile={profile} section={exploreSection} onSection={setExploreSection} onOpen={openGame} onSettings={() => setAppRoute("settings")}/>
    </>;
  } else if (route === "progress") {
    const player = inferPlayerName(profile, games);
    header = <TopBar title="Progress" detail={player || "Local player profile"} meta={`${profile?.games_analyzed ?? games.filter((game) => game.analysis).length} analyzed`} icon="progress"/>;
    view = <>
      <div className="desktop-route-view"><ProgressView games={games} profile={profile} onOpen={openGame}/></div>
      <MobileProgressView games={games} profile={profile} onOpen={openGame} onSettings={() => setAppRoute("settings")}/>
    </>;
  } else {
    header = <TopBar title="Settings" detail="Local preferences" icon="settings"/>;
    const updateEngineLines = (value: boolean) => { setEngineLinesDefault(value); setInspectorTab(value ? "line" : "summary"); localStorage.setItem("pct-engine-lines-default", String(value)); };
    view = <>
      <div className="desktop-route-view"><SettingsView theme={theme} onTheme={setTheme} engineLinesDefault={engineLinesDefault} onEngineLines={updateEngineLines} browserProfile={browserProfile} onBrowserProfile={setBrowserProfile} runtime={runtimeSettings} diagnostics={diagnostics} accountLabel={accountDisplayName(authSnapshot)} onSignOut={() => void handleSignOut()}/></div>
      <MobileSettingsView theme={theme} onTheme={setTheme} engineLinesDefault={engineLinesDefault} onEngineLines={updateEngineLines} browserProfile={browserProfile} onBrowserProfile={setBrowserProfile} runtime={runtimeSettings} diagnostics={diagnostics} accountLabel={accountDisplayName(authSnapshot)} onSignOut={() => void handleSignOut()} onBack={() => setAppRoute("home")}/>
    </>;
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

type RecentFilter = "all" | "needs-review" | "reviewed";

type RecentGameRow = {
  stored: StoredGame;
  opponent: string;
  opening: string;
  date: string;
  ratings: string;
  timeControl: string;
  status: string;
  statusDetail: string;
  active: boolean;
  reviewed: boolean;
};

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
  const [filter, setFilter] = useState<RecentFilter>("all");
  const player = inferPlayerName(profile, games).toLowerCase();
  const rows: RecentGameRow[] = games.map((stored) => {
    const tags = stored.game.tags;
    const white = tags.White ?? "White";
    const black = tags.Black ?? "Black";
    const isWhite = player && white.toLowerCase() === player;
    const isBlack = player && black.toLowerCase() === player;
    const opponent = isWhite ? black : isBlack ? white : `${white} vs. ${black}`;
    const latestJob = jobs.filter((job) => job.game_id === stored.game.id).sort((a, b) => b.id - a.id)[0];
    const active = latestJob?.status === "running" || latestJob?.status === "queued";
    const status = active
      ? latestJob.status === "running" ? "Analyzing" : "Queued"
      : latestJob?.status === "failed" ? "Failed"
        : stored.analysis_status === "complete" ? "Reviewed"
          : stored.analysis_status === "shallow" ? "Partial" : "Ready";
    const reviewed = stored.analysis_status === "complete";
    return {
      stored,
      opponent,
      opening: stored.analysis?.opening || "Opening appears after analysis",
      date: gameDate(tags),
      ratings: [tags.WhiteElo, tags.BlackElo].filter(Boolean).join(" / ") || "Ratings unavailable",
      timeControl: tags.TimeControl || "Time control unavailable",
      status,
      statusDetail: active ? latestJob.progress.message : latestJob?.status === "failed" ? latestJob.error || "Analysis failed. Try again." : reviewed ? "Analysis is ready to revisit" : "Not reviewed yet",
      active,
      reviewed,
    };
  });
  const filteredRows = rows.filter((row) => filter === "all" || (filter === "reviewed" ? row.reviewed : !row.reviewed));
  const reviewedCount = rows.filter((row) => row.reviewed).length;
  return <section className="soft-surface recent-surface">
    <header className="recent-header"><div><span>Game library</span><h1>Your recent games.</h1><p>Every game you bring in stays here until you decide what to review next.</p></div><div className="recent-header-actions"><small>{games.length} imported · {reviewedCount} reviewed</small><SoftButton icon="import" onClick={onImport}>Import game</SoftButton></div></header>
    <div className="recent-toolbar"><div className="recent-filters" role="group" aria-label="Filter games">
      {(["all", "needs-review", "reviewed"] as const).map((item) => <button key={item} type="button" className={filter === item ? "active" : ""} aria-pressed={filter === item} onClick={() => setFilter(item)}>{item === "all" ? "All games" : item === "needs-review" ? "Needs review" : "Reviewed"}</button>)}
    </div>{selected.size > 0 ? <div className="selection-actions"><strong>{selected.size} selected</strong><button onClick={onAnalyzeSelected}>Analyze selected</button><button onClick={onClear}>Clear</button></div> : <span className="recent-toolbar-note">Select games to queue a batch review.</span>}</div>
    <div className="recent-game-list" role="list" aria-label="Recent games">
      {filteredRows.map((row) => <article key={row.stored.game.id} className={`recent-game-card ${selected.has(row.stored.game.id) ? "selected" : ""}`} role="listitem">
        <label className="recent-select"><input type="checkbox" aria-label={`Select ${row.opponent}`} checked={selected.has(row.stored.game.id)} onChange={() => onSelect(row.stored.game.id)}/><span aria-hidden="true" /></label>
        <button className="recent-game-main" onClick={() => onOpen(row.stored.game.id, reviewLandingPly(row.stored))}>
          <span className="recent-result" aria-label={`Result ${row.stored.game.tags.Result ?? "unknown"}`}>{row.stored.game.tags.Result ?? "*"}</span>
          <span className="recent-game-copy"><strong>{row.opponent}</strong><span>{row.opening}</span><small>{row.date} · {row.timeControl}</small></span>
        </button>
        <div className="recent-game-meta"><span>{row.ratings}</span><small>{row.statusDetail}</small></div>
        <div className={`recent-game-status status-${row.status.toLowerCase()}`}><strong>{row.status}</strong><span>{row.reviewed ? "Open anytime" : "Ready when you are"}</span></div>
        <button className="recent-game-action" disabled={row.active} onClick={() => row.reviewed ? onOpen(row.stored.game.id, reviewLandingPly(row.stored)) : onAnalyze(row.stored.game.id)}>{row.reviewed ? "Open review" : row.active ? row.status : "Analyze"}<span aria-hidden="true">→</span></button>
      </article>)}
      {!games.length && <div className="empty-state"><Icon name="import"/><h2>No games yet</h2><p>Import a public Chess.com game or PGN. Analysis begins only when you choose it.</p><SoftButton icon="import" onClick={onImport}>Import your first game</SoftButton></div>}
      {games.length > 0 && !filteredRows.length && <div className="empty-state recent-filter-empty"><h2>No games in this view</h2><p>Try another filter or bring in a new game to keep your library moving.</p><button type="button" onClick={() => setFilter("all")}>Show all games</button></div>}
    </div>
  </section>;
}

type AnalysisProps = {
  games: StoredGame[]; profile: Profile | null; selectedGame: StoredGame | null; selectedPly: number; selectedMove?: MoveAssessment; jobs: Job[]; selectedJob: Job | null;
  orientation: BoardOrientation; reviewMode: ReviewMode; highlightedUci: string; trySource: string; retryFeedback: RetryFeedback; variation: Variation | null; variationMessage: string; variationAnalysis: VariationAnalysis | null; variationBusy: boolean;
  overviewOpen: boolean; inspectorTab: InspectorTab; moveListExpanded: boolean; runtimeSettings: RuntimeSettings | null; diagnostics: Diagnostics | null; analysisActive: boolean; analysisIssue: AnalysisIssue | null; browserProgress: BrowserReviewProgress | null; moreOpen: boolean;
  onSelectPly: (ply: number) => void; onNavigate: (action: "first" | "previous" | "next" | "last") => void; onTogglePlayback: () => void; onFlip: () => void; onSquare: (square: string) => void;
  onRetry: () => void; onRetrySubmit: (uci: string) => void; onRetryAgain: () => void; onRevealRetry: () => void; onContinueRetry: () => void; onVariation: () => void; onReturn: () => void; onVariationBack: () => void; onVariationReset: () => void; onVariationAnalyze: () => void; onVariationDelete: () => void;
  onToggleMoves: () => void; onCloseOverview: () => void; onOpenOverview: () => void; onInspectorTab: (tab: InspectorTab) => void; onToggleMore: () => void; onCloseMore: () => void; onBack: () => void; onShowBestMove: () => void; onCancelJob: () => void; onCancelBrowserAnalysis: () => void; onRetryAnalysis: () => void; onDismissAnalysisError: () => void;
};

function AnalysisView(props: AnalysisProps) {
  const { selectedGame: game, selectedMove: move, selectedPly, reviewMode, variation } = props;
  if (!game) return <section className="soft-surface analysis-empty"><Icon name="analysis"/><h1>Choose a game to analyze.</h1><p>Open a game from Recent Games. Its canonical moves remain unchanged while you review or branch.</p></section>;
  const ply = game.game.plies[selectedPly];
  const analysisActive = props.analysisActive;
  const livePlyIndex = props.browserProgress?.ply ?? selectedPly;
  const livePly = game.game.plies[livePlyIndex] ?? ply;
  const currentVariationNode = variation?.nodes.find((node) => node.id === variation.current_node_id);
  const fen = analysisActive ? (props.browserProgress?.position === "before" ? livePly?.fen_before : livePly?.fen_after) ?? livePly?.fen_after ?? initialFen : reviewMode === "variation" ? currentVariationNode?.fen ?? variation?.root_fen ?? initialFen : reviewMode === "try_move" || reviewMode === "revealed_move" ? move?.fen_before ?? ply?.fen_before ?? initialFen : ply?.fen_after ?? initialFen;
  const retryResolved = props.retryFeedback.status === "correct" || props.retryFeedback.status === "incorrect";
  const activeUci = analysisActive
    ? ""
    : reviewMode === "try_move"
      ? props.retryFeedback.uci
    : reviewMode === "variation"
      ? currentVariationNode?.uci ?? ""
      : props.highlightedUci || ply?.uci || "";
  return <div className="analysis-view">
    <div className={`analysis-layout analysis-desktop-layout ${analysisActive ? "analysis-active" : ""}`}>
    {analysisActive && <AnalysisActivity
      game={game}
      progress={props.browserProgress}
      job={props.selectedJob}
      onCancel={props.browserProgress ? props.onCancelBrowserAnalysis : props.onCancelJob}
    />}
    <section className={`board-surface ${reviewMode === "variation" ? "variation-active" : ""}`}>
      <EvaluationBar value={analysisActive ? undefined : move?.evaluation_after}/>
      <div className="board-holder"><ChessBoard fen={fen} orientation={props.orientation} activeUci={activeUci} sourceSquare={props.trySource} interactive={!analysisActive && !props.variationBusy && (reviewMode === "variation" || (reviewMode === "try_move" && !retryResolved && props.retryFeedback.status !== "checking"))} showArrow={!analysisActive && (reviewMode === "revealed_move" || (reviewMode === "try_move" && props.retryFeedback.status === "correct"))} onSquare={props.onSquare}/></div>
    </section>
    <ReviewInspector {...props} analysisActive={analysisActive}/>
    {!analysisActive && !props.analysisIssue && <Playback game={game} selectedPly={selectedPly} playing={isPlaying(reviewMode)} onNavigate={props.onNavigate} onPlay={props.onTogglePlayback} onFlip={props.onFlip}/>}
    {!analysisActive && !props.analysisIssue && props.overviewOpen && (
      <OverviewDrawer {...props}/>
    )}
    </div>
    <MobileAnalysisView {...props} game={game} move={move} fen={fen} livePly={livePlyIndex} analysisActive={analysisActive}/>
  </div>;
}

function MobileAnalysisView(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const tags = props.game.game.tags;
  const title = `${tags.White ?? "White"} vs. ${tags.Black ?? "Black"}`;
  const opening = props.game.analysis?.opening ?? "Game review";
  const accuracy = props.game.analysis ? formatAccuracy(props.game.analysis.accuracy, 0, "% accuracy") : "Review in progress";
  return <section className="mobile-analysis-view" aria-label="Mobile game review">
    <header className="mobile-analysis-header">
      <button className="mobile-back-button" aria-label="Back to recent games" onClick={props.onBack}><Icon name="previous"/></button>
      <div className="mobile-analysis-identity"><strong>{title}</strong><span>{opening} · {accuracy}</span></div>
      {props.analysisActive || props.analysisIssue ? <span aria-hidden="true"/> : <button className="mobile-more-button" aria-label="More analysis actions" aria-expanded={props.moreOpen} onClick={props.onToggleMore}><Icon name="more"/></button>}
    </header>
    {props.analysisActive ? <>
      <MobileAnalysisActivity {...props}/>
      <MobileBoardSurface {...props} evaluation={undefined}/>
      <p className="mobile-live-position">{mobilePositionLabel(props.game, props.browserProgress, props.livePly)}</p>
    </> : <>
      <MobileBoardSurface {...props} evaluation={props.move?.evaluation_after}/>
      {props.analysisIssue ? <AnalysisErrorNotice issue={props.analysisIssue} onRetry={props.onRetryAnalysis} onDismiss={props.onDismissAnalysisError}/> : <>
      <MobilePlayback {...props}/>
      {props.reviewMode === "try_move" || props.reviewMode === "variation"
        ? <MobileReviewMode {...props}/>
        : <MobileReviewSummary {...props}/>}
      {props.overviewOpen && <MobileAnalysisDrawer {...props}/>}
      </>}
    </>}
    {!props.analysisActive && !props.analysisIssue && props.moreOpen ? <MobileActionSheet {...props}/> : null}
  </section>;
}

function MobileReviewMode(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const move = props.move;
  if (props.reviewMode === "try_move") {
    return <section className="mobile-review-mode" aria-live="polite">
      <header><div><span>Retry move</span><strong>Find a stronger continuation</strong></div><button onClick={props.onReturn}>Return</button></header>
      <p>The board is restored before {move?.move_number}{move?.side === "black" ? "…" : "."} {move?.played_san || move?.san || "this move"}. Choose a piece, then its destination.</p>
      {props.retryFeedback.status !== "correct" && props.retryFeedback.status !== "incorrect" && <RetryForm feedback={props.retryFeedback} onSubmit={props.onRetrySubmit}/>}
      <RetryFeedbackPanel feedback={props.retryFeedback} onRetryAgain={props.onRetryAgain} onReveal={props.onRevealRetry} onContinue={props.onContinueRetry}/>
    </section>;
  }
  return <section className="mobile-review-mode" aria-live="polite">
    <header><div><span>Variation</span><strong>Explore a legal branch</strong></div><button disabled={props.variationBusy} onClick={props.onReturn}>Return</button></header>
    <p>{props.variationMessage || "Choose a piece, then its destination."}</p>
    {props.variationAnalysis && <div className="mobile-variation-line"><strong>{props.variationAnalysis.best_move || "—"}</strong><code>{props.variationAnalysis.lines[0]?.moves.join(" ")}</code></div>}
    <div className="mobile-mode-actions"><button disabled={props.variationBusy} onClick={props.onVariationBack}>Back</button><button disabled={props.variationBusy} onClick={props.onVariationReset}>Reset</button><button disabled={props.variationBusy} onClick={props.onVariationAnalyze}>{props.variationBusy ? "Working…" : "Analyze"}</button><button disabled={props.variationBusy} className="danger-text" onClick={props.onVariationDelete}>Delete</button></div>
  </section>;
}

function MobileBoardSurface(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean; evaluation?: number }) {
  const retryResolved = props.retryFeedback.status === "correct" || props.retryFeedback.status === "incorrect";
  const interactive = !props.analysisActive && !props.variationBusy && (props.reviewMode === "variation" || (props.reviewMode === "try_move" && !retryResolved && props.retryFeedback.status !== "checking"));
  return <section className="mobile-board-surface" aria-label="Chess board and evaluation">
    <EvaluationBar value={props.evaluation}/>
    <div className="mobile-board-holder"><ChessBoard fen={props.fen} orientation={props.orientation} activeUci={props.analysisActive ? "" : props.reviewMode === "try_move" ? props.retryFeedback.uci : props.highlightedUci} sourceSquare={props.trySource} interactive={interactive} showArrow={!props.analysisActive && (props.reviewMode === "revealed_move" || (props.reviewMode === "try_move" && props.retryFeedback.status === "correct"))} onSquare={props.onSquare}/></div>
  </section>;
}

function MobileAnalysisActivity(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const complete = props.browserProgress?.complete ?? props.selectedJob?.progress.complete ?? 0;
  const total = props.browserProgress?.total ?? props.selectedJob?.progress.total ?? 0;
  const percent = progressPercent(complete, total);
  const detail = props.browserProgress?.message ?? props.selectedJob?.progress.message ?? "Scanning positions";
  return <section className="mobile-analysis-activity" aria-label="Analysis in progress" aria-live="polite">
    <div className="mobile-analysis-activity-row"><strong>{props.browserProgress?.stage === "finalizing" && percent === 100 ? "Review ready ✓" : "Analyzing your game"}</strong><b>{percent}%</b></div>
    <p>{detail}</p>
    <progress className="mobile-analysis-progress" aria-label="Analysis progress" max={100} value={percent}/>
    <div className="mobile-analysis-activity-footer"><span>Stockfish · {props.browserProgress?.profile ? "Local" : "Server"}</span><button onClick={props.browserProgress ? props.onCancelBrowserAnalysis : props.onCancelJob}>Cancel</button></div>
  </section>;
}

function MobilePlayback(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const playing = isPlaying(props.reviewMode);
  return <nav className="mobile-playback" aria-label="Move navigation">
    <SoftButton icon="first" aria-label="First move" onClick={() => props.onNavigate("first")}/>
    <SoftButton icon="previous" aria-label="Previous move" onClick={() => props.onNavigate("previous")}/>
    <SoftButton className="mobile-play-button" icon={playing ? "pause" : "play"} aria-label={playing ? "Pause review" : "Play review"} onClick={props.onTogglePlayback}/>
    <SoftButton icon="next" aria-label="Next move" onClick={() => props.onNavigate("next")}/>
    <SoftButton icon="last" aria-label="Last move" onClick={() => props.onNavigate("last")}/>
  </nav>;
}

function MobileReviewSummary(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const move = props.move;
  const moveLabel = move ? `${move.move_number}${move.side === "white" ? "." : "…"} ${move.played_san || move.san}` : "Current move";
  const showBetter = needsBetterMove(move);
  const label = move?.classification === "Book" ? "Book move" : move?.classification || "Review pending";
  return <section className={`mobile-review-summary class-${classificationClass(move?.classification || "pending")}`} aria-label="Review summary">
    <div className="mobile-review-row"><div><strong>{label}</strong><span>{moveLabel}</span></div><p>{move ? humanMoveExplanation(move) : "Analysis appears here when the review is ready."}</p></div>
    {showBetter && move && <div className="mobile-review-row mobile-better-row"><div><strong>Better</strong><span>{move.best_san || move.best_uci}</span></div><p>{betterMoveExplanation(move)}</p></div>}
    <button className="mobile-more-analysis" onClick={props.onOpenOverview}>More analysis <span aria-hidden="true">↑</span></button>
  </section>;
}

function MobileAnalysisDrawer(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const tabs: Array<{ id: InspectorTab; label: string }> = [{ id: "summary", label: "Review" }, { id: "moves", label: "Moves" }, { id: "line", label: "Line" }, { id: "patterns", label: "Patterns" }];
  const dialogRef = useDialogFocus<HTMLDivElement>(props.onCloseOverview);
  return <div className="mobile-drawer-layer" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) props.onCloseOverview(); }}>
    <aside ref={dialogRef} className="mobile-analysis-drawer" role="dialog" aria-modal="true" aria-label="More analysis">
      <span className="mobile-drawer-handle" aria-hidden="true"/>
      <header><strong>More analysis</strong><button aria-label="Close more analysis" onClick={props.onCloseOverview}><Icon name="close"/></button></header>
      <nav className="mobile-drawer-tabs" aria-label="Analysis details">{tabs.map((tab) => <button key={tab.id} className={props.inspectorTab === tab.id ? "active" : ""} onClick={() => props.onInspectorTab(tab.id)}>{tab.label}</button>)}</nav>
      <div className="mobile-drawer-content">
        {props.inspectorTab === "summary" ? <MobileDrawerReview {...props}/> : null}
        {props.inspectorTab === "moves" ? <MobileDrawerMoves {...props}/> : null}
        {props.inspectorTab === "line" ? <MobileDrawerLine {...props}/> : null}
        {props.inspectorTab === "patterns" && <p className="mobile-drawer-empty">No pattern evidence is recorded for this review.</p>}
      </div>
    </aside>
  </div>;
}

function MobileDrawerReview(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const move = props.move;
  const showBetter = needsBetterMove(move);
  const label = move?.classification === "Book" ? "Book move" : move?.classification || "Review pending";
  return <div className="mobile-drawer-review">
    <div className={`mobile-drawer-verdict class-${classificationClass(move?.classification || "pending")}`}><strong>{label}</strong><span>{move ? `${move.move_number}${move.side === "white" ? "." : "…"} ${move.played_san || move.san}` : "Current move"}</span><p>{move ? humanMoveExplanation(move) : "Analysis appears here when the review is ready."}</p></div>
    {showBetter && move && <div className="mobile-drawer-verdict class-best"><strong>Better</strong><span>{move.best_san || move.best_uci}</span><p>{betterMoveExplanation(move)}</p></div>}
    {move && <div className="mobile-drawer-actions"><button onClick={() => { props.onRetry(); props.onCloseOverview(); }}>Retry</button><button onClick={() => { props.onVariation(); props.onCloseOverview(); }}>Explore variation</button></div>}
  </div>;
}

function MobileDrawerMoves(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const assessments = props.game.analysis?.moves ?? [];
  const selectedMoveRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    selectedMoveRef.current?.scrollIntoView({ block: "nearest" });
  }, [props.selectedPly]);
  return <div className="mobile-full-move-list">{props.game.game.plies.map((ply, index) => { const assessment = assessments[index]; const current = index === props.selectedPly; return <button ref={current ? selectedMoveRef : undefined} key={index} className={current ? "current" : ""} aria-current={current ? "step" : undefined} onClick={() => { props.onSelectPly(index); props.onCloseOverview(); }}><span>{Math.floor(index / 2) + 1}{index % 2 ? "…" : "."}</span><strong>{ply.san}</strong><em>{assessment?.classification || "Pending"}</em><i className={`mini-class class-${classificationClass(assessment?.classification || "pending")}`} aria-hidden="true"/></button>; })}</div>;
}

function MobileDrawerLine(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const move = props.move;
  return <div className="mobile-line-details"><div><span>Evaluation</span><strong>{formatEval(move?.evaluation_after)}</strong></div><div><span>Best line</span><code>{move?.principal_variation.join(" ") || move?.best_san || move?.best_uci || "No line available"}</code></div><div><span>Depth</span><strong>{move?.depth || "—"}</strong></div>{move?.nodes ? <div><span>Nodes</span><strong>{move.nodes.toLocaleString()}</strong></div> : null}</div>;
}

function MobileActionSheet(props: AnalysisProps & { game: StoredGame; move?: MoveAssessment; fen: string; livePly: number; analysisActive: boolean }) {
  const close = props.onCloseMore;
  const dialogRef = useDialogFocus<HTMLElement>(close);
  return <div className="mobile-action-layer" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) close(); }}><aside ref={dialogRef} className="mobile-action-sheet" role="dialog" aria-modal="true" aria-label="Analysis actions"><span className="mobile-drawer-handle" aria-hidden="true"/><header><strong>Analysis actions</strong><button aria-label="Close analysis actions" onClick={close}><Icon name="close"/></button></header><button onClick={() => { props.onOpenOverview(); close(); }}>Overview</button><button onClick={() => { props.onRetry(); close(); }}>Retry move</button><button onClick={() => { props.onVariation(); close(); }}>Explore variation</button><button onClick={() => { props.onShowBestMove(); close(); }}>Show best move</button><button onClick={() => { props.onFlip(); close(); }}>Flip board</button></aside></div>;
}

function useDialogFocus<T extends HTMLElement>(onClose: () => void) {
  const dialogRef = useRef<T>(null);
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    const dialog = dialogRef.current;
    const previous = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    if (!dialog) return;
    const focusable = () => Array.from(dialog.querySelectorAll<HTMLElement>("button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex='-1'])"));
    focusable()[0]?.focus();
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeRef.current();
        return;
      }
      if (event.key !== "Tab") return;
      const controls = focusable();
      if (!controls.length) return;
      const first = controls[0];
      const last = controls[controls.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    dialog.addEventListener("keydown", handleKeyDown);
    return () => {
      dialog.removeEventListener("keydown", handleKeyDown);
      previous?.focus();
    };
  }, []);
  return dialogRef;
}

function mobilePositionLabel(game: StoredGame, progress: BrowserReviewProgress | null, fallbackPly: number) {
  const index = progress?.ply ?? fallbackPly;
  const ply = game.game.plies[index];
  return ply ? `Move ${index + 1} · ${Math.floor(index / 2) + 1}${index % 2 ? "…" : "."}${ply.san}` : `Move ${index + 1}`;
}

function progressPercent(complete: number, total: number) {
  return total > 0 ? Math.max(0, Math.min(100, Math.round((complete / total) * 100))) : 0;
}

function analysisStageLabel(stage: BrowserReviewProgress["stage"] | undefined, status?: Job["status"]) {
  if (stage === "starting") return "Preparing the review";
  if (stage === "analyzing") return "Scanning positions";
  if (stage === "submitting") return "Validating positions";
  if (stage === "finalizing") return "Building the review";
  return status === "queued" ? "Queued for analysis" : "Analyzing your game";
}

function AnalysisErrorNotice({ issue, onRetry, onDismiss }: { issue: AnalysisIssue; onRetry: () => void; onDismiss: () => void }) {
  return <section className="analysis-error-notice" role="alert">
    <div><span>{issue.kind === "analysis" ? "Analysis stopped" : "Variation unavailable"}</span><strong>{issue.kind === "analysis" ? "The review is not ready yet." : "The branch did not open."}</strong><p>{issue.message}</p></div>
    <div className="analysis-error-actions">{issue.kind === "analysis" && <button onClick={onRetry}>Try analysis again</button>}<button onClick={onDismiss}>Dismiss</button></div>
  </section>;
}

function AnalysisActivity({ game, progress, job, onCancel }: { game: StoredGame; progress: BrowserReviewProgress | null; job: Job | null; onCancel: () => void }) {
  const complete = progress?.complete ?? job?.progress.complete ?? 0;
  const total = progress?.total ?? job?.progress.total ?? 0;
  const percent = progressPercent(complete, total);
  const title = analysisStageLabel(progress?.stage, job?.status);
  const position = progress ? `Position ${progress.ply + 1} · ${progress.position}` : `${game.game.plies.length} positions in this game`;
  const detail = progress?.message ?? job?.progress.message ?? "Preparing the analysis service";
  return <section className="analysis-activity" aria-label="Analysis in progress" aria-live="polite">
    <div className="analysis-activity-copy"><span className="analysis-live-mark" aria-hidden="true"/><div><strong>{title}</strong><p>{detail}</p></div></div>
    <div className="analysis-activity-status"><span>{position}</span><span className="analysis-engine">Engine · {progress?.profile ?? "server"}</span><b>{percent}%</b><button onClick={onCancel}>Cancel</button></div>
    <progress className="analysis-activity-track" aria-label="Analysis progress" max={100} value={percent}/>
  </section>;
}

function ReviewInspector(props: AnalysisProps & { analysisActive: boolean }) {
  const move = props.selectedMove;
  const game = props.selectedGame;
  const selectedMoveRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    selectedMoveRef.current?.scrollIntoView({ block: "nearest" });
  }, [props.moveListExpanded, props.selectedPly]);
  if (!game) return null;
  const moves = game.analysis?.moves ?? [];
  const plies = game.game.plies;
  const start = props.moveListExpanded ? 0 : Math.max(0, props.selectedPly - 3);
  const end = props.moveListExpanded ? plies.length : Math.min(plies.length, props.selectedPly + 4);
  return <aside className={`review-rail ${props.analysisActive ? "review-rail-active" : ""}`}>
    <section className="inspector-surface">
      <header className="inspector-header"><div><span>{props.analysisActive ? "Analysis Inspector" : "Game review"}</span><strong>{props.analysisActive ? "Working through the game" : "What changed here"}</strong></div><small>{props.analysisActive ? "Live" : `${plies.length} plies`}</small></header>
      {props.analysisActive ? <div className="inspector-analysis-state"><div className="inspector-state-icon"><Icon name="analysis"/></div><strong>{analysisStageLabel(props.browserProgress?.stage, props.selectedJob?.status)}</strong><p>{props.browserProgress?.message ?? props.selectedJob?.progress.message ?? "The review will appear here when the engine is done."}</p>{props.browserProgress && <dl><div><dt>Position</dt><dd>{props.browserProgress.ply + 1} · {props.browserProgress.position}</dd></div><div><dt>Depth</dt><dd>{props.browserProgress.depth ? `${props.browserProgress.depth}/${props.browserProgress.targetDepth ?? "—"}` : "Starting"}</dd></div><div><dt>Engine</dt><dd>{props.browserProgress.profile}</dd></div></dl>}</div> : props.analysisIssue ? <AnalysisErrorNotice issue={props.analysisIssue} onRetry={props.onRetryAnalysis} onDismiss={props.onDismissAnalysisError}/> : <>
        <section className={`inspector-block inspector-current class-${classificationClass(move?.classification || "pending")}`}>
          <header><span>Current move</span><small>{move ? `${move.move_number}${move.side === "white" ? "." : "…"}` : "Not ready"}</small></header>
          {move ? <div className="inspector-reading"><span className="class-orb"><Icon name={needsAttention(move.classification) ? "warning" : "check"}/></span><div><b>{move.classification}</b><strong>{move.move_number}{move.side === "white" ? "." : "…"} {move.played_san || move.san}</strong><p>{humanMoveExplanation(move)}</p></div></div> : <p className="inspector-empty">Analysis appears here when the review is ready.</p>}
        </section>
        {props.reviewMode === "try_move" && move ? <section className="inspector-block inspector-mode" aria-live="polite"><header><span>Retry move</span><button onClick={props.onReturn}>Return</button></header><div className="mode-content"><h3>Find a stronger move</h3><p>The board is restored before {move.played_san || move.san}. Move on the board or enter UCI.</p>{props.retryFeedback.status !== "correct" && props.retryFeedback.status !== "incorrect" && <RetryForm feedback={props.retryFeedback} onSubmit={props.onRetrySubmit}/>}<RetryFeedbackPanel feedback={props.retryFeedback} onRetryAgain={props.onRetryAgain} onReveal={props.onRevealRetry} onContinue={props.onContinueRetry}/></div></section> : props.reviewMode === "variation" && move ? <section className="inspector-block inspector-mode" aria-live="polite"><header><span>Variation</span><button disabled={props.variationBusy} onClick={props.onReturn}>Return</button></header><div className="mode-content"><h3>Explore this branch</h3><p>{props.variationMessage || "Choose a source and destination on the board."}</p>{props.variationAnalysis && <div className="variation-eval"><strong>{props.variationAnalysis.best_move || "—"}</strong><code>{props.variationAnalysis.lines[0]?.moves.join(" ")}</code></div>}<div className="mode-actions"><button disabled={props.variationBusy} onClick={props.onVariationBack}>Back</button><button disabled={props.variationBusy} onClick={props.onVariationReset}>Reset</button><button disabled={props.variationBusy} onClick={props.onVariationAnalyze}>{props.variationBusy ? "Working…" : "Analyze"}</button><button disabled={props.variationBusy} className="danger-text" onClick={props.onVariationDelete}>Delete</button></div></div></section> : move && needsBetterMove(move) && <section className="inspector-block inspector-best class-best"><header><span>Better move</span><small>{formatEval(move.evaluation_after_best)}</small></header><div className="inspector-reading"><span className="class-orb"><Icon name="star"/></span><div><b>Better</b><strong>{move.move_number}{move.side === "white" ? "." : "…"} {move.best_san || move.best_uci}</strong><p>{betterMoveExplanation(move)}</p><div className="quiet-actions"><button onClick={props.onRetry}><Icon name="retry"/>Retry</button><button onClick={props.onVariation}><Icon name="branch"/>Explore</button></div></div></div></section>}
        <section className="inspector-block inspector-moves"><header><span>Move list</span><small>{plies.length} plies</small></header><div className="move-ledger">{plies.slice(start, end).map((item, offset) => { const index = start + offset; const assessment = moves[index]; const current = index === props.selectedPly; return <button ref={current ? selectedMoveRef : undefined} key={index} className={current ? "current" : ""} aria-current={current ? "step" : undefined} onClick={() => props.onSelectPly(index)}><span>{Math.floor(index / 2) + 1}{index % 2 ? "…" : "."}</span><strong>{item.san}</strong><em>{assessment?.classification || "Pending"}</em>{current && <i className={`mini-class class-${classificationClass(assessment?.classification || "pending")}`}>{needsAttention(assessment?.classification ?? "") ? "!" : ""}</i>}</button>; })}</div>{plies.length > 7 && <button className="ledger-toggle" onClick={props.onToggleMoves}>{props.moveListExpanded ? "Show nearby moves" : "Show all moves"}<span>⌄</span></button>}</section>
      </>}
    </section>
  </aside>;
}

function RetryForm({ feedback, onSubmit }: { feedback: RetryFeedback; onSubmit: (uci: string) => void }) {
  const [value, setValue] = useState("");
  const busy = feedback.status === "checking";
  return <form className="retry-form" onSubmit={(event) => { event.preventDefault(); onSubmit(value); }}><label><span className="sr-only">Move in UCI</span><input value={value} onChange={(event) => setValue(event.target.value)} pattern="[a-h][1-8][a-h][1-8][qrbn]?" placeholder="e2e4" autoComplete="off" disabled={busy} required/></label><button disabled={busy}>{busy ? "Checking…" : "Check"}</button></form>;
}

function RetryFeedbackPanel({ feedback, onRetryAgain, onReveal, onContinue }: { feedback: RetryFeedback; onRetryAgain: () => void; onReveal: () => void; onContinue: () => void }) {
  if (!feedback.message) return null;
  const resolved = feedback.status === "correct" || feedback.status === "incorrect";
  return <div className={`retry-feedback retry-${feedback.status}`} role={feedback.status === "error" || feedback.status === "illegal" ? "alert" : "status"}>
    <div><span>{feedback.status === "correct" ? "Nice find" : feedback.status === "incorrect" ? "Legal move" : feedback.status === "checking" ? "Checking" : "Try again"}</span>{feedback.uci && <code>{feedback.uci}</code>}</div>
    <p>{feedback.message}</p>
    {resolved && <div className="retry-feedback-actions">{feedback.status === "correct" ? <button onClick={onContinue}>Continue review</button> : <button onClick={onRetryAgain}>Try another</button>}<button onClick={onReveal}>Show better move</button></div>}
  </div>;
}

function Playback({ game, selectedPly, playing, onNavigate, onPlay, onFlip }: { game: StoredGame; selectedPly: number; playing: boolean; onNavigate: AnalysisProps["onNavigate"]; onPlay: () => void; onFlip: () => void }) {
  const selected = game.game.plies[selectedPly];
  return <footer className="playback-bar"><div className="playback-buttons"><SoftButton icon="first" aria-label="First move" onClick={() => onNavigate("first")}/><SoftButton icon="previous" aria-label="Previous move" onClick={() => onNavigate("previous")}/><SoftButton className="primary-play" icon={playing ? "pause" : "play"} aria-label={playing ? "Pause review" : "Play review"} onClick={onPlay}/><SoftButton icon="next" aria-label="Next move" onClick={() => onNavigate("next")}/><SoftButton icon="last" aria-label="Last move" onClick={() => onNavigate("last")}/></div><div className="move-selector" role="status"><span>{selected ? `${Math.floor(selectedPly / 2) + 1}${selectedPly % 2 ? "…" : "."} ${selected.san}` : "Starting position"}</span></div><SoftButton className="playback-flip" icon="flip" onClick={onFlip}>Flip</SoftButton></footer>;
}

function OverviewDrawer(props: AnalysisProps) {
  const move = props.selectedMove;
  const analysis = props.selectedGame?.analysis;
  const dialogRef = useDialogFocus<HTMLElement>(props.onCloseOverview);
  return <aside id="analysis-overview" ref={dialogRef} className="overview-drawer" role="dialog" aria-modal="true" aria-labelledby="analysis-overview-title"><header><div><span>Game Overview</span><strong id="analysis-overview-title">Evidence behind this review</strong></div><button aria-label="Close overview" onClick={props.onCloseOverview}><Icon name="close"/></button></header><nav aria-label="Overview sections">{(["summary", "line", "method"] as InspectorTab[]).map((tab) => <button key={tab} className={props.inspectorTab === tab ? "active" : ""} aria-current={props.inspectorTab === tab ? "page" : undefined} onClick={() => props.onInspectorTab(tab)}>{titleCase(tab)}</button>)}</nav><div className="drawer-body">
    {props.inspectorTab === "summary" && <><div className="summary-hero"><strong>{formatAccuracy(analysis?.accuracy, 1)}</strong><span>review accuracy</span></div><dl className="evidence-list"><div><dt>Opening</dt><dd>{[analysis?.eco, analysis?.opening].filter(Boolean).join(" · ") || "Unclassified"}</dd></div><div><dt>Book depth</dt><dd>{analysis?.book_ply ?? 0} plies</dd></div><div><dt>Selected evaluation</dt><dd>{formatEval(move?.evaluation_after)}</dd></div><div><dt>Classification</dt><dd>{move?.classification || "Pending"}</dd></div></dl></>}
    {props.inspectorTab === "line" && <><div className="engine-line"><span>Principal variation</span><strong>{move?.best_san || move?.best_uci || "—"}</strong><code>{move?.principal_variation.join(" ") || "No line available"}</code></div><dl className="evidence-list"><div><dt>Depth</dt><dd>{move?.depth || "—"}</dd></div><div><dt>Nodes</dt><dd>{move?.nodes?.toLocaleString() || "—"}</dd></div><div><dt>Workers</dt><dd>{props.diagnostics?.engine_workers ?? "—"}</dd></div><div><dt>Deep target</dt><dd>{props.runtimeSettings?.deep_depth ?? "—"}</dd></div></dl></>}
    {props.inspectorTab === "method" && <div className="method-copy"><h3>Local, reproducible evidence</h3><p>C++ reconstructs each position, Stockfish evaluates candidates, and the versioned classification model assigns the displayed label. This panel exposes recorded evidence, not hidden reasoning.</p><dl className="evidence-list"><div><dt>Requested profile</dt><dd>{engineProfileLabel(analysis?.requested_engine_profile)}</dd></div><div><dt>Actual profile</dt><dd>{engineProfileLabel(analysis?.actual_engine_profile)}</dd></div><div><dt>Engine</dt><dd>{analysis?.engine_name || move?.engine_version || "Stockfish local"}</dd></div><div><dt>Source</dt><dd>{engineSourceLabel(analysis?.engine_source)}</dd></div><div><dt>Engine build</dt><dd>{move?.engine_version || "Not recorded"}</dd></div><div><dt>Classifier</dt><dd>{move?.classification_model_version || "Tutor model"}</dd></div></dl></div>}
  </div></aside>;
}

function ExploreView({ games, section, selectedId, onSection, onSelect, onOpen }: { games: StoredGame[]; section: ExploreSection; selectedId: string; onSection: (section: ExploreSection) => void; onSelect: (id: string) => void; onOpen: (id: string, ply: number) => void }) {
  const entries = buildExploreEntries(games);
  const visible = entries.filter((entry) => entry.section === section);
  const selected = visible.find((entry) => entry.id === selectedId) ?? visible[0];
  return <section className="soft-surface explore-surface"><aside className="explore-index"><header><span>Personal library</span><h1>Study positions that came from your games.</h1></header><nav>{(["Openings", "Middlegames", "Endgames"] as ExploreSection[]).map((item) => <button key={item} className={section === item ? "active" : ""} onClick={() => onSection(item)}><span>{item}</span><small>{entries.filter((entry) => entry.section === item).length}</small></button>)}</nav><div className="concept-list">{visible.map((entry) => <button key={entry.id} className={selected?.id === entry.id ? "active" : ""} onClick={() => onSelect(entry.id)}><strong>{entry.title}</strong><span>{entry.tags.slice(0, 2).join(" · ")}</span></button>)}</div></aside>{selected ? <article className="concept-detail"><div className="concept-board"><ChessBoard fen={selected.fen} orientation="white" compact/></div><div className="concept-copy"><span>{selected.section.slice(0, -1)} concept</span><h2>{selected.title}</h2><p>{selected.purpose}</p><small>From an analyzed game</small><SoftButton icon="analysis" onClick={() => onOpen(selected.gameId, selected.ply)}>Open in Analysis</SoftButton></div></article> : <div className="empty-state"><Icon name="book"/><h2>No {section.toLowerCase()} yet</h2><p>Analyze more games and this library will assemble itself from real positions.</p></div>}</section>;
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

function SettingsView({ theme, onTheme, engineLinesDefault, onEngineLines, browserProfile, onBrowserProfile, runtime, diagnostics, accountLabel, onSignOut }: { theme: Theme; onTheme: (theme: Theme) => void; engineLinesDefault: boolean; onEngineLines: (value: boolean) => void; browserProfile: BrowserEngineProfile; onBrowserProfile: (profile: BrowserEngineProfile) => void; runtime: RuntimeSettings | null; diagnostics: Diagnostics | null; accountLabel: string; onSignOut: () => void }) {
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
        <div><h2>Browser engine</h2><p>Pick the pass that fits the moment. Quick is a fast read, Balanced goes deeper, and Aggressive spends up to a minute on each position.</p><small className="settings-inline-note">Saved on this device for now. Completed reviews keep the requested and actual profile plus engine source for later reference.</small><div className="settings-notices"><a href="/engine/SOURCE.stockfish.txt" target="_blank" rel="noreferrer">Source record ↗</a><a href="/engine/COPYING.stockfish.txt" target="_blank" rel="noreferrer">GPL-3.0 license ↗</a></div></div>
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
      <section>
        <div><h2>Account</h2><p>Signed in as {accountLabel}.</p></div>
        <div className="settings-account"><span>{accountLabel}</span><button type="button" onClick={onSignOut}>Sign out</button></div>
      </section>
      <section><div><h2>Coaching style</h2><p>A selectable style will appear when C++ exposes a persisted coaching-provider contract.</p></div><span className="unavailable-setting">Unavailable in this build</span></section>
    </div>
  </section>;
}

function ImportModal({ busy, stage, error, onClose, onSubmit }: { busy: boolean; stage: string; error: string; onClose: () => void; onSubmit: (url: string, pgn: string) => void }) {
  const [url, setUrl] = useState("");
  const [pgn, setPgn] = useState("");
  const dialogRef = useRef<HTMLFormElement>(null);

  useEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return;
    const focusable = () => Array.from(dialog.querySelectorAll<HTMLElement>(
      'button:not([disabled]), input:not([disabled]), textarea:not([disabled]), [href], [tabindex]:not([tabindex="-1"])',
    ));
    const first = focusable()[0];
    first?.focus();
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        if (!busy) onClose();
        return;
      }
      if (event.key !== "Tab") return;
      const controls = focusable();
      if (controls.length === 0) return;
      const current = document.activeElement;
      const index = controls.indexOf(current as HTMLElement);
      const next = event.shiftKey
        ? controls[(index <= 0 ? controls.length : index) - 1]
        : controls[(index + 1) % controls.length];
      if (!dialog.contains(current) || index < 0 || next) {
        event.preventDefault();
        (next ?? controls[0]).focus();
      }
    };
    dialog.addEventListener("keydown", onKeyDown);
    return () => dialog.removeEventListener("keydown", onKeyDown);
  }, [busy, onClose]);

  return <div className="modal-backdrop" role="presentation"><form ref={dialogRef} className="import-modal" role="dialog" aria-modal="true" aria-labelledby="import-title" aria-describedby="import-description" onSubmit={(event) => { event.preventDefault(); onSubmit(url, pgn); }}><header><div><span>Add to Recent Games</span><h2 id="import-title">Bring in a game.</h2><p id="import-description">Paste a public Chess.com link or PGN. The imported game stays canonical until you choose Analyze.</p></div><button type="button" aria-label="Close import" onClick={onClose} disabled={busy}><Icon name="close"/></button></header><label htmlFor="chesscom-url"><span>Chess.com game link</span><input id="chesscom-url" name="url" autoComplete="url" type="url" value={url} onChange={(event) => setUrl(event.target.value)} placeholder="https://www.chess.com/game/live/…"/></label><div className="or-rule"><span>or</span></div><label htmlFor="game-pgn"><span>PGN</span><textarea id="game-pgn" name="pgn" value={pgn} onChange={(event) => setPgn(event.target.value)} placeholder={'[Event "…"]\n\n1. e4 e5 …'} aria-describedby={error ? "import-error" : undefined}/></label>{error && <p id="import-error" className="form-error" role="alert">{error}</p>}<footer><small>Public game data only · no Chess.com password</small><button type="submit" disabled={busy}>{busy ? stage || "Importing…" : "Import game"}</button></footer></form></div>;
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

function ServiceUnavailableView({ message, onRetry, onSignOut }: {
  message: string;
  onRetry: () => void;
  onSignOut: () => void;
}) {
  return <main className="service-unavailable" aria-labelledby="service-unavailable-title">
    <section className="service-unavailable-panel">
      <span className="service-unavailable-kicker">Plywise service</span>
      <h1 id="service-unavailable-title">Your account is signed in, but the chess service is not reachable.</h1>
      <p>{message}</p>
      <div className="service-unavailable-actions">
        <button className="landing-primary" type="button" onClick={onRetry}>Try again</button>
        <button className="landing-secondary" type="button" onClick={onSignOut}>Sign out</button>
      </div>
      <small>For local development, start the C++ service on port 8787. For a hosted build, check the public API origin.</small>
    </section>
  </main>;
}

function accountDisplayName(auth: AuthSnapshot): string {
  const user = auth.session?.user;
  if (!user) return "Plywise account";
  for (const key of ["full_name", "name", "user_name"]) {
    const value = user.user_metadata[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return user.email || "Plywise account";
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
function formatAccuracy(value: number | undefined, digits: number, suffix = "") {
  return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(digits)}${suffix}` : "—";
}
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

function dayGreeting(date: Date) {
  const hour = date.getHours();
  if (hour < 12) return "Good morning";
  if (hour < 18) return "Good afternoon";
  return "Good evening";
}
