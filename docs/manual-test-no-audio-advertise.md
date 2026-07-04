# Manual verification: `--no-audio-advertise`

This procedure requires real hardware and cannot be automated: it exercises
actual macOS/iOS AirPlay client behavior and real mDNS traffic on a LAN.
Automated coverage (CLI parsing, exact feature-bitmask values, RAOP
registration decision, help text) is in `tests/`; see
`specs/no-audio-advertisement.md` §5 for what is and isn't covered there.

Read `specs/no-audio-advertisement.md` first, especially §4 ("Known risks")
— upstream issues #303/#442/#489 already found that clearing the AirPlay
audio-supported feature bit causes some clients to send a `TEARDOWN` request
roughly 30–60 seconds into mirroring, with no known fix. Expect to reproduce
that in step 2 below; the `raop` mode in step 2c is the genuinely open
question (untested by upstream).

For each run: note the UxPlay version/commit, the client device and OS
version, and keep the full `-d` debug log.

## 1. Baseline: `-a` (confirms the new warning, and the pre-existing gap it warns about)

On the Linux/macOS server:

```bash
uxplay -d -a
```

From a Mac or iOS device on the same network:

1. Confirm the server log prints:
   `Audio playback is disabled locally, but UxPlay still advertises audio support. macOS/iOS may still route audio to this receiver.`
2. Start screen mirroring to UxPlay.
3. Check whether macOS/iOS switches its audio output to UxPlay (Control
   Center / Sound settings).
4. Confirm whether audio becomes silent (server should produce no audio
   output, since `-a` is in effect).
5. Keep the session alive for at least 90 seconds; note whether it stays
   connected or disconnects.
6. Save the full UxPlay debug log.

Expected (this is the documented, pre-existing gap this feature addresses):
the client may still select UxPlay as its audio output, resulting in
silence, because `-a` does not change the advertisement.

## 2. Experimental mode: `--no-audio-advertise`

Repeat the following for **each** of the three modes. Run them as separate,
independent sessions (restart `uxplay` between modes).

### 2a. `bits` mode (reproduces upstream's tested, known-teardown configuration)

```bash
uxplay -d --no-audio-advertise bits
```

- Confirm the log prints:
  `Experimental no-audio advertisement mode enabled. Some AirPlay clients may disconnect after 30/60 seconds if they require an audio stream during mirroring.`
- Confirm the debug log's `register_dnssd: advertised AirPlay service with
  "Features" code = 0x...` line shows `0x5243F4E6` (see
  `specs/no-audio-advertisement.md` for how this value is derived).
- Start screen mirroring. Check whether macOS keeps local speakers/headphones
  as the audio output, or still selects UxPlay.
- Keep the session alive for **at least 90 seconds**. Watch specifically for
  an RTSP `TEARDOWN` in the debug log around the 30–60 second mark, and note
  the exact elapsed time if it occurs.
- Save the full debug log.

### 2b. `raop` mode (untested by upstream — the primary open question)

```bash
uxplay -d --no-audio-advertise raop
```

- Confirm the log prints the "skipping `_raop._tcp` registration" line.
- Repeat the same mirroring/90-second/TEARDOWN observations as 2a.
- Since the feature bitmask is untouched in this mode, also note whether the
  client behaves differently from the `-a`-alone baseline (step 1) purely
  from `_raop._tcp` being absent.
- Also run the `GET /info` wire probe from §3 below: `txtRAOP` must be absent
  from the response, and the debug log should show the
  `raop_handler_info: raop TXT record empty (service not registered), omitting txtRAOP`
  line when the probe runs.
- Note: AirPlay feature bit 30 ("RAOP support: with this bit set, the AirTunes
  service is not required") is still set in this mode, so a client may treat
  the `_airplay._tcp` port itself as RAOP-capable. If you want to go further,
  bit 30 can additionally be cleared in the source (one line:
  `dnssd_set_airplay_features(dnssd, 30, 0)`) — report results upstream.

### 2c. `both` mode (default — combination, also untested by upstream)

```bash
uxplay -d --no-audio-advertise
```

- Confirm both log lines from 2a and 2b appear.
- Repeat the same mirroring/90-second/TEARDOWN observations.
- Compare against 2a and 2b: does combining the two change the teardown
  timing at all, or behave identically to `bits` alone?

## 3. Service discovery inspection

From a Mac (or any host with `dns-sd`), compare what's advertised in each
mode above:

```bash
dns-sd -B _airplay._tcp
dns-sd -B _raop._tcp
```

Expected:
- `_airplay._tcp`: UxPlay's instance should appear in every mode (baseline,
  `bits`, `raop`, `both`).
- `_raop._tcp`: UxPlay's instance should appear in the baseline and `bits`
  modes, and should be **absent** in `raop` and `both` modes.

To inspect the actual TXT record content (e.g. to confirm the `features`/`ft`
value on the wire matches what the debug log reports), resolve the service
and dump its TXT record:

```bash
dns-sd -L "<UxPlay instance name>" _airplay._tcp local
dns-sd -Z _airplay._tcp local   # zone-file-style dump, includes TXT records
```

### BLE / `GET /info` path (no BLE hardware needed)

UxPlay also serves both TXT record contents over its RTSP `GET /info`
handler; this is the path Bluetooth LE service discovery uses. The BLE-shaped
branch triggers on any CSeq-less RTSP/1.0 request whose URL contains the
`txtAirPlay`/`txtRAOP` tokens, so it can be probed with plain `nc` from any
host — no BLE hardware required (`curl` won't work: it speaks HTTP/1.1, and
this branch requires RTSP/1.0). The port is the TCP port advertised by the
`_airplay._tcp` service (shown by `dns-sd -L` above; UxPlay uses one port for
both services):

```bash
printf 'GET /info?txtAirPlay?txtRAOP RTSP/1.0\r\n\r\n' | nc <server-ip> <port>
```

Expected: the binary-plist response body contains the `txtAirPlay` key in
every mode, and contains `txtRAOP` only in the baseline and `bits` modes. In
`raop` and `both` modes `txtRAOP` must be **absent** (not present-but-empty),
and the server's `-d` log should print the "omitting txtRAOP" debug line.

## 4. Reporting results

For each of the four runs (baseline, `bits`, `raop`, `both`), record:

- Did the client select UxPlay as its audio output?
- Did the session survive 90+ seconds, or did it TEARDOWN? At what elapsed
  time?
- Did `_raop._tcp` appear/disappear as expected?
- Anything unexpected in the debug log.

If `raop` or `both` mode avoids the TEARDOWN that `bits` alone reproduces,
that is a significant finding for the still-open upstream issues
[#303](https://github.com/FDH2/UxPlay/issues/303),
[#442](https://github.com/FDH2/UxPlay/issues/442), and
[#489](https://github.com/FDH2/UxPlay/issues/489) — worth reporting back
upstream with the full debug logs from this procedure.
