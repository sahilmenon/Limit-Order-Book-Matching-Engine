# Live browser demo

The C++20 matching engine (`FastOrderBook`) compiled to WebAssembly, matching a
live synthetic order flow entirely in the browser. The whole hot loop — order
generation *and* matching — runs in WASM (`web/wasm/lob_wasm.cpp`, a `MarketSim`
wrapper); the JavaScript in `public/` only pulls a JSON snapshot each frame and
paints the depth ladder and trade tape.

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

`web/public/` is a static bundle — deploy it as-is. `_headers` sets the
`application/wasm` content type.

```sh
npx wrangler pages deploy web/public --project-name limit-order-book
```

Target custom domain: **orderbook.sahilmenon.com** (configure in the Pages
project's Custom Domains, pointing at the deployment).

## Files

```
web/wasm/lob_wasm.cpp   embind wrapper: MarketSim(FastOrderBook) -> step()/snapshot()
web/build.sh            emcc invocation producing the ES-module WASM bundle
web/public/index.html   layout
web/public/styles.css   quant-terminal styling
web/public/app.js       frame loop: step the sim, render ladder + tape + stats
web/public/_headers     Cloudflare Pages: serve .wasm as application/wasm
```
