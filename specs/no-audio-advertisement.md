# Spec: stop UxPlay from advertising audio support (`--no-audio-advertise`)

Status: experimental. Applies to the `uxplay` server only. Does not change the AirPlay/RAOP
wire protocol itself, does not bypass FairPlay/DRM/MFi, and does not implement AirPlay 2
multi-room audio.

## 1. Current behavior

- `uxplay -a` and `uxplay -as 0` are equivalent (`-as 0` is converted to `-a`'s effect at
  startup). Both set the internal `use_audio` flag to `false`.
- `use_audio = false` **only** affects the local GStreamer audio-rendering pipeline on the
  server: `audio_renderer_init()` is skipped, the audio bus watch is not installed, and the
  `audio_process`/`audio_get_format`/`audio_set_volume`/`audio_flush` RAOP callbacks become
  no-ops. No received audio is played on the machine running UxPlay.
- `-a`/`-as 0` do **not** change anything about how UxPlay advertises itself over
  DNS-SD/mDNS. Specifically, with `-a`:
  - UxPlay still registers the `_raop._tcp` service (AirPlay/AirTunes audio-receiver
    discovery), unconditionally, with a full, unmodified TXT record.
  - UxPlay still advertises AirPlay feature bit 9 ("audio supported") as `1` in both the
    `_airplay._tcp` TXT record's `features` field and the `_raop._tcp` TXT record's `ft`
    field, along with bits 11, 18, 19, 20, 21 (audio packet redundancy and audio format
    1–4 support).
- Consequence: a macOS or iOS client has no signal, at discovery or connection time, that
  this particular UxPlay instance doesn't want to be used for audio. macOS in particular may
  automatically select UxPlay as the system audio output when screen mirroring starts. With
  `-a`, the practical result for the user is **silence**, with no indication of why — the
  client believes it successfully routed audio to a working audio receiver.
- This is confirmed by tracing every use of the `use_audio` variable in `uxplay.cpp`: all
  call sites are inside the server-side rendering/playback path. None of them touch
  `start_dnssd()`, `register_dnssd()`, or the DNS-SD feature bitmask.

## 2. Desired behavior

Add an explicit, opt-in, experimental CLI option:

```
uxplay --no-audio-advertise [bits|raop|both]
```

Short alias: `-naa` (does not collide with any existing UxPlay option).

Bare `--no-audio-advertise` (no trailing token) is equivalent to `--no-audio-advertise both`.

| Mode | Effect |
|---|---|
| `bits` | Clear AirPlay feature bits 9, 11, 18, 19, 20, 21 in the advertised `features`/`ft` bitmask (both `_airplay._tcp` and `_raop._tcp` TXT records derive from the same bitmask). This is **exactly** the configuration already tested upstream in issues #303/#442/#489 (see §4). |
| `raop` | Skip registration of the `_raop._tcp` service entirely — it will not appear in `dns-sd -B _raop._tcp` output at all. The feature bitmask is left at its normal default. This combination has **not** been tested upstream. |
| `both` (default) | Both of the above — the narrowest advertisement UxPlay can currently produce that still offers `_airplay._tcp` mirroring. |

The mDNS records are not the only place the advertisement is served from: the RTSP
`GET /info` handler (`raop_handler_info()` in `lib/raop_handlers.h`) also returns the
`_airplay._tcp`/`_raop._tcp` TXT record contents inside its plist response — both in the
Bluetooth LE service-discovery variant (request without a `CSeq` header) and in the
bplist-`qualifier` variant. Since the raop TXT buffer is only ever built by
`dnssd_register_raop()`, in `raop`/`both` modes that buffer stays empty, and the handler
now **omits** an unbuilt/empty TXT record from the response (previously it emitted an
empty `txtRAOP` data node). The `features` integer in the same `GET /info` response is
read live from the shared bitmask, so `bits` mode was already reflected on that path.
This keeps the BLE/`GET /info` discovery surface consistent with the mDNS one, while
preserving the existing fallback where a *failed* (rather than skipped) `_raop._tcp`
registration still serves its TXT over BLE: both mDNS backends build the TXT buffer
before the network registration step, so in that case the buffer is non-empty.

In every mode, `--no-audio-advertise` **implies local audio playback is disabled**
(`use_audio` is forced to `false`), regardless of `-as <sink>` or argument order. There is no
way to narrow the advertisement while keeping local playback on — the task and this spec
treat that combination as nonsensical, not as something requiring a hard CLI error.

No CLI combination involving `--no-audio-advertise` is rejected as an error; ordering
relative to `-a`/`-as` does not matter, since `--no-audio-advertise` always wins on
`use_audio`.

