function originFromEnv(env, name, protocols, { productionTls = false } = {}) {
  const value = String(env[name] ?? "").trim();
  if (!value) return null;

  let parsed;
  try {
    parsed = new URL(value);
  } catch {
    throw new Error(`${name} must be an absolute URL origin.`);
  }

  if (
    !protocols.includes(parsed.protocol) ||
    parsed.username ||
    parsed.password ||
    parsed.pathname !== "/" ||
    parsed.search ||
    parsed.hash
  ) {
    throw new Error(`${name} must be a credential-free ${protocols.join("/")} origin.`);
  }
  if (productionTls && !["https:", "wss:"].includes(parsed.protocol)) {
    throw new Error(`${name} must use TLS for a production deployment.`);
  }
  return parsed.origin;
}

function websocketOrigin(httpOrigin) {
  const parsed = new URL(httpOrigin);
  parsed.protocol = parsed.protocol === "https:" ? "wss:" : "ws:";
  return parsed.origin;
}

function securityHeaders(connectSources) {
  const contentSecurityPolicy = [
    "default-src 'self'",
    "base-uri 'self'",
    "object-src 'none'",
    "form-action 'self'",
    "frame-ancestors 'none'",
    "script-src 'self'",
    "worker-src 'self'",
    "style-src 'self'",
    "img-src 'self' data:",
    "font-src 'self'",
    `connect-src ${[...connectSources].join(" ")}`,
  ].join("; ");

  return [
    {
      source: "/(.*)",
      headers: [
        { key: "Content-Security-Policy", value: contentSecurityPolicy },
        { key: "Permissions-Policy", value: "camera=(), microphone=(), geolocation=()" },
        { key: "Referrer-Policy", value: "no-referrer" },
        { key: "X-Content-Type-Options", value: "nosniff" },
        { key: "X-Frame-Options", value: "DENY" },
      ],
    },
    {
      source: "/api/:path*",
      headers: [{ key: "Cache-Control", value: "no-store" }],
    },
    {
      source: "/assets/(.*)",
      headers: [{ key: "Cache-Control", value: "public, max-age=31536000, immutable" }],
    },
  ];
}

export function createConfig(env = process.env) {
  const production = env.VERCEL_ENV === "production";
  const proxyApiOrigin = originFromEnv(env, "PCT_PLYWISE_API_ORIGIN", ["https:"], { productionTls: true });
  const publicApiOrigin = originFromEnv(env, "VITE_PLYWISE_API_ORIGIN", ["http:", "https:"], {
    productionTls: production,
  });
  const publicEventOrigin = originFromEnv(env, "VITE_PLYWISE_EVENT_ORIGIN", ["ws:", "wss:"], {
    productionTls: production,
  });
  const supabaseOrigin = originFromEnv(
    env,
    "VITE_SUPABASE_URL",
    ["http:", "https:"],
    { productionTls: production },
  );
  const supabasePublishableKey = String(env.VITE_SUPABASE_PUBLISHABLE_KEY ?? "").trim();

  // A production static deployment without either a direct API origin or a
  // same-origin proxy is a healthy-looking landing page with a dead console.
  // Fail the deployment rather than shipping that broken state.
  if (production && !proxyApiOrigin && !publicApiOrigin) {
    throw new Error(
      "Production Vercel deployments require PCT_PLYWISE_API_ORIGIN (proxy) " +
        "or VITE_PLYWISE_API_ORIGIN (direct API).",
    );
  }
  if (production && (!supabaseOrigin || !supabasePublishableKey)) {
    throw new Error(
      "Production Vercel deployments require VITE_SUPABASE_URL and VITE_SUPABASE_PUBLISHABLE_KEY " +
        "so account sign-in cannot silently fall back.",
    );
  }
  if (production && proxyApiOrigin && !publicEventOrigin) {
    throw new Error(
      "Proxy deployments require VITE_PLYWISE_EVENT_ORIGIN so live C++ job updates use the API TLS origin.",
    );
  }

  const connectSources = new Set(["'self'"]);
  if (publicApiOrigin) connectSources.add(publicApiOrigin);
  if (publicEventOrigin) connectSources.add(publicEventOrigin);
  else if (publicApiOrigin) connectSources.add(websocketOrigin(publicApiOrigin));
  if (supabaseOrigin) connectSources.add(supabaseOrigin);

  const rewrites = [];
  if (proxyApiOrigin) {
    rewrites.push({
      source: "/api/:path*",
      destination: `${proxyApiOrigin}/api/:path*`,
    });
  }
  rewrites.push(
    { source: "/auth/callback", destination: "/index.html" },
    { source: "/auth/reset", destination: "/index.html" },
    { source: "/(.*)", destination: "/index.html" },
  );

  return {
    rewrites,
    headers: securityHeaders(connectSources),
  };
}

export const config = createConfig();
