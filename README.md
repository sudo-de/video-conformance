# Video Conformance

Checks and repairs reference videos for a Unity app that streams MP4s from a
backend to **WebGL and mobile**. It parses the MP4 container atom-by-atom, tells
you exactly why a file will fail on those platforms, and re-encodes it to the
spec — as a CLI, a batch script, or a drag-and-drop web app.

It exists because "the audio doesn't work" is almost never a codec problem, and
proving that requires evidence rather than assertion.

## The spec it enforces

| | Required | Why |
|---|---|---|
| Container | MP4 (ISO-BMFF) | WebM/MKV/AVI fail on Android and WebGL |
| Video | H.264 / `avc1`, 8-bit yuv420p | WebGL cannot decode H.265; 10-bit isn't hardware-accelerated on mobile |
| Audio | AAC-LC / `mp4a` (OTI `0x40`, AOT 2) | HE-AAC decodes as silence or half-rate in WebGL |
| Channels | stereo, 48 kHz | mono is the usual odd one out in a delivery batch |
| Layout | `moov` before `mdat` (faststart) | otherwise byte-range streaming stalls until fully buffered |

Closed captions are reported but never enforced — a file without them still
passes. What the tool guarantees is that it does not *lose* them: an existing
text caption track is carried into the fixed MP4 as `mov_text`, language tag
included, and a chapter list (a `text` track named by a `tref chap` reference)
is not mistaken for one.

No browser renders in-band timed text inside `<video>`, so `mp4web` also
extracts the kept track as a **WebVTT sidecar** and attaches it to its preview
player with `<track>` — the only form that actually displays. Ship the same
`.vtt` next to the MP4 for WebGL; `GET /api/captions/:id` serves it.

## What it detects

Codec-level checks are the easy half. These are the ones that actually explain a
silent video whose codecs are already correct:

- **No audio track** — exported without sound
- **Encoded silence** — a valid AAC track carrying ~2 kbps of nothing
- **Disabled track** — `tkhd` flags with bit 0 clear; players skip it
- **Muted at container level** — `tkhd` volume of 0.0
- **Truncated audio** — audio much shorter than video
- **Leading empty edit** — an `elst` delay Unity ignores
- **`moov` at end of file** — `ffmpeg -c copy` does this *by default*
- **Captions silently dropped** — plain `-map 0:v -map 0:a` discards them

Plus H.265/AV1/VP9, WebM/MKV/AVI containers, MP3-in-MP4, HE-AAC, odd sample
rates, multichannel audio, and 10-bit profiles.

## Quick start

```sh
make          # build mp4check + mp4web
make test     # 12 smoke tests (needs ffmpeg)
make serve    # web UI + API on http://localhost:8787
```