## 3. Non-goals

- **Not** an implementation of AirPlay 2 multi-room audio.
- **Not** a FairPlay/DRM/MFi bypass, and does not touch pairing/authentication code paths.
- **Not** a claim of full, verified support on real macOS/iOS clients. Automated tests in
  this change cover CLI parsing and the DNS-SD advertisement construction only; see §5 for
  what is and isn't covered, and `docs/manual-test-no-audio-advertise.md` for the human
  verification procedure.
- **Not** the "display-class" / Mac-model AirPlay reclassification requested in upstream
  issue #463 (advertising `model=MacBookPro18,3` instead of `AppleTV3,2`, per the
  reverse-engineered, non-Apple OpenAirPlay spec). That proposal is speculative, untested,
  has unknown FairPlay/handshake implications per the upstream maintainer's own assessment,
  and is out of scope here.
- **Not** the default behavior. `--no-audio-advertise` is opt-in only, precisely because the
  known-risk teardown behavior described in §4 is unresolved.
- Does **not** disable audio RTP forwarding (`-artp`) or change `-al` audio latency
  reporting; those remain independent, orthogonal options.

## 4. Known risks

Upstream (`FDH2/UxPlay`) has already investigated clearing AirPlay feature bit 9
("audio supported") as a way to get video-only-on-server / audio-stays-on-client behavior:

