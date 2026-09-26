.RECIPEPREFIX := >

CC ?= cc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
LDFLAGS ?= -lm

MAIN_BIN := fieldvox
DUMP_BIN := fv-blobdump
CSV_BIN := fv-csvvox

MAIN_SRC := src/main.c src/fv_core.c
DUMP_SRC := src/fv_blobdump.c src/fv_core.c
CSV_SRC := src/fv_csvvox.c src/fv_core.c
HDR := src/fv_core.h

.PHONY: all clean run inspect dump extract csvtest audition test

all: $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN)

$(MAIN_BIN): $(MAIN_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(MAIN_SRC) $(LDFLAGS)

$(DUMP_BIN): $(DUMP_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(DUMP_SRC) $(LDFLAGS)

$(CSV_BIN): $(CSV_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(CSV_SRC) $(LDFLAGS)

run: all
>./$(MAIN_BIN) --seconds 24 --threshold 1.80 --seed 0xBEEFBEEF

inspect:
>ls -lh out
>sha256sum out/* || true
>echo
>echo "=== first 12 JSONL log lines ==="
>head -n 12 out/session.jsonl || true

dump: all
>./$(DUMP_BIN) --input out/session.fvblobs --summary

extract: all
>mkdir -p out/extracted
>./$(DUMP_BIN) --input out/session.fvblobs --extract-dir out/extracted
>ls -lh out/extracted | head -n 40
>sha256sum out/extracted/*.wav | head -n 20

csvtest: all
>./$(CSV_BIN) --input-csv out/sample-sensor.csv --out-prefix out/csv-session --threshold 1.65 --baseline-samples 160 --seed 0xC5C5C5C5
>./$(DUMP_BIN) --input out/csv-session.fvblobs --summary

audition:
>ffmpeg -hide_banner -y -f concat -safe 0 -i out/audition/list.txt -af "volume=2.5,alimiter=limit=0.95" -ar 48000 -ac 1 -c:a pcm_s16le out/audition-blobs.wav

test: all
>./tests/test_smoke.sh

clean:
>rm -f $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN)
>rm -rf out
