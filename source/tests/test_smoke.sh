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

collision_target="$test_dir/collision.fvblobs"
collision_link="$test_dir/collision-input.csv"
cp "$project_dir/tests/fixtures/sample-sensor.csv" "$collision_target" || exit 1
cp "$collision_target" "$test_dir/collision-before" || exit 1
ln -s "$collision_target" "$collision_link" || exit 1
if "$project_dir/fv-csvvox" \
        --input-csv "$collision_link" \
        --out-prefix "$test_dir/collision" \
        --threshold 0.10 \
        --baseline-samples 8 >/dev/null 2>&1; then
    echo "aliased input/output path was incorrectly accepted" >&2
    exit 1
fi
cmp "$collision_target" "$test_dir/collision-before" || {
    echo "input was modified during collision rejection" >&2
    exit 1
}

malformed_csv="$test_dir/malformed.csv"
{
    echo '0,10,0,0,1'
    echo '1,1000001,0,0'
    awk 'BEGIN { printf "2,10,0,0,"; for (i = 0; i < 5000; i++) printf "1"; printf "\n" }'
    cat "$project_dir/tests/fixtures/sample-sensor.csv"
} >"$malformed_csv"
"$project_dir/fv-csvvox" \
    --input-csv "$malformed_csv" \
    --out-prefix "$test_dir/csv/malformed" \
    --threshold 0.10 \
    --baseline-samples 8 \
    --seed 1 >/dev/null || exit 1
jq -e 'select(.type == "csv_session_end") | .rows_skipped == 4' \
    "$test_dir/csv/malformed.jsonl" >/dev/null || {
    echo "malformed CSV rows were not rejected as single complete rows" >&2
    exit 1
}

cp "$test_dir/first/out/session.fvblobs" "$test_dir/truncated.fvblobs" || exit 1
truncate -s 100 "$test_dir/truncated.fvblobs" || exit 1
if "$project_dir/fv-blobdump" --input "$test_dir/truncated.fvblobs" --summary; then
    echo "truncated blob stream was incorrectly accepted" >&2
    exit 1
fi

cp "$test_dir/first/out/session.fvblobs" "$test_dir/nonfinite.fvblobs" || exit 1
printf '\x00\x00\xc0\x7f' | dd of="$test_dir/nonfinite.fvblobs" bs=1 seek=44 conv=notrunc status=none
if "$project_dir/fv-blobdump" --input "$test_dir/nonfinite.fvblobs" --summary >/dev/null 2>&1; then
    echo "non-finite blob metadata was incorrectly accepted" >&2
    exit 1
fi

echo "FieldVox source smoke tests passed."