- **Issues #303 and #442** (duplicates): a user manually cleared feature bit 9 in
  `uxplay.cpp`, with `_raop._tcp` still registered (i.e., exactly this spec's `bits` mode).
  Result: audio *does* keep playing on the client throughout the session, but the client
  sends an RTSP `TEARDOWN` request (stream type 110, "terminate video services") on the
  mirroring TCP socket almost exactly **60 seconds** (57–58s observed in captured logs)
  after mirroring starts, ending the video stream. Maintainer `fduncanh` reproduced and
  confirmed this with full debug logs, and additionally reproduced the **same** ~60s
  teardown using a completely independent, unrelated third-party AirPlay server
  implementation (`apsdk`) — leading to the conclusion that this is an AirPlay protocol
  behavior on the **client** side, not a bug in UxPlay's server code. No workaround was
  found (a missing "heartbeat"/keepalive was suspected and ruled out — the client sends its
  own heartbeat every second regardless of the server's audio-support advertisement).
  Issue #303 was closed by the maintainer as unresolved.
- **Issue #489**: a duplicate request for the same "video only" behavior. Maintainer
  confirms the identical ~60s-teardown finding and states plainly: *"we would love to add
  such a mode, but need someone to discover how to prevent the iOS client requesting a
  teardown 60 secs after the no-audio mode starts... we have no idea what to do."*
- **This implementation does not resolve the ~60s teardown.** `--no-audio-advertise bits`
  (and therefore `both`, which includes it) is expected to reproduce the same known,
  unresolved teardown behavior described above. It is included anyway because: (a) it's
  still strictly better than `-a` alone, since the *advertisement* is now honest even if the
  session eventually tears down; (b) it gives users and future contributors a
  reproducible, tested baseline matching the exact configuration upstream already
  characterized.
- **`raop` mode is untested by upstream.** Skipping `_raop._tcp` registration entirely,
  either alone or combined with the bit-clearing (`both`), has not been tried in any of the
  referenced issues. It is unknown whether this changes, avoids, or has no effect on the
  ~60s teardown, or introduces different client behavior (e.g., at the discovery level
  rather than the session level). Treat `raop` and `both` as genuinely experimental and
  unverified until confirmed via `docs/manual-test-no-audio-advertise.md` on real hardware.
- **Feature bit 30 remains set in `raop`/`both` modes.** Bit 30 is documented (both in
  UxPlay's own bit table and in the OpenAirPlay features table) as "RAOP support: with this
  bit set, the AirTunes service is not required" — i.e., the client is told the
  `_airplay._tcp` port itself is a RAOP-capable endpoint. So even with `_raop._tcp`
  unregistered, a client may still route audio via the unified advertisement. Clearing
  bit 30 as well is a further untested variant deliberately left out of scope here (a
  one-line experiment: `dnssd_set_airplay_features(dnssd, 30, 0)`); it is unknown whether
  clients would still start mirroring at all in that configuration.
- Because of the above, `--no-audio-advertise` must never become the default, and every
  startup log message it produces must say "experimental."

## 5. Testing strategy

Automated (see `tests/`):

- CLI parsing / decision logic (`audio_advertise.h`/`.cpp`, pure functions, no I/O):
  - `-a` alone → local audio disabled, advertisement warning fires, RAOP still registered,
    feature bits untouched.
  - `-as 0` alone → same warning fires (a separate code path from `-a`; both are tested
    independently since the warning is emitted after both have had a chance to run).
  - Neither `-a`/`-as 0` nor `--no-audio-advertise` → no warnings.
  - `--no-audio-advertise bits` → RAOP still registered, feature bits cleared.
  - `--no-audio-advertise raop` → RAOP not registered, feature bits untouched.
  - `--no-audio-advertise` / `--no-audio-advertise both` → RAOP not registered, feature bits
    cleared.
- DNS-SD feature-bitmask construction (`lib/dnssd.c`, pure struct manipulation, no
  network I/O):
  - Default bitmask (no options) matches the current, unchanged value
    (`features1 = 0x527FFEE6`, `features2 = 0x0`).
  - `hls_support`/`h265_support`/`setup_legacy_pairing` still flip their specific bits, in
    all combinations — regression coverage for "existing default advertisement must not
    change."
  - Applying the no-audio-advertise bit-clearing on top of the default yields exactly
    `features1 = 0x5243F4E6`, `features2 = 0x0`, and is idempotent.
- Help text: `uxplay -h` output contains `--no-audio-advertise`.

Explicitly **not** automated (requires real hardware / real mDNS traffic, see
`docs/manual-test-no-audio-advertise.md`):

- Whether `_raop._tcp` actually disappears from `dns-sd -B _raop._tcp` output on the
  network.
- Whether the `GET /info` plist response omits `txtRAOP` in `raop`/`both` modes
  (`raop_handler_info()` is a static function in a header that pulls in the full raop
  stack, so it is not unit-testable in this framework-free harness; see the `nc` wire
  probe in `docs/manual-test-no-audio-advertise.md` §3, which exercises exactly the code
  path used by BLE service discovery).
- Whether a real macOS/iOS client actually avoids selecting UxPlay as its audio output.
- Whether the ~60s TEARDOWN occurs, is avoided, or changes timing under `bits`, `raop`, or
  `both`.

## 6. Engineering note

**What Apple's public documentation confirms.** Two Apple Support articles were reviewed:
"Stream video and audio with AirPlay" (Mac User Guide,
`support.apple.com/guide/mac-help/mchld7e543a0/mac`) and "Use AirPlay to stream video or
mirror the screen of your iPhone or iPad" (`support.apple.com/en-us/102661`). Both describe
only end-user UI flows: how to start mirroring/streaming from Control Center or an app, and
the Mac-side "Allow AirPlay for" security setting. **Neither document mentions AirPlay
feature bits, RAOP, DNS-SD/mDNS TXT record fields, or any distinction between
"display-class" and "speaker-class" receivers.** Apple does not publish protocol-level
documentation for AirPlay receiver behavior in any public-facing support content found
during this investigation.

**What remains undocumented/private.** Everything this change (and the upstream issues it
draws on) relies on — the meaning of individual feature bits, the RAOP TXT record schema,
why a client tears down mirroring ~60 seconds after the audio-supported bit is cleared, and
whether any combination of advertised capabilities avoids that teardown — is reverse-engineered
knowledge from the AirPlay receiver developer community (this project, `pyatv`, `shairport-sync`,
`RPiPlay`, the unofficial OpenAirPlay spec, etc.), not Apple documentation. It should be
treated accordingly: plausible, tested-where-possible, but not authoritative.

**What was tested automatically.** CLI flag recognition and the resulting
warn/register/clear-bits decisions (`audio_advertise.h`/`.cpp`); the exact resulting AirPlay
feature bitmask for the default configuration and for each `--no-audio-advertise` mode
(`lib/dnssd.c`); that existing `hls_support`/`h265_support`/`setup_legacy_pairing` bit
overrides are unaffected by the refactor that made the bit table testable; that `-h` help
output documents the new flag.

**What still requires real macOS/iOS validation.** Everything listed under "Explicitly not
automated" in §5 above — actual client audio-output selection behavior, actual absence of
`_raop._tcp` on the wire, and actual session teardown timing under each mode. See
`docs/manual-test-no-audio-advertise.md`.

**Whether the ~60s teardown still happens.** Expected: **yes**, for `bits` and `both` modes,
since they reproduce the exact configuration (feature bit 9 off) that upstream issues
#303/#442/#489 already confirmed triggers it, and upstream found no way to avoid it —
including by testing an independent third-party AirPlay server implementation, which ruled
out a UxPlay-specific bug. Whether `raop` mode changes this at all is unknown and is exactly
the open question this change is built to let someone test next.
