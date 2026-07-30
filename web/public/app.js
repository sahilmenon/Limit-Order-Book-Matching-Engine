// Drives the WebAssembly matching engine and paints it. Two sources share one
// render path and one JSON snapshot shape:
//   - Synthetic: a MarketSim generating and matching random order flow in WASM.
//   - Real:      an ItchReplay rebuilding a recorded NASDAQ ITCH slice (AAPL).
// All matching and book maintenance happen in C++/WASM; this file only paints.

import createLobModule from "./lob.js";

const SYNTH = { floor: 100_000, ticks: 2_000, qtyMax: 500, scale: 0.01 };
const ITCH_SCALE = 0.0001; // ITCH prices carry 4 implied decimals ($ = raw/1e4)
const LEVELS = 12;

const $ = (id) => document.getElementById(id);
const fmtInt = (n) => n.toLocaleString("en-US");

// Current price->dollars factor; depends on the active source.
let scale = SYNTH.scale;
const fmtPrice = (t) => (t * scale).toFixed(2);

function depthRow(price, size, maxSize) {
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

function renderLadder(el, levels) {
  const maxSize = levels.reduce((m, [, s]) => Math.max(m, s), 0);
  const frag = document.createDocumentFragment();
  for (const [price, size] of levels) frag.appendChild(depthRow(price, size, maxSize));
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
    opsLabel: $("ops-label"),
  };

  let engine = null;
  let mode = "synthetic";
  let running = true;
  let batch = 12;
  let lastOps = 0;
  let lastT = performance.now();

  function disposeEngine() {
    if (engine && engine.delete) engine.delete();
    engine = null;
  }

  function buildSynthetic() {
    disposeEngine();
    const seed = (Date.now() & 0xffffffff) >>> 0;
    engine = new mod.MarketSim(seed, SYNTH.floor, SYNTH.ticks, SYNTH.qtyMax);
    scale = SYNTH.scale;
    els.opsLabel.textContent = "Orders matched";
    status.textContent = "engine live · matching synthetic flow in WebAssembly";
  }

  async function buildReal() {
    disposeEngine();
    status.textContent = "loading recorded NASDAQ ITCH…";
    const meta = await (await fetch("./sample.itch.json")).json();
    const buf = await (await fetch("./sample.itch.bin")).arrayBuffer();
    engine = new mod.ItchReplay();
    engine.load(new Uint8Array(buf));
    scale = ITCH_SCALE;
    els.opsLabel.textContent = "Executions";
    status.textContent = `replaying recorded NASDAQ ITCH · ${meta.ticker} · ${(
      meta.bytes / 1024
    ).toFixed(0)} KB`;
  }

  function frame() {
    if (running && engine) engine.step(batch);

    if (engine) {
      const snap = JSON.parse(engine.snapshot(LEVELS));
      renderLadder(els.asks, snap.asks.slice().reverse());
      renderLadder(els.bids, snap.bids);
      renderTape(els.tape, snap.tape);

      const { bid, ask, resting, ops, trades } = snap.stats;
      const haveTop = bid > 0 && ask > 0;
      els.sBid.textContent = bid > 0 ? fmtPrice(bid) : "—";
      els.sAsk.textContent = ask > 0 ? fmtPrice(ask) : "—";
      els.sSpread.textContent = haveTop ? fmtPrice(ask - bid) : "—";
      els.midPrice.textContent = haveTop ? fmtPrice((bid + ask) / 2) : "—";
      els.sResting.textContent = fmtInt(resting);
      els.sOps.textContent = fmtInt(mode === "real" ? trades : ops);

      const now = performance.now();
      if (now - lastT > 500) {
        const rate = ((ops - lastOps) * 1000) / (now - lastT);
        lastOps = ops;
        lastT = now;
        els.sRate.textContent = fmtInt(Math.round(rate)) + "/s";
      }
    }
    requestAnimationFrame(frame);
  }

  // --- Controls ------------------------------------------------------------
  const synBtn = $("mode-syn");
  const realBtn = $("mode-real");

  function setMode(next, btn) {
    if (mode === next) return;
    mode = next;
    synBtn.classList.toggle("active", next === "synthetic");
    realBtn.classList.toggle("active", next === "real");
    lastOps = 0;
    if (next === "synthetic") buildSynthetic();
  }

  synBtn.addEventListener("click", () => setMode("synthetic", synBtn));
  realBtn.addEventListener("click", async () => {
    if (mode === "real") return;
    realBtn.disabled = true;
    try {
      await buildReal();
      mode = "real";
      synBtn.classList.remove("active");
      realBtn.classList.add("active");
      lastOps = 0;
    } catch (err) {
      status.textContent = "could not load ITCH slice: " + err;
    } finally {
      realBtn.disabled = false;
    }
  });

  $("toggle").addEventListener("click", (e) => {
    running = !running;
    e.target.textContent = running ? "Pause" : "Resume";
  });
  $("speed").addEventListener("input", (e) => {
    batch = Number(e.target.value);
  });

  buildSynthetic();
  requestAnimationFrame(frame);
}

main();
