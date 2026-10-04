#!/usr/bin/env bash
# Generate the test media used by the engine/render tests, plus a framemd5 reference for each
# clip (one md5 per decoded frame in the decoder's native pixel format, presentation order).
#
#   tests/fixtures/gen_fixtures.sh [OUT_DIR] [--bench]
#
# Needs an ffmpeg CLI with libx264, libx265, libvpx, libsvtav1 (Ubuntu's ffmpeg has all). libaom is
# optional: Homebrew's ffmpeg lacks it, and the av1_aom clip is then skipped.
# Override the binary with FFMPEG=/path/to/ffmpeg. --bench additionally renders a large 4K HEVC
# 10-bit clip with a 250-frame GOP for scvp-bench.
#
# manifest.tsv columns: name  kind  path  open-args  frames
#   kind: container | rawes | rawvideo | imageseq | compare (second input of comparison tests)
#   open-args: rawvideo -> pix_fmt:WxH:fps   imageseq -> fps   others -> "-"
set -euo pipefail

FF="${FFMPEG:-ffmpeg}"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/out"
BENCH=0
for a in "$@"; do
  case "$a" in
    --bench) BENCH=1 ;;
    *) OUT="$a" ;;
  esac
done
mkdir -p "$OUT"
cd "$OUT"
rm -f manifest.tsv

# Temporal noise makes every frame unique, so a wrong frame index can never match by accident.
SRC="testsrc2=size=320x240:rate=30,noise=alls=12:allf=t+u"
SRC_LAVFI=(-f lavfi -i "$SRC")

ff() { SVT_LOG=0 "$FF" -nostdin -hide_banner -loglevel error -y "$@"; }
# `-h encoder=X` exits 0 either way; only a known encoder prints its "Encoder X" header. (Not
# piped into grep -q: ffmpeg would die of SIGPIPE, which pipefail turns into "missing".)
has_encoder() { [[ "$("$FF" -hide_banner -h "encoder=$1" 2>/dev/null)" == "Encoder $1 "* ]]; }

# ref <name> <kind> <path> <open-args> <ffmpeg input args...>
ref() {
  local name="$1" kind="$2" path="$3" open="$4"; shift 4
  "$FF" -nostdin -v error -noautorotate "$@" -map 0:v:0 -fps_mode passthrough -f framemd5 - \
    > "$name.framemd5"
  local frames
  frames=$(grep -vc '^#' "$name.framemd5")
  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$kind" "$path" "$open" "$frames" >> manifest.tsv
  printf '  %-22s %5s frames  (%s)\n' "$name" "$frames" "$path"
}

echo "Generating fixtures into $OUT"

# --- containers --------------------------------------------------------------------------
ff "${SRC_LAVFI[@]}" -t 6 -c:v libx264 -preset veryfast -bf 3 -b-pyramid normal \
   -g 48 -keyint_min 48 -sc_threshold 0 -pix_fmt yuv420p h264_b.mp4
ref h264_b container h264_b.mp4 - -i h264_b.mp4

# Cutting between keyframes with stream copy creates an edit list + AV_PKT_FLAG_DISCARD pre-roll.
ff -ss 1.37 -i h264_b.mp4 -c copy h264_trim.mp4
ref h264_trim container h264_trim.mp4 - -i h264_trim.mp4

ff "${SRC_LAVFI[@]}" -t 6 -c:v libx264 -preset veryfast -bf 3 \
   -x264-params open-gop=1:keyint=24:min-keyint=24:scenecut=0 h264_opengop.mkv
ref h264_opengop container h264_opengop.mkv - -i h264_opengop.mkv

ff "${SRC_LAVFI[@]}" -t 6 -c:v libx265 -preset ultrafast \
   -x265-params keyint=60:min-keyint=60:scenecut=0:open-gop=1:bframes=4:log-level=error hevc_opengop.mkv
ref hevc_opengop container hevc_opengop.mkv - -i hevc_opengop.mkv

ff "${SRC_LAVFI[@]}" -t 4 -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le \
   -x265-params keyint=30:min-keyint=30:log-level=error -tag:v hvc1 hevc10.mp4
ref hevc10 container hevc10.mp4 - -i hevc10.mp4

ff "${SRC_LAVFI[@]}" -t 4 -c:v libvpx-vp9 -deadline good -cpu-used 8 -b:v 1M \
   -auto-alt-ref 1 -lag-in-frames 25 -g 60 vp9.webm
