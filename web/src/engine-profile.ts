export type BrowserEngineProfile = "quick" | "balanced";

export const BROWSER_ENGINE_VERSION = "stockfish-18.0.8-lite-single";

export interface BrowserEngineProfileConfig {
  readonly id: BrowserEngineProfile;
  readonly label: string;
  readonly description: string;
  readonly depth: number;
  readonly maxAnalysisMs: number;
  readonly hashMb: number;
  readonly threads: 1;
}

const profiles: Record<BrowserEngineProfile, BrowserEngineProfileConfig> = {
  quick: {
    id: "quick",
    label: "Quick",
    description: "A short pass for a fast first read.",
    depth: 10,
    maxAnalysisMs: 15_000,
    hashMb: 16,
    threads: 1,
  },
  balanced: {
    id: "balanced",
    label: "Balanced",
    description: "A deeper pass that takes a little longer.",
    depth: 14,
    maxAnalysisMs: 30_000,
    hashMb: 16,
    threads: 1,
  },
};

export function browserEngineProfile(profile: BrowserEngineProfile): BrowserEngineProfileConfig {
  return profiles[profile];
}

export function normalizeBrowserEngineProfile(value: unknown): BrowserEngineProfile {
  return value === "balanced" ? "balanced" : "quick";
}

export function browserEngineProfiles(): readonly BrowserEngineProfileConfig[] {
  return [profiles.quick, profiles.balanced];
}
