#!/usr/bin/env bash

project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
test_dir=$(mktemp -d) || exit 1
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM

mkdir -p "$test_dir/first" "$test_dir/second" "$test_dir/csv" || exit 1

(
    cd "$test_dir/first" || exit 1
    "$project_dir/fieldvox" --seconds 2 --threshold 1.80 --seed 0xBEEFBEEF
) || exit 1
(
    cd "$test_dir/second" || exit 1
    "$project_dir/fieldvox" --seconds 2 --threshold 1.80 --seed 0xBEEFBEEF
) || exit 1

cmp "$test_dir/first/out/session.fvblobs" "$test_dir/second/out/session.fvblobs" || exit 1
cmp "$test_dir/first/out/session.wav" "$test_dir/second/out/session.wav" || exit 1
cmp "$test_dir/first/out/session.jsonl" "$test_dir/second/out/session.jsonl" || exit 1

"$project_dir/fv-blobdump" --input "$test_dir/first/out/session.fvblobs" --summary || exit 1
jq -c . "$test_dir/first/out/session.jsonl" >/dev/null || exit 1
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
    -of default=noprint_wrappers=1 "$test_dir/first/out/session.wav" >/dev/null || exit 1

"$project_dir/fv-csvvox" \
    --input-csv "$project_dir/tests/fixtures/sample-sensor.csv" \
    --out-prefix "$test_dir/csv/session" \
    --threshold 0.10 \
    --baseline-samples 8 \
    --seed 0xC5C5C5C5 || exit 1
"$project_dir/fv-blobdump" --input "$test_dir/csv/session.fvblobs" --summary || exit 1
jq -c . "$test_dir/csv/session.jsonl" >/dev/null || exit 1
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
    -of default=noprint_wrappers=1 "$test_dir/csv/session.wav" >/dev/null || exit 1

quoted_input="$test_dir/quoted\"sample.csv"
cp "$project_dir/tests/fixtures/sample-sensor.csv" "$quoted_input" || exit 1
"$project_dir/fv-csvvox" \
    --input-csv "$quoted_input" \
    --out-prefix "$test_dir/csv/quoted" \
    --threshold 0.10 \
    --baseline-samples 8 \
    --seed 1 >/dev/null || exit 1
jq -c . "$test_dir/csv/quoted.jsonl" >/dev/null || exit 1

if "$project_dir/fieldvox" --seconds 2junk >/dev/null 2>&1; then
    echo "invalid numeric argument was incorrectly accepted" >&2
    exit 1
fi
if "$project_dir/fv-csvvox" --input-csv "$quoted_input" \
        --out-prefix "$test_dir/csv/invalid" --threshold nan >/dev/null 2>&1; then
    echo "non-finite threshold was incorrectly accepted" >&2
    exit 1
fi

cp "$test_dir/first/out/session.fvblobs" "$test_dir/truncated.fvblobs" || exit 1
truncate -s 100 "$test_dir/truncated.fvblobs" || exit 1
if "$project_dir/fv-blobdump" --input "$test_dir/truncated.fvblobs" --summary; then
    echo "truncated blob stream was incorrectly accepted" >&2
    exit 1
fi

echo "FieldVox source smoke tests passed."