ref vp9 container vp9.webm - -i vp9.webm

ff "${SRC_LAVFI[@]}" -t 4 -c:v libsvtav1 -preset 12 -g 60 av1_svt.mkv
ref av1_svt container av1_svt.mkv - -i av1_svt.mkv

# No test names this clip (it is one more row in the decode-every-fixture tests), so it may be
# missing; the clips tests open by name (vp9, av1_svt, ...) stay mandatory.
if has_encoder libaom-av1; then
  ff "${SRC_LAVFI[@]}" -t 2 -c:v libaom-av1 -cpu-used 8 -b:v 500k -g 30 -lag-in-frames 19 av1_aom.mkv
  ref av1_aom container av1_aom.mkv - -i av1_aom.mkv
else
  printf '  %-22s skipped (no libaom-av1 encoder)\n' av1_aom
fi

ff "${SRC_LAVFI[@]}" -t 4 -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le prores.mov
ref prores container prores.mov - -i prores.mov

ff "${SRC_LAVFI[@]}" -t 4 -c:v mjpeg -q:v 3 -pix_fmt yuvj420p mjpeg.avi
ref mjpeg container mjpeg.avi - -i mjpeg.avi

ff "${SRC_LAVFI[@]}" -t 4 -c:v libx264 -preset veryfast -bf 3 -g 48 -sc_threshold 0 h264_b.avi
ref h264_b_avi container h264_b.avi - -i h264_b.avi

# MPEG-4 Part 2 with B-frames in AVI: the demuxer gives anchors no pts.
ff "${SRC_LAVFI[@]}" -t 4 -c:v mpeg4 -bf 2 -g 24 -q:v 4 mpeg4_b.avi
ref mpeg4_b_avi container mpeg4_b.avi - -i mpeg4_b.avi

ff "${SRC_LAVFI[@]}" -t 4 -c:v ffv1 -pix_fmt yuv420p10le ffv1_10bit.mkv
ref ffv1_10bit container ffv1_10bit.mkv - -i ffv1_10bit.mkv
# The same pictures without the first 3 frames (lossless): pairs with ffv1_10bit at offset -3.
ff -i ffv1_10bit.mkv -vf "trim=start_frame=3,setpts=PTS-STARTPTS" -c:v ffv1 -pix_fmt yuv420p10le ffv1_10bit_trim3.mkv
ref ffv1_10bit_trim3 compare ffv1_10bit_trim3.mkv - -i ffv1_10bit_trim3.mkv

# Variable frame rate: pauses after frame 30 and 60.
VFR="setpts='if(lt(N,30),N,if(lt(N,60),N+15,N+30))/30/TB'"
ff "${SRC_LAVFI[@]}" -t 4 -vf "$VFR" -fps_mode passthrough -c:v libx264 -preset veryfast \
   -bf 2 -g 30 vfr.mkv
ref vfr_mkv container vfr.mkv - -i vfr.mkv
ff "${SRC_LAVFI[@]}" -t 4 -vf "$VFR" -fps_mode passthrough -c:v libx264 -preset veryfast \
   -bf 2 -g 30 -video_track_timescale 30000 vfr.mp4
ref vfr_mp4 container vfr.mp4 - -i vfr.mp4

ff -display_rotation 90 -i h264_b.mp4 -c copy rot90.mp4
ref rot90 container rot90.mp4 - -i rot90.mp4

# --- MPEG-TS -----------------------------------------------------------------------------
ff "${SRC_LAVFI[@]}" -t 4 -c:v mpeg2video -q:v 4 -bf 2 -g 15 -f mpegts mpeg2.ts
ref mpeg2_ts container mpeg2.ts - -i mpeg2.ts

ff "${SRC_LAVFI[@]}" -t 2 -c:v mpeg2video -q:v 4 -bf 2 -g 15 -f mpegts seg1.ts
ff -f lavfi -i "testsrc2=size=320x240:rate=30,hue=h=90,noise=alls=12:allf=t+u" -t 2 \
   -c:v mpeg2video -q:v 4 -bf 2 -g 15 -output_ts_offset 1000 -f mpegts seg2.ts
cat seg1.ts seg2.ts > ts_discont.ts
rm -f seg1.ts seg2.ts
ref ts_discont container ts_discont.ts - -i ts_discont.ts

ff -ss 1.1 -i mpeg2.ts -c copy -copyinkf -f mpegts ts_midgop.ts
ref ts_midgop container ts_midgop.ts - -i ts_midgop.ts

