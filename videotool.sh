#!/bin/bash
# videotool.sh — audit / fix reference videos for Unity WebGL + Android/iOS streaming
#   ./videotool.sh audit <dir|file...>        -> CSV report incl. real audio loudness
#   ./videotool.sh fix   <dir|file...>        -> writes conformant copies into ./fixed/
# Target: MP4 / H.264 8-bit yuv420p / AAC-LC stereo 48 kHz / faststart
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
MP4CHECK="$HERE/mp4check"
OUTDIR="${OUTDIR:-fixed}"

collect() {
  for a in "$@"; do
    if [ -d "$a" ]; then
      find "$a" -type f \( -iname '*.mp4' -o -iname '*.mov' -o -iname '*.m4v' \
        -o -iname '*.webm' -o -iname '*.mkv' -o -iname '*.avi' -o -iname '*.ts' \) -print0
    elif [ -f "$a" ]; then printf '%s\0' "$a"; fi
  done
}

probe() { ffprobe -v error -select_streams "$1" -show_entries stream="$2" -of csv=p=0 "$3" 2>/dev/null | head -1; }

audit() {
  echo "file,verdict,video,pix_fmt,audio,aac_profile,rate,ch,captions,mean_dB,peak_dB,samples,note"
  while IFS= read -r -d '' f; do
    vc=$(probe v:0 codec_name "$f"); px=$(probe v:0 pix_fmt "$f")
    ac=$(probe a:0 codec_name "$f"); ap=$(probe a:0 profile "$f")
    ar=$(probe a:0 sample_rate "$f"); ch=$(probe a:0 channels "$f")
    sc=$(probe s:0 codec_name "$f")
    note=""; mean=""; peak=""; ns=""
    if [ -z "$ac" ]; then
      note="NO AUDIO TRACK"
    else
      vd=$(ffmpeg -hide_banner -nostdin -i "$f" -map 0:a:0 -af volumedetect -f null - 2>&1)
      mean=$(echo "$vd" | grep -o 'mean_volume: [-0-9.]*' | tail -1 | awk '{print $2}')
      peak=$(echo "$vd" | grep -o 'max_volume: [-0-9.]*'  | tail -1 | awk '{print $2}')
      ns=$(echo "$vd"   | grep -o 'n_samples: [0-9]*'     | tail -1 | awk '{print $2}')
      [ "${ns:-0}" = "0" ] && note="AUDIO DECODES TO NOTHING"
      case "$peak" in -9[0-9]*|-inf) note="SILENT AUDIO" ;; esac
    fi
    v=$("$MP4CHECK" --no-color "$f" 2>/dev/null | grep -cE '^\s+x ')
    [ "$v" -eq 0 ] && verdict=PASS || verdict=FAIL
    printf '"%s",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' \
      "$f" "$verdict" "${vc:-none}" "${px:-}" "${ac:-none}" "${ap:-}" "${ar:-}" "${ch:-}" \
      "${sc:-none}" "${mean:-}" "${peak:-}" "${ns:-}" "$note"
  done < <(collect "$@")
}

fixone() {
  f="$1"; mkdir -p "$OUTDIR"
  out="$OUTDIR/$(basename "${f%.*}").mp4"
  vc=$(probe v:0 codec_name "$f"); px=$(probe v:0 pix_fmt "$f")
  ac=$(probe a:0 codec_name "$f"); ch=$(probe a:0 channels "$f")

  # video: copy when already conformant H.264 8-bit, else transcode (avoids generation loss)
  if [ "$vc" = "h264" ] && [ "$px" = "yuv420p" ]; then
    vopt=(-c:v copy); vmsg="copy"
  else
    vopt=(-c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p -crf 21 -preset medium)
    vmsg="transcode ($vc/$px -> h264/yuv420p)"
  fi

  if [ -z "$ac" ]; then
    echo "!! $(basename "$f"): NO AUDIO TRACK — cannot be fixed by encoding, re-export from source"
    return 1
  fi
  # mono -> stereo at unity gain (default upmix would drop 3 dB); >2ch -> downmix
  if [ "${ch:-2}" = "1" ]; then aopt=(-af "pan=stereo|c0=c0|c1=c0"); amsg="mono->stereo (unity gain)"
  elif [ "${ch:-2}" -gt 2 ] 2>/dev/null; then aopt=(-af "pan=stereo|c0=0.5*FL+0.3*FC|c1=0.5*FR+0.3*FC"); amsg="${ch}ch->stereo"
  else aopt=(); amsg="stereo"; fi

  # closed captions: carry text tracks through as mov_text. Bitmap subtitles have
  # no MP4 text form and mapping one aborts the encode, so they are named out.
  sc=$(probe s:0 codec_name "$f")
  case "$sc" in
    mov_text|subrip|srt|text|ass|ssa|webvtt|ttml|stl|subviewer|eia_608)
        sopt=(-map "0:s?" -c:s mov_text); smsg="kept" ;;
    "") sopt=(); smsg="none" ;;
    *)  sopt=(); smsg="$sc dropped (no MP4 text form)" ;;
  esac

  echo ">> $(basename "$f"): video=$vmsg audio=$amsg captions=$smsg"
  # ${x[@]+...} keeps an empty array from tripping `set -u` on bash 3.2
  encode() {
    ffmpeg -hide_banner -v warning -nostdin -y -i "$f" -map 0:v:0 -map 0:a:0 "$@" \
      "${vopt[@]}" ${aopt[@]+"${aopt[@]}"} -c:a aac -profile:a aac_low -b:a 192k -ar 48000 \
      -movflags +faststart "$out"
  }
  if ! encode ${sopt[@]+"${sopt[@]}"}; then
    # the captions broke the encode — the video is worth more than the captions
    if [ ${#sopt[@]} -gt 0 ] && encode; then
      echo "!! $(basename "$f"): captions could not be converted to mov_text — dropped"
    else
      echo "!! ffmpeg failed: $f"; return 1
    fi
  fi
  "$MP4CHECK" --no-color "$out" | sed -n '2,6p;/^  x /p'
}

case "${1:-}" in
  audit) shift; audit "$@" ;;
  fix)   shift; while IFS= read -r -d '' f; do fixone "$f"; done < <(collect "$@") ;;
  *) echo "usage: $0 {audit|fix} <dir|file...>" >&2; exit 2 ;;
esac
