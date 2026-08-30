#!/usr/bin/env bash
# Smoke tests for mp4check: generates fixtures with ffmpeg and asserts the
# conformance verdicts. Exits non-zero on the first failed expectation.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/mp4check"
FIX="$(mktemp -d)"
trap 'rm -rf "$FIX"' EXIT

pass=0 fail=0
ff() { ffmpeg -hide_banner -v error -nostdin -y "$@"; }

expect() { # expect <label> <file> <PASS|FAIL> [substring-that-must-appear]
  local label="$1" file="$2" want="$3" needle="${4:-}"
  local out; out="$("$BIN" --no-color "$file" 2>&1)"
  local got="PASS"; echo "$out" | grep -qE '^\s+x ' && got="FAIL"
  if [ "$got" != "$want" ]; then
    printf '  FAIL  %-26s expected %s, got %s\n' "$label" "$want" "$got"; fail=$((fail+1)); return
  fi
  if [ -n "$needle" ] && ! echo "$out" | grep -qi -- "$needle"; then
    printf '  FAIL  %-26s missing reason: %s\n' "$label" "$needle"; fail=$((fail+1)); return
  fi
  printf '  ok    %-26s %s\n' "$label" "$want"; pass=$((pass+1))
}

command -v ffmpeg >/dev/null || { echo "ffmpeg is required"; exit 2; }
[ -x "$BIN" ] || { echo "build first: make"; exit 2; }

echo "generating fixtures in $FIX"
# 1s of colour bars + a real tone, so audio is genuinely non-silent
ff -f lavfi -i testsrc=size=320x240:rate=25:duration=2 \
   -f lavfi -i "sine=frequency=440:duration=2" \
   -c:v libx264 -profile:v high -pix_fmt yuv420p -c:a aac -ar 48000 -ac 2 \
   -movflags +faststart "$FIX/good.mp4"

ff -i "$FIX/good.mp4" -c:v libx265 -tag:v hvc1 -crf 30 -c:a copy -movflags +faststart "$FIX/hevc.mp4"
ff -i "$FIX/good.mp4" -c:v copy -an -movflags +faststart "$FIX/noaudio.mp4"
ff -i "$FIX/good.mp4" -c:v copy -af volume=0 -c:a aac -movflags +faststart "$FIX/silent.mp4"
ff -i "$FIX/good.mp4" -c copy "$FIX/nofaststart.mp4"          # ffmpeg default = moov at end
ff -i "$FIX/good.mp4" -c:v libvpx-vp9 -b:v 100k -c:a libopus "$FIX/vp9.webm" 2>/dev/null \
  || cp "$FIX/good.mp4" "$FIX/vp9.webm"                        # skip if vp9 unavailable

printf '1\n00:00:00,200 --> 00:00:01,200\nhello\n\n' > "$FIX/cc.srt"
ff -i "$FIX/good.mp4" -i "$FIX/cc.srt" -map 0:v -map 0:a -map 1 -c:v copy -c:a copy \
   -c:s mov_text -metadata:s:s:0 language=eng -movflags +faststart "$FIX/captions.mp4"
# a chapter list is also a 'text' track — it must not be counted as captions
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=One\n' > "$FIX/chap.txt"
ff -i "$FIX/good.mp4" -i "$FIX/chap.txt" -map_metadata 1 -map 0:v -map 0:a -c copy \
   -movflags +faststart "$FIX/chapters.mp4"

echo "running assertions"
expect "conformant mp4"      "$FIX/good.mp4"        PASS
expect "h265/hevc rejected"  "$FIX/hevc.mp4"        FAIL "H.265"
expect "missing audio track" "$FIX/noaudio.mp4"     FAIL "NO AUDIO TRACK"
expect "encoded silence"     "$FIX/silent.mp4"      FAIL "SILENCE"
expect "moov at end of file" "$FIX/nofaststart.mp4" FAIL "faststart"
expect "webm container"      "$FIX/vp9.webm"        FAIL "WebM"
expect "captions detected"   "$FIX/captions.mp4"    PASS "capts: tx3g"
expect "missing captions"    "$FIX/good.mp4"        PASS "No closed-caption track"
expect "chapters not captions" "$FIX/chapters.mp4"  PASS "No closed-caption track"

# exit code contract: 0 when everything passes, 1 when anything fails
"$BIN" --no-color "$FIX/good.mp4" >/dev/null 2>&1
[ $? -eq 0 ] && { echo "  ok    exit code 0 on pass"; pass=$((pass+1)); } \
             || { echo "  FAIL  exit code should be 0"; fail=$((fail+1)); }
"$BIN" --no-color "$FIX/hevc.mp4" >/dev/null 2>&1
[ $? -eq 1 ] && { echo "  ok    exit code 1 on fail"; pass=$((pass+1)); } \
             || { echo "  FAIL  exit code should be 1"; fail=$((fail+1)); }

# csv mode stays machine-readable
"$BIN" --csv "$FIX/good.mp4" | head -1 | grep -q '^file,verdict' \
  && { echo "  ok    csv header"; pass=$((pass+1)); } \
  || { echo "  FAIL  csv header changed"; fail=$((fail+1)); }

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