# Field order via setfield: FFmpeg 9 dropped the -top option.
ff "${SRC_LAVFI[@]}" -t 4 -vf "tinterlace=mode=interleave_top,setfield=tff" -c:v mpeg2video -q:v 4 \
   -flags +ilme+ildct -bf 2 -g 15 -f mpegts interlaced.ts
ref interlaced_ts container interlaced.ts - -i interlaced.ts

# --- raw elementary streams (no container timestamps) -------------------------------------
ff "${SRC_LAVFI[@]}" -t 6 -c:v libx264 -preset veryfast -bf 3 -g 48 -sc_threshold 0 -f h264 raw.h264
ref raw_h264 rawes raw.h264 - -f h264 -i raw.h264

ff "${SRC_LAVFI[@]}" -t 6 -c:v libx264 -preset veryfast -bf 3 \
   -x264-params open-gop=1:keyint=24:min-keyint=24:scenecut=0 -f h264 raw_opengop.h264
ref raw_opengop_h264 rawes raw_opengop.h264 - -f h264 -i raw_opengop.h264

ff "${SRC_LAVFI[@]}" -t 6 -c:v libx265 -preset ultrafast \
   -x265-params keyint=60:min-keyint=60:scenecut=0:open-gop=1:bframes=4:log-level=error -f hevc raw.h265
ref raw_h265 rawes raw.h265 - -f hevc -i raw.h265

ff "${SRC_LAVFI[@]}" -t 4 -c:v mpeg2video -bf 2 -g 24 -q:v 4 -f mpeg2video raw.m2v
ref raw_m2v rawes raw.m2v - -f mpegvideo -i raw.m2v

# --- raw video frames ----------------------------------------------------------------------
for fmt in yuv420p nv12 yuv420p10le uyvy422; do
  f="clip_320x240_${fmt}.yuv"
  ff "${SRC_LAVFI[@]}" -frames:v 30 -pix_fmt "$fmt" -f rawvideo "$f"
  ref "raw_${fmt}" rawvideo "$f" "${fmt}:320x240:30" \
    -f rawvideo -pixel_format "$fmt" -video_size 320x240 -framerate 30 -i "$f"
done

# --- image sequences -----------------------------------------------------------------------
rm -rf png_seq png16_seq png_start7 png_gap jpg_seq
mkdir -p png_seq png16_seq png_start7 png_gap jpg_seq
ff "${SRC_LAVFI[@]}" -frames:v 30 -start_number 0 png_seq/frame_%04d.png
ref png_seq imageseq png_seq 30 -framerate 30 -start_number 0 -i png_seq/frame_%04d.png
ff "${SRC_LAVFI[@]}" -frames:v 10 -pix_fmt rgb48be -start_number 0 png16_seq/frame_%04d.png
ref png16_seq imageseq png16_seq 30 -framerate 30 -start_number 0 -i png16_seq/frame_%04d.png
ff "${SRC_LAVFI[@]}" -frames:v 10 -start_number 7 png_start7/img%03d.png
ref png_start7 imageseq png_start7 30 -framerate 30 -start_number 7 -i png_start7/img%03d.png
ff "${SRC_LAVFI[@]}" -frames:v 12 -start_number 0 png_gap/frame_%04d.png
rm -f png_gap/frame_0005.png
ref png_gap imageseq png_gap 30 -framerate 30 -pattern_type glob -i 'png_gap/*.png'
ff "${SRC_LAVFI[@]}" -frames:v 20 -q:v 3 -start_number 0 jpg_seq/frame_%04d.jpg
ref jpg_seq imageseq jpg_seq 30 -framerate 30 -start_number 0 -i jpg_seq/frame_%04d.jpg

# --- benchmark clip ------------------------------------------------------------------------
if (( BENCH )); then
  echo "Rendering 4K HEVC 10-bit benchmark clip (slow)..."
  ff -f lavfi -i "testsrc2=size=3840x2160:rate=60,noise=alls=6:allf=t+u" -t 20 \
     -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le \
     -x265-params keyint=250:min-keyint=250:scenecut=0:log-level=error bench_4k_hevc10.mkv
  ref bench_4k_hevc10 container bench_4k_hevc10.mkv - -i bench_4k_hevc10.mkv
fi

echo "Wrote $(wc -l < manifest.tsv) fixtures to $OUT/manifest.tsv"
