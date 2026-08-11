import type { ReactNode } from "react";
import { moveOverlayGeometry, squaresFromFen, uciSquares, type BoardOrientation } from "../chess";

const blackPieces = new Set(["♟", "♜", "♞", "♝", "♛", "♚"]);
const pieceKinds: Record<string, string> = {
  "♔": "king", "♕": "queen", "♖": "rook", "♗": "bishop", "♘": "knight", "♙": "pawn",
  "♚": "king", "♛": "queen", "♜": "rook", "♝": "bishop", "♞": "knight", "♟": "pawn",
};

export function ChessBoard({ fen, orientation, activeUci = "", sourceSquare = "", interactive = false, showArrow = false, compact = false, showCoordinates = true, onSquare }: {
  fen: string;
  orientation: BoardOrientation;
  activeUci?: string;
  sourceSquare?: string;
  interactive?: boolean;
  showArrow?: boolean;
  compact?: boolean;
  showCoordinates?: boolean;
  onSquare?: (square: string) => void;
}) {
  const highlighted = uciSquares(activeUci);
  const squares = squaresFromFen(fen, orientation);
  const geometry = showArrow && activeUci ? moveOverlayGeometry(activeUci, orientation) : null;
  return <div className={`chess-board ${compact ? "compact-board" : ""}`} role="grid" aria-label={compact ? "Position preview" : "Chess position"}>
    {squares.map((square, index) => {
      const light = (Math.floor(index / 8) + index) % 2 === 0;
      const from = highlighted?.[0] === square.name;
      const to = highlighted?.[1] === square.name;
      const selected = sourceSquare === square.name;
      const side = blackPieces.has(square.piece) ? "black" : "white";
      const kind = pieceKinds[square.piece];
      const squareContent = <>
        {!compact && showCoordinates && index % 8 === 0 && <span className="rank-label">{square.rank}</span>}
        {!compact && showCoordinates && index >= 56 && <span className="file-label">{square.file}</span>}
        {kind && (
          <span className={`board-piece ${side}-piece`} aria-hidden="true"><LegacyPiece kind={kind}/></span>
        )}
      </>;
      const className = `board-square ${light ? "light" : "dark"} ${from ? "from" : ""} ${to ? "to" : ""} ${selected ? "source" : ""}`;
      const label = interactive ? `Choose ${square.name}` : `${kind ? `${kind} on` : "empty"} ${square.name}`;
      return interactive ? <button
        type="button"
        key={`${square.name}-${index}`}
        className={className}
        aria-label={label}
        onClick={() => onSquare?.(square.name)}
      >{squareContent}</button> : <div
        key={`${square.name}-${index}`}
        className={className}
        role="gridcell"
        aria-label={label}
      >{squareContent}</div>;
    })}
    {geometry && <svg className="best-arrow" viewBox="0 0 100 100" aria-hidden="true"><defs><marker id="react-arrowhead" markerWidth="4" markerHeight="4" viewBox="0 0 4 4" refX="3.5" refY="2" orient="auto"><path d="M0 0 4 2 0 4Z"/></marker></defs><line x1={geometry.source.x} y1={geometry.source.y} x2={geometry.destination.x} y2={geometry.destination.y} markerEnd="url(#react-arrowhead)"/></svg>}
  </div>;
}

function LegacyPiece({ kind }: { kind: string }) {
  const common = { viewBox: "0 0 64 64", "aria-hidden": true, focusable: false } as const;
  const base = <path className="piece-base" d="M18 53h28M22 47h20M25 41h14"/>;
  const shapes: Record<string, ReactNode> = {
    pawn: <><circle cx="32" cy="19" r="8"/><path d="M24 42c1.4-10.2 4.1-15 8-15s6.6 4.8 8 15Z"/>{base}</>,
    rook: <><path d="M20 14h7v6h10v-6h7v14H20Z"/><path d="M23 28h18l-3 14H26Z"/>{base}</>,
    knight: <><path d="M21 43c3.2-14.6 12.2-25.2 24-29 .7 7.5-1.1 13.5-5.3 18l4.3 10H30l-5 5Z"/><path d="M34 19l-3 4 5 1"/><circle cx="39" cy="22" r="1.6"/>{base}</>,
    bishop: <><circle cx="32" cy="13" r="5"/><path d="M24 42c1.2-12.8 3.9-22 8-22s6.8 9.2 8 22Z"/><path d="M35 21 28 34"/>{base}</>,
    queen: <><path d="M17 24 23 13l6 12 3-14 3 14 6-12 6 11-7 18H24Z"/><circle cx="17" cy="23" r="3"/><circle cx="23" cy="13" r="3"/><circle cx="32" cy="11" r="3"/><circle cx="41" cy="13" r="3"/><circle cx="47" cy="23" r="3"/>{base}</>,
    king: <><path d="M32 9v12M26 15h12"/><path d="M22 42c1.5-11.5 5.4-18 10-18s8.5 6.5 10 18Z"/><path d="M25 24h14"/>{base}</>,
  };
  return <svg {...common}>{shapes[kind] ?? shapes.pawn}</svg>;
}

export function EvaluationBar({ value }: { value?: number }) {
  const clamped = Math.max(-800, Math.min(800, value ?? 0));
  // Keep a large engine swing legible without letting the rail become a
  // thermometer. The rail is a visual summary; the exact score remains text.
  const whiteShare = Math.max(8, Math.min(92, 50 + Math.tanh(clamped / 340) * 42));
  const label = formatEval(value);
  const side = value === undefined ? "Even position" : value >= 0 ? `White advantage, ${label}` : `Black advantage, ${formatEval(Math.abs(value))}`;
  return <aside className="evaluation-column" aria-label={`Current engine evaluation: ${side}`}>
    <span className="evaluation-value">{label}</span>
    <div className="evaluation-rail" role="img" aria-label={side}>
      <span className="evaluation-rail-black" />
      <span className="evaluation-rail-white" style={{ height: `${whiteShare}%` }} />
      <span className="evaluation-rail-marker" style={{ bottom: `calc(${whiteShare}% - 2px)` }} />
    </div>
    <span className="evaluation-opposite">{formatEval(value === undefined ? undefined : -value)}</span>
  </aside>;
}

export function formatEval(value?: number) {
  if (value === undefined) return "—";
  const pawns = value / 100;
  return `${pawns > 0 ? "+" : ""}${pawns.toFixed(2)}`;
}
