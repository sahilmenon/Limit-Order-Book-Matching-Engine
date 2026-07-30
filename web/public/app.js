// Drives the WebAssembly matching engine: each animation frame it advances the
// in-WASM market simulator by a batch of orders, pulls a JSON snapshot, and
// renders the depth ladder, trade tape, and counters. All matching happens in
// C++/WASM; this file only paints.

import createLobModule from "./lob.js";

const FLOOR = 100_000; // price band floor, in integer ticks (cents)
const TICKS = 2_000; // ladder width
const QTY_MAX = 500;
const LEVELS = 12; // depth rows shown per side
const TICK = 0.01; // ticks -> dollars for display

const $ = (id) => document.getElementById(id);
const fmtPrice = (t) => (t * TICK).toFixed(2);
const fmtInt = (n) => n.toLocaleString("en-US");

function depthRow(side, price, size, maxSize) {
  const pct = maxSize > 0 ? Math.min(100, (size / maxSize) * 100) : 0;
  const row = document.createElement("div");
  row.className = "row";
  row.innerHTML =
    `<div class="depthbar" style="width:${pct}%"></div>` +
    `<span class="price">${fmtPrice(price)}</span>` +
    `<span class="size">${fmtInt(size)}</span>` +
    `<span class="depthcol">${pct.toFixed(0)}%</span>`;
  return row;
}

function renderLadder(el, levels, side) {
  const maxSize = levels.reduce((m, [, s]) => Math.max(m, s), 0);
  const frag = document.createDocumentFragment();
  for (const [price, size] of levels) frag.appendChild(depthRow(side, price, size, maxSize));
  el.replaceChildren(frag);
}

function renderTape(el, tape) {
  const frag = document.createDocumentFragment();
  for (const [price, size, side] of tape) {
    const row = document.createElement("div");
    row.className = `row ${side === 1 ? "buy" : "sell"}`;
    row.innerHTML =
      `<span class="t">${side === 1 ? "▲ buy " : "▼ sell"}</span>` +
      `<span class="price">${fmtPrice(price)}</span>` +
      `<span class="size">${fmtInt(size)}</span>`;
    frag.appendChild(row);
  }
  el.replaceChildren(frag);
}

async function main() {
  const status = $("status");
  let mod;
  try {
    mod = await createLobModule();
  } catch (err) {
    status.textContent = "failed to load WASM engine: " + err;
    return;
  }

  const seed = (Date.now() & 0xffffffff) >>> 0;
  const sim = new mod.MarketSim(seed, FLOOR, TICKS, QTY_MAX);
  status.textContent = "engine live — matching in WebAssembly";

  let running = true;
  let batch = 12; // orders per frame; driven by the speed slider
  let lastOps = 0;
  let lastT = performance.now();
  let rate = 0;

  const els = {
    bids: $("bids"),
    asks: $("asks"),
    tape: $("tape"),
    midPrice: $("mid-price"),
    sBid: $("s-bid"),
    sAsk: $("s-ask"),
    sSpread: $("s-spread"),
    sResting: $("s-resting"),
    sOps: $("s-ops"),
    sRate: $("s-rate"),
  };

  function frame() {
    if (running) sim.step(batch);

    const snap = JSON.parse(sim.snapshot(LEVELS));
    // Asks are best-first (ascending); show them descending so the touch sits
    // just above the mid, the conventional ladder layout.
    renderLadder(els.asks, snap.asks.slice().reverse(), "ask");
    renderLadder(els.bids, snap.bids, "bid");
    renderTape(els.tape, snap.tape);

    const { bid, ask, resting, ops, trades } = snap.stats;
    const haveTop = bid > 0 && ask > 0;
    els.sBid.textContent = bid > 0 ? fmtPrice(bid) : "—";
    els.sAsk.textContent = ask > 0 ? fmtPrice(ask) : "—";
    els.sSpread.textContent = haveTop ? fmtPrice(ask - bid) : "—";
    els.midPrice.textContent = haveTop ? fmtPrice((bid + ask) / 2) : "—";
    els.sResting.textContent = fmtInt(resting);
    els.sOps.textContent = fmtInt(ops);
    els.sRate.textContent = trades.toString();

    const now = performance.now();
    if (now - lastT > 500) {
      rate = ((ops - lastOps) * 1000) / (now - lastT);
      lastOps = ops;
      lastT = now;
      els.sRate.textContent = fmtInt(Math.round(rate)) + "/s";
    }

    requestAnimationFrame(frame);
  }

  $("toggle").addEventListener("click", (e) => {
    running = !running;
    e.target.textContent = running ? "Pause" : "Resume";
  });
  $("speed").addEventListener("input", (e) => {
    batch = Number(e.target.value);
  });

  requestAnimationFrame(frame);
}

main();
