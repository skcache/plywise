import { useState, type KeyboardEvent, type PointerEvent } from "react";
import { ChessBoard } from "./Board";
import { Icon } from "./Icon";

const reviewFen = "r1b1r1k1/ppq2ppp/3p1n2/3Pp3/2P5/5NP1/PPQ2PBP/R3R1K1 w - - 0 18";
const demoFen = "2r2rk1/pp1nbppp/3p1n2/q1pP4/2P1P3/2N1BN2/PP1N1PPP/1R1QR1K1 w - - 1 15";

const previewClassifications = [
  ["Brilliant", "is-brilliant", 0, 0],
  ["Textbook", "is-book", 8, 9],
  ["Best", "is-best", 14, 9],
  ["Excellent", "is-excellent", 11, 15],
  ["Good", "is-good", 3, 6],
  ["Inaccuracy", "is-inaccuracy", 1, 0],
  ["Mistake", "is-mistake", 1, 1],
  ["Blunder", "is-blunder", 0, 0],
] as const;

type PreviewTilt = "center" | "north" | "south" | "east" | "west" | "north-east" | "north-west" | "south-east" | "south-west";

function previewTilt(event: PointerEvent<HTMLElement>): PreviewTilt {
  const bounds = event.currentTarget.getBoundingClientRect();
  const horizontal = (event.clientX - bounds.left) / bounds.width;
  const vertical = (event.clientY - bounds.top) / bounds.height;
  const x = horizontal < .35 ? "west" : horizontal > .65 ? "east" : "";
  const y = vertical < .35 ? "north" : vertical > .65 ? "south" : "";
  return (y && x ? `${y}-${x}` : y || x || "center") as PreviewTilt;
}

export function LandingView({ onStart, onSignIn }: {
  onStart: () => void;
  onSignIn: () => void;
}) {
  const [tilt, setTilt] = useState<PreviewTilt>("center");
  const scrollTo = (id: string) => document.getElementById(id)?.scrollIntoView({ behavior: "smooth", block: "start" });
  const openPreview = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onStart();
  };

  return <main className="landing-page">
    <div className="landing-shell">
      <header className="landing-header">
        <button className="landing-brand" onClick={() => window.scrollTo({ top: 0, behavior: "smooth" })}>
          <span className="landing-brand-mark"><img src="/pieces/lasker/white_knight.svg" alt="" width={24} height={24} /></span>
          <span>Plywise</span>
        </button>
        <nav className="landing-nav" aria-label="Landing page">
          <button onClick={() => scrollTo("landing-flow")}>How it works</button>
        </nav>
        <button className="landing-signin" onClick={onSignIn}>Sign in</button>
      </header>

      <section className="landing-hero" aria-labelledby="landing-title">
        <div className="landing-copy">
          <h1 id="landing-title">Free chess analysis.</h1>
          <p className="landing-lede">Bring a public game link or PGN, then review the game.</p>
          <div className="landing-actions">
            <button className="landing-primary" onClick={onStart}>Sign up to analyze <Icon name="analysis" /></button>
            <button className="landing-secondary" onClick={onSignIn}>Already have an account? Sign in</button>
          </div>
        </div>

        <section
          className={`landing-preview preview-tilt-${tilt}`}
          aria-label="Game analysis preview"
          onPointerMove={(event) => setTilt(previewTilt(event))}
          onPointerLeave={() => setTilt("center")}
        >
          <span className="landing-preview-header">Game analysis</span>
          <div className="landing-preview-body">
            <div
              className="landing-preview-board"
              role="button"
              tabIndex={0}
              aria-label="Open a free analysis from the preview"
              onClick={onStart}
              onKeyDown={openPreview}
            >
              <div aria-hidden="true">
                <ChessBoard fen={demoFen} orientation="white" compact={false} showCoordinates={false} activeUci="f3e5" showArrow />
              </div>
            </div>
            <aside className="landing-preview-inspector" aria-label="Move quality preview">
              <div className="landing-demo-classifications" aria-label="Illustrative move classifications">
                <div className="landing-demo-classification-head"><span>Move markers</span><span>White</span><span>Black</span></div>
                {previewClassifications.map(([label, marker, white, black]) => <div className="landing-demo-classification-row" key={label}>
                  <span><i className={marker} aria-hidden="true" />{label}</span><b>{white}</b><b>{black}</b>
                </div>)}
              </div>
            </aside>
          </div>
        </section>
      </section>

      <section className="landing-flow" id="landing-flow" aria-labelledby="landing-flow-title">
        <div className="landing-section-heading">
          <p className="landing-eyebrow">How it works</p>
          <h2 id="landing-flow-title">From game link to useful review.</h2>
        </div>
        <div className="landing-visual-flow">
          <article className="landing-flow-stage">
            <div className="landing-stage-heading"><strong>Bring a game</strong></div>
            <div className="landing-stage-input" role="img" aria-label="Paste a public game link or PGN">
              <span>chess.com/game/...</span><b>Analyze</b>
            </div>
            <p>Paste a public link or PGN.</p>
          </article>
          <article className="landing-flow-stage">
            <div className="landing-stage-heading"><strong>Review</strong></div>
            <div className="landing-stage-loading" role="img" aria-label="Reviewing the game">
              <span className="landing-thinking-line" aria-hidden="true">
                <span className="landing-thinking-text" data-text="Reviewing the game...">Reviewing the game...</span>
              </span>
            </div>
            <p>Let the review run when you are ready.</p>
          </article>
          <article className="landing-flow-stage landing-flow-stage-board">
            <div className="landing-stage-heading"><strong>Follow the review</strong></div>
            <div className="landing-stage-board" role="img" aria-label="Chess analysis board with a suggested move">
              <ChessBoard fen={reviewFen} orientation="white" compact activeUci="c2f5" showArrow />
              <span>Analysis ready</span>
            </div>
            <p>See the position and the move worth another look.</p>
          </article>
        </div>
      </section>
    </div>
  </main>;
}