Requires a C++17 compiler, `ffmpeg` on `PATH`, and Node 20.19+ or 22.12+ for
the frontend (Vite's floor).

## The three tools

### `mp4check` — analysis, no dependencies

Pure C++ ISO-BMFF parser. Never invokes ffmpeg, so it is fast enough to sweep a
whole delivery folder.

```sh
./mp4check video.mp4                     # one file
./mp4check /path/to/Unit31 /path/to/Unit32   # recurse directories
./mp4check units/ --csv > report.csv      # machine-readable
./mp4check units/ --script fix.sh          # emit ffmpeg commands for failures
```

```
[FAIL] unit34.mp4
  MP4 / ISO-BMFF  0.79 MB   brand=isom [isom,iso2,avc1,mp41]
  video: avc1  1920x1080  High@4.0  6.00s  180 frames  1097 kbps
  audio: mp4a  MPEG-4 Audio (AAC) / AAC-LC  48000 Hz  1ch  6.02s  283 frames
  atrak: id=2 flags=0x000003 (enabled) volume=1.00 elst=1  2 kbps (1162 bytes)
  layout: moov@824065 mdat@40 -> NOT faststart
  x Audio stream is only 2 kbps — that is encoded SILENCE, not speech. The
    source had no audio or was muted before export.
  x 'moov' atom is at the END of the file (not faststart) — streaming from AWS
    stalls or plays without audio until fully buffered. Add -movflags +faststart.
  fix: ffmpeg -y -i "unit34.mp4" -map 0:v:0 -map 0:a:0? -c:v libx264 ...
```

Every failure line carries the reason and the exact command that fixes it.

Exit code is `0` when every file passes, `1` when any fails — so it drops
straight into a pre-upload CI gate.

### `mp4web` — upload → progress → download

A dependency-free C++ HTTP server with a React/TypeScript frontend. Drop files
in, watch live progress, download the fixed MP4s. Each file shows a
**before → after** comparison of every checked field, which is the artifact you
send a vendor who insists the codec is wrong.

```sh
make serve                    # build everything, serve on :8787
./mp4web 9000 --dist web/dist # custom port
```

Uploads stream to disk and are parsed with `mmap`, so a multi-GB drop never sits
in RAM. Progress comes from ffmpeg's `-progress` output over SSE. Concurrent
encodes are capped at `cores/4`.

| Endpoint | Purpose |
|---|---|
| `POST /api/upload` | multipart upload, returns job ids |
| `GET /api/events` | SSE stream of the live job list |
| `GET /api/download/:id` | the fixed file |
| `GET /api/captions/:id` | WebVTT sidecar for the fixed file, when it has one |
| `GET /api/report.csv` | audit report for every job |
| `DELETE /api/job/:id` | forget one job and delete its files |
| `DELETE /api/jobs` | forget all of them |

Every card carries **Re-upload** (pick a replacement file — the usual answer to
a FAIL, which is always "re-export from the source") and **Remove**. Removing a
job that is still converting cancels it: the entry is forgotten immediately and
the encode's output is thrown away when it lands.

### `videotool.sh` — batch audit with real loudness

Wraps `mp4check` with ffmpeg decoding, so it catches audio that is *present and
valid but inaudible* — which container parsing cannot see.

```sh
./videotool.sh audit units/ > report.csv
./videotool.sh fix   units/          # writes into ./fixed/
```

## How files are repaired

Video is **stream-copied** when it is already conformant H.264 8-bit, so there
is no generation loss; it is transcoded only when it genuinely has to be.

```sh
ffmpeg -i IN -map 0:v:0 -map 0:a:0 -map 0:s? \
  -c:v copy \
  -af "pan=stereo|c0=c0|c1=c0" \
  -c:a aac -profile:a aac_low -b:a 192k -ar 48000 \
  -c:s mov_text \
  -movflags +faststart OUT.mp4
```

The subtitle mapping is added only for text captions (`tx3g`, `subrip`, `ass`,
`webvtt`, …). Bitmap tracks have no MP4 text form and mapping one aborts the
whole encode, so they are left out by name; if a text track fails anyway, the
encode is retried without it rather than failing the file.

The `pan` filter is deliberate. ffmpeg's default mono→stereo upmix applies a
−3 dB energy-preserving gain, which makes an audio complaint *worse*; `pan` at
unity keeps the original level.

Some files cannot be fixed by encoding. A missing audio track or an all-silent
one is reported as such rather than being re-wrapped into a file that looks
compliant and still has no sound.

## Development

```sh
make dev      # API on :8787, Vite dev server with HMR on :5173
```

Vite proxies `/api` to the C++ server. Do **not** add
`--compile-commands-dir=${workspaceFolder}` to `clangd.arguments` — VS Code does
not expand that variable there and clangd silently loses the compilation
database. It finds `compile_commands.json` on its own.

## Docker

```sh
docker compose up --build      # http://localhost:8787
```

Three-stage build; the runtime image carries only ffmpeg, the two binaries, and
the static UI, and runs as a non-root user. Uploads and output live in the
`/app/.mp4web` volume.

## Layout

```
mp4probe.hpp      ISO-BMFF parser + conformance rules (shared)
mp4check.cpp      CLI front-end
mp4web.cpp        HTTP API server
videotool.sh      batch audit/fix with loudness measurement
tests/smoke.sh    fixture-driven test suite
web/              React + TypeScript + Vite frontend
```

`mp4probe.hpp` is the single source of truth — the CLI and the server share one
parser, so they can never disagree about a verdict.

## Caveats

- Container parsing proves a real audio stream exists; it cannot prove the
  content is *audible*. Use `videotool.sh audit` for measured loudness.
- The server binds `0.0.0.0`, so it is reachable on your LAN.
- `make clean` removes `web/dist`; run `make web` (or `make serve`) afterwards.
