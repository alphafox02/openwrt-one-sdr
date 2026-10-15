# host/spectrum

The spectrum and waterfall viewer: a 262,144-point FFT server with a browser UI.

A vendored variant of the viewer from
[W1700K-SDR](https://github.com/h0m3us3r/w1700k-sdr), copied at commit `0b0413e`
(2026-10-13) under its MIT licence (full text and credit in [LICENSE](LICENSE)),
with only minimal adaptation for this project — the page title in
`web/index.html` — marked in-file. It maps the `/w1700k-stream` shared-memory
ring, runs the FFT, and serves the page in `web/`. In this project the ring is
filled by the VITA 49 bridge in [../stream/](../stream/README.md). To refresh it,
re-copy `host/spectrum/` from that upstream at a newer commit and re-apply the
title change.

| Path | Role |
| --- | --- |
| `src/` | the C++17 server: ring reader (`stream.cpp`), FFT (`spectrum.cpp`), HTTP/WebSocket serving (`serve.cpp`, `web.cpp`), the receiver-controller protocol (`controller.cpp`), JSON (`json.cpp`) |
| `web/` | the browser UI compiled into the binary: `index.html`, `app.js`, `render.js`, `style.css` |
| `Makefile`, `embed.sh` | build; `embed.sh` bakes `web/` into the binary |

## Build and run

```sh
make                                   # needs zlib and FFTW3 (single precision)
build/w1700k-spectrum --no-controller --center 2437 --rate 40 \
    --stream /w1700k-stream --listen 127.0.0.1:8073
# then open http://127.0.0.1:8073
```

`--no-controller` runs it as a passive viewer. Its controller is a pluggable
child process (newline-delimited JSON on stdio); a controller that drives this
project's radio is future work, mapped in
[../../docs/control-plane.md](../../docs/control-plane.md).

## Licence

MIT, Copyright (c) 2026 h0m3us3r. Every file keeps its upstream SPDX header. Any
change made for this project is marked in-file. See [../../LICENSE](../../LICENSE).
