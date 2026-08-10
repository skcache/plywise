# Plywise

Plywise is a free, open-source chess analysis tool for completed games.

Paste a Chess.com game link or PGN, run Stockfish, and review what actually happened. Single-game
analysis will stay free. The bigger goal is to build a personal chess intelligence system that
remembers your games, finds repeated weaknesses, and gives you useful positions to practice.

## What it does

- Imports completed Chess.com games and pasted PGNs.
- Reconstructs and validates games with a C++ chess core.
- Runs Stockfish analysis with real progress and cancellation.
- Explains mistakes, openings, alternatives, and important positions.
- Supports retrying moves and exploring legal variations.
- Builds progress and weakness evidence from analyzed games.

## What's done

The local C++ analysis system and React interface are working. Import, analysis, review, variations,
practice data, progress, persistence, tests, and the new Home screen are all in place.

We are now moving Plywise from a Mac-first local app to a web-first hybrid product.

## Run it locally

Run `npm ci --prefix web`, then `scripts/browser-first-smoke.sh` to build the C++ API and start the
React dev server against it on loopback. No hosted service or billing account is needed.

## Run the API yourself

Vercel only serves the React site. The C++ API is a long-running container, so run `docker compose
up --build -d` on an always-on Linux host and put a TLS reverse proxy in front of port `8787`.
Tagged releases also publish `ghcr.io/skcache/plywise-api`; set `PCT_API_IMAGE` to that image if
you want to pull the release instead of building it locally:

```sh
PCT_API_IMAGE=ghcr.io/skcache/plywise-api:latest docker compose pull
PCT_API_IMAGE=ghcr.io/skcache/plywise-api:latest docker compose up -d
```
For a remote authenticated launch, set `PCT_POSTGRES_URL`, `PCT_SUPABASE_URL`,
`PCT_TRUSTED_HOSTS`, and `PCT_ALLOWED_ORIGINS`, then choose one API connection mode in Vercel:

- Direct: set `VITE_PLYWISE_API_ORIGIN` to the HTTPS API origin and
  `VITE_PLYWISE_EVENT_ORIGIN` to its `wss://` origin.
- Same-origin proxy: set the non-public `PCT_PLYWISE_API_ORIGIN` to the HTTPS API origin. Vercel
  will proxy `/api/*` through the site, and set `VITE_PLYWISE_EVENT_ORIGIN` to the API's `wss://`
  origin so live server-job updates do not try to use Vercel's static host.

The checked-in `vercel.mjs` rejects a production deployment without one of those API modes,
keeps API responses out of caches, and builds the CSP from the configured public origins. The
service itself refuses to start when the hosted C++ configuration is incomplete.

## What's left

- Make the current experience work safely on the web.
- Keep free single-game analysis running on the user's device where possible.
- Add optional accounts, saved history, and cross-device sync.
- Build the personal intelligence and practice layer.
- Explore a completed-game browser extension after the Chess.com integration is approved.
- Harden everything for a public open-source release.

Plywise is independent and is not affiliated with or sponsored by Chess.com.
