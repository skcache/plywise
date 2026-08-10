import { loadEnv } from "vite";

const allowedPublicVariables = new Set([
  "VITE_PLYWISE_API_ORIGIN",
  "VITE_PLYWISE_EVENT_ORIGIN",
  "VITE_SUPABASE_URL",
  "VITE_SUPABASE_PUBLISHABLE_KEY",
  // Vercel injects this public JSON for its optional observability integrations.
  "VITE_VERCEL_OBSERVABILITY_CLIENT_CONFIG",
]);

const modeFlag = process.argv.indexOf("--mode");
const mode = modeFlag >= 0 ? process.argv[modeFlag + 1] : "production";
if (!mode) {
  console.error("The public environment check requires a mode after --mode.");
  process.exit(1);
}

const shellPublicVariables = Object.fromEntries(
  Object.entries(process.env).filter(([name]) => name.startsWith("VITE_")),
);
export const publicEnvironment = {
  ...loadEnv(mode, process.cwd(), "VITE_"),
  ...shellPublicVariables,
};
const publicVariables = Object.keys(publicEnvironment);
const unexpected = publicVariables.filter((name) => !allowedPublicVariables.has(name));

if (unexpected.length > 0) {
  console.error(
    `Unexpected public build variables: ${unexpected.sort().join(", ")}. ` +
      "Every VITE_ value is shipped to the browser.",
  );
  process.exit(1);
}

function validateOrigin(name, protocols) {
  const value = publicEnvironment[name]?.trim();
  if (!value) return null;

  let parsed;
  try {
    parsed = new URL(value);
  } catch {
    console.error(`${name} must be an absolute URL.`);
    process.exit(1);
  }

  if (
    !protocols.includes(parsed.protocol) ||
    parsed.username ||
    parsed.password ||
    parsed.pathname !== "/" ||
    parsed.search ||
    parsed.hash
  ) {
    console.error(`${name} must be a credential-free ${protocols.join("/")} origin.`);
    process.exit(1);
  }
  if (
    mode === "production" &&
    ((name === "VITE_PLYWISE_API_ORIGIN" && parsed.protocol !== "https:") ||
      (name === "VITE_PLYWISE_EVENT_ORIGIN" && parsed.protocol !== "wss:") ||
      (name === "VITE_SUPABASE_URL" && parsed.protocol !== "https:"))
  ) {
    console.error(`${name} must use TLS in a configured production build.`);
    process.exit(1);
  }
  return parsed.origin;
}

export const validatedPublicOrigins = {
  api: validateOrigin("VITE_PLYWISE_API_ORIGIN", ["http:", "https:"]),
  events: validateOrigin("VITE_PLYWISE_EVENT_ORIGIN", ["ws:", "wss:"]),
  supabase: validateOrigin("VITE_SUPABASE_URL", ["https:", "http:"]),
};

const supabaseUrl = publicEnvironment.VITE_SUPABASE_URL?.trim();
const supabaseKey = publicEnvironment.VITE_SUPABASE_PUBLISHABLE_KEY?.trim();
if (Boolean(supabaseUrl) !== Boolean(supabaseKey)) {
  console.error("VITE_SUPABASE_URL and VITE_SUPABASE_PUBLISHABLE_KEY must be configured together.");
  process.exit(1);
}
if (mode === "production" && (!supabaseUrl || !supabaseKey)) {
  console.error(
    "Production builds require VITE_SUPABASE_URL and VITE_SUPABASE_PUBLISHABLE_KEY for account sign-in.",
  );
  process.exit(1);
}
if (supabaseKey && /service[_-]?role|secret/i.test(supabaseKey)) {
  console.error("VITE_SUPABASE_PUBLISHABLE_KEY must not contain a service-role or secret key.");
  process.exit(1);
}
