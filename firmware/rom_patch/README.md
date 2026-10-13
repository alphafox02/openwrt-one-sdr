# firmware/rom_patch

The code-execution mechanism for the Wi-Fi MCU: run custom nds32 code inside the
MediaTek firmware by redirecting an existing hook through a handler placed in the
patch image's executable padding.

This is the proof-of-concept that established the exec path the streaming
producer relies on. It redirects one of MediaTek's own runtime hooks to a small
handler assembled into the gap in the ROM patch, then back to the original
target, so the handler runs in firmware context without displacing anything.
Proven on hardware: the handler ran on the MT7981 Wi-Fi MCU.

| File | Role |
| --- | --- |
| `build_poc_patch.py` | builds the patched ROM-patch image: assembles the handler, places it in the padding gap, and rewrites the chosen hook to reach it. |
| `capture_handler.s` | an in-firmware ICAP capture arm (the capture-setup register sequence), interposed on a runtime hook — the first in-context capture trigger. |
| `hook_marker.s` | a minimal handler that writes a marker, used to confirm the redirected hook actually executes. |
| `mem_peek.s` | a handler that reads a memory word back to the host, used to probe firmware state from inside. |

## Hazards

- Runs on the Wi-Fi MCU. A wrong hook or handler can wedge the subsystem and
  force a reboot.
- A plain reboot reverts the firmware to stock.

## Licence

MIT, © 2026 CEMAXECUTER LLC. Assembled from source; contains no MediaTek code.
See [../../LICENSE](../../LICENSE).
