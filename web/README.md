# Live browser demo

The C++20 matching engine compiled to WebAssembly, running entirely in the
browser. Two sources share one render path:

- **Synthetic** (`MarketSim`): generates and matches random order flow through
  `FastOrderBook`. The whole hot loop runs in WASM.
- **Real** (`ItchReplay`): rebuilds a recorded NASDAQ TotalView-ITCH slice
  (Apple) through the naive `OrderBook`'s maintenance API, the same reconstruction
  path Milestone 2 validates against a Python reference.

The JavaScript in `public/` only pulls a JSON snapshot each frame and paints the
depth ladder and trade tape.

## Recorded ITCH slice

The real-data mode ships a compact, self-contained per-symbol ITCH stream
(`public/sample.itch.bin`, ~120 KB) carved from a full session so the browser
never downloads gigabytes. Regenerate it from a fetched file:

```sh
python scripts/fetch_itch.py --date 12302019 --mb 32          # -> data/itch_sample.bin
python scripts/extract_symbol_itch.py --ticker AAPL \
    --out web/public/sample.itch.bin                          # -> slice + .json sidecar
```

The source session is one of Nasdaq's free TotalView-ITCH 5.0 sample files,
published at [emi.nasdaq.com/ITCH](https://emi.nasdaq.com/ITCH/). Nasdaq owns
that data. The slice rides along in this repo so the demo can replay a real book
without pulling a multi-gigabyte session, and the MIT license covers only the
code around it.

## Build the WASM

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html):

```sh
git clone https://github.com/emscripten-core/emsdk && cd emsdk
./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh
cd -                       # back to the repo root
bash web/build.sh          # -> web/public/lob.js + web/public/lob.wasm
```

## Run locally

WASM must be served over HTTP (not `file://`) for streaming compilation:

```sh
python -m http.server -d web/public 8080
# open http://localhost:8080
```

## Deploy (Cloudflare Pages)

Live at **[orderbook.sahilmenon.com](https://orderbook.sahilmenon.com)** (Pages
project `orderbook`). `web/public/` is a static bundle; `_headers` sets the
`application/wasm` content type and `wrangler.toml` names the project.

```sh
cd web && wrangler pages deploy public   # needs CLOUDFLARE_API_TOKEN + _ACCOUNT_ID
```

## Files

```
web/wasm/lob_wasm.cpp   embind wrapper: MarketSim(FastOrderBook) -> step()/snapshot()
web/build.sh            emcc invocation producing the ES-module WASM bundle
web/public/index.html   layout
web/public/styles.css   quant-terminal styling
web/public/app.js       frame loop: step the sim, render ladder + tape + stats
web/public/_headers     Cloudflare Pages: serve .wasm as application/wasm
```
