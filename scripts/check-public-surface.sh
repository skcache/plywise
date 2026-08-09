#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

unexpected_markdown=$(git ls-files -- '*.md' | awk '
  $0 == "README.md" ||
  $0 == "web/public/pieces/lasker/ATTRIBUTION.md" ||
  $0 == "web/public/pieces/lasker/LICENSE.md" { next }
  { print }
')

if [ -n "$unexpected_markdown" ]; then
  printf '%s\n' "Unexpected Markdown files are tracked in the public repository:" >&2
  printf '%s\n' "$unexpected_markdown" >&2
  exit 1
fi

private_paths=$(git ls-files | awk '
  $0 ~ /^\.agents\// ||
  $0 == "AGENTS.md" ||
  $0 ~ /^ground-truth\// ||
  $0 ~ /^docs\// ||
  $0 ~ /^archive\// ||
  $0 ~ /^design\// ||
  $0 ~ /^release\// ||
  $0 ~ /^chess-analysis-frontend-north-star\// ||
  $0 == "RESTRUCTURE_PLAN.md" ||
  $0 == "restructure-plan.html" ||
  $0 == "security_best_practices_report.md" ||
  $0 == "plywise-threat-model.md" ||
  $0 == "engineering-notebook.html" ||
  $0 == "web/README.md" { print }
')

if [ -n "$private_paths" ]; then
  printf '%s\n' "Private implementation paths are tracked in the public repository:" >&2
  printf '%s\n' "$private_paths" >&2
  exit 1
fi

printf '%s\n' "Public repository surface check passed."
