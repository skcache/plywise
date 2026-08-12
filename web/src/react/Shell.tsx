import type { ReactNode } from "react";
import { Icon, type IconName } from "./Icon";
import type { Route } from "../routes";

export type { Route } from "../routes";

const navigation: Array<{ route: Route; label: string; icon: IconName }> = [
  { route: "home", label: "Home", icon: "home" },
  { route: "recent", label: "Recent", icon: "recent" },
  { route: "analysis", label: "Analysis", icon: "analysis" },
  { route: "explore", label: "Explore", icon: "explore" },
  { route: "progress", label: "Progress", icon: "progress" },
];

export function AppShell({ route, onRoute, header, children }: {
  route: Route;
  onRoute: (route: Route) => void;
  header: ReactNode;
  children: ReactNode;
}) {
  return <div className="app-frame">
    <aside className="side-rail" aria-label="Primary navigation">
      <button className="brand-orb" aria-label="Plywise home" onClick={() => onRoute("home")}>
        <img src="/pieces/lasker/white_knight.svg" alt="" width={34} height={34} />
      </button>
      <nav>
        {navigation.map((item) => <button
          key={item.route}
          className={route === item.route ? "active" : ""}
          aria-current={route === item.route ? "page" : undefined}
          onClick={() => onRoute(item.route)}
        ><Icon name={item.icon}/><span>{item.label}</span></button>)}
      </nav>
      <button className={`rail-settings ${route === "settings" ? "active" : ""}`} onClick={() => onRoute("settings")} aria-current={route === "settings" ? "page" : undefined}>
        <Icon name="settings"/><span>Settings</span>
      </button>
    </aside>
    <section className={`app-stage app-stage-${route}`}>
      <div className={`desktop-route-header route-header-${route}`}>{header}</div>
      <main className={`route-canvas route-${route}`}>{children}</main>
      <nav className="mobile-bottom-nav" aria-label="Primary navigation">
        {navigation.map((item) => <button key={item.route} className={route === item.route ? "active" : ""} aria-current={route === item.route ? "page" : undefined} onClick={() => onRoute(item.route)}><Icon name={item.icon}/><span>{item.label}</span></button>)}
      </nav>
    </section>
  </div>;
}

export function TopBar({ title, detail, meta, actions, icon = "recent" }: {
  title: string;
  detail?: string;
  meta?: string;
  actions?: ReactNode;
  icon?: IconName;
}) {
  return <header className="top-bar">
    <div className="top-identity">
      <Icon name={icon}/>
      <strong>{title}</strong>
      {detail && <><i>•</i><span>{detail}</span></>}
      {meta && <small>{meta}</small>}
    </div>
    <div className="top-actions">{actions}</div>
  </header>;
}

export function SoftButton({ icon, children, className = "", ...props }: {
  icon?: IconName;
  children?: ReactNode;
  className?: string;
} & React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return <button className={`soft-button ${className}`} {...props}>{icon && <Icon name={icon}/>} {children && <span>{children}</span>}</button>;
}
