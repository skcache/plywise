export type BrowserEngineProfile = "quick" | "balanced" | "aggressive";

export const BROWSER_OBSERVATION_CONTRACT_VERSION = "browser-observation-v1";
export const BROWSER_ENGINE_NAME = "Stockfish";
export const BROWSER_ENGINE_VERSION = "stockfish-18.0.8-lite-single";
export const BROWSER_ENGINE_SOURCE = "browser";
export const BROWSER_ENGINE_ASSET_HASH =
  "js:5243fd9b276cab7dfe3ad1d43ab9ead73568fac76468c614242977a210c4a391;" +
  "wasm:a8fbc05ec6920b56d7485826dcb02c5ffd2826bcbf751cf973046f237a9096f1";

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
  aggressive: {
    id: "aggressive",
    label: "Aggressive",
    description: "The longest browser pass. More detail, more time, and more battery.",
    depth: 18,
    maxAnalysisMs: 60_000,
    hashMb: 16,
    threads: 1,
  },
};

export function browserEngineProfile(profile: BrowserEngineProfile): BrowserEngineProfileConfig {
  return profiles[profile];
}

export function normalizeBrowserEngineProfile(value: unknown): BrowserEngineProfile {
  return value === "balanced" || value === "aggressive" ? value : "quick";
}

export function browserEngineProfiles(): readonly BrowserEngineProfileConfig[] {
  return [profiles.quick, profiles.balanced, profiles.aggressive];
}
