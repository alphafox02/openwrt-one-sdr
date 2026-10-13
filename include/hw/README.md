# include/hw

Register headers.

- `mt7996/` and `adie/mt7976.h` — **vendored REFERENCE headers from
  h0m3us3r/w1700k-sdr** (SPDX MIT; see each file's header and LICENSE). These
  are the W1700K's reverse-engineered MT7996 register maps. The MT7981 shares
  the **MT7976 A-die** and (confirmed on hardware) the **identical capture
  engine at the same WF-bus addresses**, so these apply directly. Vendored here
  so the repo is self-contained (no need to re-clone w1700k-sdr each session).
- `mt7981/` — the MT7981-specific headers: hardware-confirmed deltas and the
  capture-path map (`capture_map.h`), derived from the plaintext WM + mt76 +
  the mt7996/ reference above.
- `hw_bits.h` — shared bit helpers (from w1700k-sdr, MIT).
