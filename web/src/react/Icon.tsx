import type { ReactNode, SVGProps } from "react";

export type IconName =
  | "home"
  | "analysis"
  | "recent"
  | "explore"
  | "progress"
  | "settings"
  | "overview"
  | "import"
  | "more"
  | "first"
  | "previous"
  | "play"
  | "pause"
  | "next"
  | "last"
  | "flip"
  | "close"
  | "search"
  | "check"
  | "warning"
  | "star"
  | "branch"
  | "retry"
  | "book"
  | "chart"
  | "eye"
  | "eye-off";

const paths: Record<IconName, ReactNode> = {
  home: <><path d="m4 10 8-7 8 7v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-9Z"/><path d="M9 21v-7h6v7"/></>,
  analysis: <><circle cx="12" cy="12" r="7"/><path d="m17 17 4 4M9 12h6M12 9v6"/></>,
  recent: <><rect x="4" y="5" width="16" height="15" rx="3"/><path d="M8 3v4M16 3v4M4 10h16M8 14h3M13 14h3"/></>,
  explore: <><circle cx="12" cy="12" r="8"/><path d="m15.5 8.5-2 5-5 2 2-5 5-2Z"/></>,
  progress: <path d="M5 19V9M10 19V5M15 19v-7M20 19V3"/>,
  settings: <><circle cx="12" cy="12" r="3"/><path d="M19 12a7 7 0 0 0-.1-1.2l2-1.5-2-3.4-2.4 1a7 7 0 0 0-2-1.2L14.2 3h-4.4l-.3 2.7a7 7 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.5A7 7 0 0 0 5 12c0 .4 0 .8.1 1.2l-2 1.5 2 3.4 2.4-1a7 7 0 0 0 2 1.2l.3 2.7h4.4l.3-2.7a7 7 0 0 0 2-1.2l2.4 1 2-3.4-2-1.5c.1-.4.1-.8.1-1.2Z"/></>,
  overview: <path d="M5 20V10M10 20V5M15 20v-8M20 20V3"/>,
  import: <><path d="M12 16V3m0 0L7 8m5-5 5 5"/><path d="M4 14v5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-5"/></>,
  more: <><circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/></>,
  first: <><path d="M6 5v14M18 6l-7 6 7 6Z"/></>,
  previous: <path d="m16 6-7 6 7 6Z"/>,
  play: <path d="m9 5 10 7-10 7Z"/>,
  pause: <path d="M9 5v14M15 5v14"/>,
  next: <path d="m8 6 7 6-7 6Z"/>,
  last: <><path d="M18 5v14M6 6l7 6-7 6Z"/></>,
  flip: <><path d="M4 8a8 8 0 0 1 13-3l2 2"/><path d="M19 3v4h-4M20 16a8 8 0 0 1-13 3l-2-2"/><path d="M5 21v-4h4"/></>,
  close: <path d="m6 6 12 12M18 6 6 18"/>,
  search: <><circle cx="11" cy="11" r="7"/><path d="m16 16 5 5"/></>,
  check: <path d="m5 12 4 4L19 6"/>,
  warning: <><path d="M12 3 2.8 20h18.4L12 3Z"/><path d="M12 9v5M12 17h.01"/></>,
  star: <path d="m12 3 2.7 5.5 6.1.9-4.4 4.3 1 6.1-5.4-2.9-5.4 2.9 1-6.1-4.4-4.3 6.1-.9L12 3Z"/>,
  branch: <><circle cx="6" cy="5" r="2"/><circle cx="18" cy="7" r="2"/><circle cx="18" cy="18" r="2"/><path d="M8 5h3a5 5 0 0 1 5 5v6M8 5v13h8"/></>,
  retry: <><path d="M4 9a8 8 0 1 1 1 8"/><path d="M4 4v5h5"/></>,
  book: <><path d="M4 5.5A3.5 3.5 0 0 1 7.5 2H12v18H7.5A3.5 3.5 0 0 0 4 23V5.5Z"/><path d="M20 5.5A3.5 3.5 0 0 0 16.5 2H12v18h4.5A3.5 3.5 0 0 1 20 23V5.5Z"/></>,
  chart: <><path d="M4 19V5M4 19h16"/><path d="m7 15 4-5 3 2 5-7"/></>,
  eye: <><path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z"/><circle cx="12" cy="12" r="2.5"/></>,
  "eye-off": <><path d="m3 3 18 18"/><path d="M10.6 10.6a2 2 0 0 0 2.8 2.8"/><path d="M9.9 5.2A10.8 10.8 0 0 1 12 5c6 0 9.5 7 9.5 7a18 18 0 0 1-3.1 3.9"/><path d="M6.2 6.2C3.8 7.8 2.5 12 2.5 12s3.5 6 9.5 6c1 0 1.9-.2 2.8-.5"/></>,
};

export function Icon({ name, ...props }: { name: IconName } & SVGProps<SVGSVGElement>) {
  return <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false" {...props}>{paths[name]}</svg>;
}
