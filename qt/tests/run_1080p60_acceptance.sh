#!/usr/bin/env bash
set -euo pipefail

duration=600
url="rtmp://192.168.1.101:1935/live/test"
binary="$(cd "$(dirname "$0")/.." && pwd)/build-1080p60/ngnix_client"
result_root="$(cd "$(dirname "$0")/.." && pwd)/test-results/1080p60"

while (($#)); do
    case "$1" in
        --duration)
            duration="$2"
            shift 2
            ;;
        --url)
            url="$2"
            shift 2
            ;;
        --binary)
            binary="$2"
            shift 2
            ;;
        --result-root)
            result_root="$2"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if ! [[ "$duration" =~ ^[1-9][0-9]*$ ]]; then
    echo "duration must be a positive integer" >&2
    exit 2
fi

if [[ ! -x "$binary" ]]; then
    echo "Qt binary not executable: $binary" >&2
    exit 2
fi

if ! ss -ltn | grep -Eq '[:.]1935[[:space:]]'; then
    echo "RTMP service unavailable on port 1935" >&2
    exit 3
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="$result_root/$timestamp"
mkdir -p "$result_dir"
probe_json="$result_dir/ffprobe.json"
qt_log="$result_dir/qt.log"
resource_log="$result_dir/resources.tsv"
summary="$result_dir/summary.txt"

if ! timeout 12 ffprobe -v error \
    -select_streams v:0 \
    -show_entries stream=codec_name,width,height,avg_frame_rate,r_frame_rate \
    -of json "$url" >"$probe_json" 2>"$result_dir/ffprobe.err"; then
    echo "RTMP publisher unavailable: $url" >&2
    exit 4
fi

python3 - "$probe_json" <<'PY'
import json
import sys
from fractions import Fraction

path = sys.argv[1]
data = json.load(open(path, encoding="utf-8"))
streams = data.get("streams", [])
if not streams:
    raise SystemExit("ffprobe returned no video stream")
s = streams[0]
rate_text = s.get("avg_frame_rate") or s.get("r_frame_rate") or "0/1"
rate = float(Fraction(rate_text))
checks = {
    "codec": s.get("codec_name") == "h264",
    "width": s.get("width") == 1920,
    "height": s.get("height") == 1080,
    "fps": rate >= 59.0,
}
if not all(checks.values()):
    raise SystemExit(f"stream precondition failed: {checks}, fps={rate:.3f}")
print(f"probe_ok codec=h264 size=1920x1080 fps={rate:.3f}")
PY

qt_pid=""
cleanup() {
    if [[ -n "$qt_pid" ]] && kill -0 "$qt_pid" 2>/dev/null; then
        kill -TERM "$qt_pid" 2>/dev/null || true
        wait "$qt_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

export QT_RTMP_AUTOPLAY=1
export QT_RTMP_URL="$url"
export QT_LOGGING_RULES="*.debug=false"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"

"$binary" >"$qt_log" 2>&1 &
qt_pid=$!
printf 'second\trss_kib\tcpu_percent\n' >"$resource_log"

for ((second = 1; second <= duration; ++second)); do
    if ! kill -0 "$qt_pid" 2>/dev/null; then
        echo "Qt process exited before ${duration}s" >&2
        wait "$qt_pid" || true
        exit 5
    fi
    ps -o rss=,%cpu= -p "$qt_pid" \
        | awk -v second="$second" '{print second "\t" $1 "\t" $2}' \
        >>"$resource_log"
    sleep 1
done

kill -TERM "$qt_pid" 2>/dev/null || true
wait "$qt_pid" 2>/dev/null || true
qt_pid=""

python3 - "$qt_log" "$resource_log" "$summary" <<'PY'
import re
import statistics
import sys

qt_log, resource_log, summary = sys.argv[1:]
pattern = re.compile(
    r"\[VIDEO\]\[stats\].*输入 (\d+)x(\d+).*码流 ([0-9.]+)fps.*"
    r"解码 ([0-9.]+)fps.*显示 ([0-9.]+)fps.*覆盖 (\d+)"
)
samples = []
for line in open(qt_log, encoding="utf-8", errors="replace"):
    match = pattern.search(line)
    if match:
        samples.append(tuple(float(v) for v in match.groups()))
if len(samples) < 2:
    raise SystemExit("Qt emitted fewer than two parseable VIDEO stats samples")

tail = samples[-min(5, len(samples)):]
decode = statistics.mean(row[3] for row in tail)
render = statistics.mean(row[4] for row in tail)
if any(int(row[0]) != 1920 or int(row[1]) != 1080 for row in tail):
    raise SystemExit(f"Qt reported wrong input size: {tail}")
if decode < 59.0 or render < 59.0:
    raise SystemExit(f"Qt rate below target: decode={decode:.3f}, render={render:.3f}")

rss = []
for line in open(resource_log, encoding="utf-8"):
    fields = line.split()
    if fields and fields[0].isdigit():
        rss.append(int(fields[1]))
if len(rss) >= 10:
    first = statistics.mean(rss[:5])
    last = statistics.mean(rss[-5:])
    allowed_growth = max(32768.0, first * 0.15)
    if last - first > allowed_growth:
        raise SystemExit(
            f"RSS growth exceeds bound: first={first:.0f}KiB last={last:.0f}KiB"
        )

text = (
    f"PASS input=1920x1080 decode_fps={decode:.3f} "
    f"render_fps={render:.3f} samples={len(samples)}\n"
)
open(summary, "w", encoding="utf-8").write(text)
print(text, end="")
PY

echo "evidence=$result_dir"
