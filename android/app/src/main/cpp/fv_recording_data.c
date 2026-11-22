/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fv_recording_data.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox recordings require C23
#endif

static bool ends_with(const char *s, const char *tail) {
    const size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s+a-b, tail) == 0;
}
static int compare_recordings(const void *a, const void *b) {
    const FvRecording *x=a, *y=b;
    if (x->modified != y->modified) return x->modified > y->modified ? -1 : 1;
    return strcmp(y->path, x->path);
}
void fv_recording_catalog_free(FvRecordingCatalog *c) {
    if (c) { free(c->items); *c=(FvRecordingCatalog){0}; }
}
bool fv_recording_catalog_load(FvRecordingCatalog *c, const char *dir_a, const char *dir_b,
                               const char *exclude_a, const char *exclude_b) {
    if (!c) return false;
    FvRecordingCatalog next={0}; size_t capacity=0;
    const char *dirs[]={dir_a,dir_b};
    for (size_t i=0;i<2;i++) {
        if (!dirs[i] || !*dirs[i] || (i && dir_a && strcmp(dir_a,dir_b)==0)) continue;
        DIR *dir=opendir(dirs[i]);
        if (!dir) { if (errno==ENOENT) continue; goto fail; }
        struct dirent *entry;
        errno=0;
        while ((entry=readdir(dir))) {
            const char *name=entry->d_name;
            if (name[0]=='.' || (!ends_with(name,".wav") && !ends_with(name,".mp4"))) continue;
            FvRecording item={0};
            const int n=snprintf(item.path,sizeof(item.path),"%s/%s",dirs[i],name);
            if (n<0 || (size_t)n>=sizeof(item.path)) { closedir(dir); goto fail; }
            if ((exclude_a && strcmp(item.path,exclude_a)==0) ||
                (exclude_b && strcmp(item.path,exclude_b)==0)) continue;
            struct stat st;
            if (lstat(item.path,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0) { errno=0; continue; }
            if (next.count==capacity) {
                const size_t grown=capacity ? capacity*2 : 16;
                if (grown<capacity || grown>SIZE_MAX/sizeof(*next.items)) { closedir(dir); goto fail; }
                FvRecording *p=realloc(next.items,grown*sizeof(*next.items));
                if (!p) { closedir(dir); goto fail; }
                next.items=p;capacity=grown;
            }
            snprintf(item.name,sizeof(item.name),"%s",name);
            item.modified=st.st_mtime;item.bytes=st.st_size;item.video=ends_with(name,".mp4");
            next.items[next.count++]=item;
            errno=0;
        }
        const int saved_error=errno;
        closedir(dir);
        if (saved_error) goto fail;
    }
    if (next.count>1) qsort(next.items,next.count,sizeof(*next.items),compare_recordings);
    fv_recording_catalog_free(c);*c=next;return true;
fail:
    fv_recording_catalog_free(&next);return false;
}

static void put_le16(uint8_t dst[static 2], uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8u) & 0xFFu);
}
static void put_le32(uint8_t dst[static 4], uint32_t value) {
    for (unsigned i = 0; i < 4u; i++) dst[i] = (uint8_t)(value >> (8u * i));
}
void fv_wav_build_header(uint8_t header[static 44], uint32_t sample_rate,
                         uint16_t channels, uint32_t pcm_bytes) {
    memset(header, 0, 44);
    const uint16_t block_align = (uint16_t)(channels * 2u);
    const uint32_t byte_rate = sample_rate * (uint32_t)block_align;
    memcpy(header, "RIFF", 4);
    put_le32(header + 4, pcm_bytes + 36u);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16u);
    put_le16(header + 20, 1u);
    put_le16(header + 22, channels);
    put_le32(header + 24, sample_rate);
    put_le32(header + 28, byte_rate);
    put_le16(header + 32, block_align);
    put_le16(header + 34, 16u);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, pcm_bytes);
}
static bool write_all_fd(int fd, const void *data, size_t bytes) {
    const uint8_t *p = data;
    while (bytes) {
        const ssize_t written = write(fd, p, bytes);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        p += (size_t)written;
        bytes -= (size_t)written;
    }
    return true;
}
bool fv_wav_write_fd(int fd, uint32_t sample_rate, uint16_t channels,
                     const int16_t *pcm, size_t frames) {
    if (fd < 0 || !channels || channels > 8u || (frames && !pcm) ||
        frames > (size_t)(UINT32_MAX / (2u * (size_t)channels))) return false;
    uint8_t header[44];
    const uint32_t pcm_bytes = (uint32_t)(frames * (size_t)channels * 2u);
    fv_wav_build_header(header, sample_rate, channels, pcm_bytes);
    if (!write_all_fd(fd, header, sizeof(header))) return false;
    uint8_t bytes[2048];
    const size_t total_samples = frames * (size_t)channels;
    for (size_t off = 0; off < total_samples;) {
        size_t chunk = total_samples - off;
        if (chunk > 1024u) chunk = 1024u;
        for (size_t i = 0; i < chunk; i++) {
            const uint16_t v = (uint16_t)pcm[off + i];
            bytes[2u * i] = (uint8_t)v;
            bytes[2u * i + 1u] = (uint8_t)(v >> 8u);
        }
        if (!write_all_fd(fd, bytes, chunk * 2u)) return false;
        off += chunk;
    }
    return true;
}
bool fv_wav_write_file(const char *path, uint32_t sample_rate, uint16_t channels,
                       const int16_t *pcm, size_t frames) {
    if (!path || !*path) return false;
    char temp[700];
    const int n = snprintf(temp, sizeof(temp), "%s.tmp-XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temp)) return false;
    const int fd = mkstemp(temp);
    if (fd < 0) return false;
    bool ok = fv_wav_write_fd(fd, sample_rate, channels, pcm, frames);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8u)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8u) | ((uint32_t)p[2]<<16u) | ((uint32_t)p[3]<<24u);
}
static bool wav_info(FILE *file,FvWavInfo *out) {
    struct stat st;
    uint8_t header[12];
    if (fstat(fileno(file),&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<12 ||
        fread(header,1,12,file)!=12 || memcmp(header,"RIFF",4)!=0 || memcmp(header+8,"WAVE",4)!=0) return false;
    const uint64_t end=8u+(uint64_t)le32(header+4);
    if (end<12 || end>(uint64_t)st.st_size) return false;
    FvWavInfo info={0}; uint16_t align=0; bool fmt=false,data=false;
    uint64_t data_bytes=0,offset=12;
    while (offset+8<=end) {
        uint8_t chunk[8];
        if (fseeko(file,(off_t)offset,SEEK_SET)!=0 || fread(chunk,1,8,file)!=8) return false;
        const uint64_t length=le32(chunk+4),start=offset+8;
        if (length>end-start) return false;
        if (memcmp(chunk,"fmt ",4)==0) {
            uint8_t f[16];
            if (fmt || length<16 || fread(f,1,16,file)!=16 || le16(f)!=1 || le16(f+14)!=16) return false;
            info.channels=le16(f+2);info.sample_rate=le32(f+4);align=le16(f+12);
            if (!info.channels || info.channels>8 || !info.sample_rate || info.sample_rate>384000 ||
                align!=info.channels*2u || le32(f+8)!=info.sample_rate*(uint32_t)align) return false;
            fmt=true;
        } else if (memcmp(chunk,"data",4)==0) {
            if (data) return false;
            info.data_offset=start;data_bytes=length;data=true;
        }
        offset=start+length+(length&1u);
        if (offset>end) return false;
    }
    if (!fmt || !data || !data_bytes || data_bytes%align) return false;
    info.frames=data_bytes/align;*out=info;return true;
}
bool fv_wav_read_info(const char *path,FvWavInfo *info) {
    if (!path || !info) return false;
    FILE *file=fopen(path,"rb"); if (!file) return false;
    const bool ok=wav_info(file,info);fclose(file);return ok;
}
bool fv_wav_waveform(const char *path,float peaks[FV_PLAYBACK_BINS],const atomic_bool *cancel) {
    if (!path || !peaks) return false;
    FILE *file=fopen(path,"rb");if (!file) return false;
    FvWavInfo info;
    bool ok=wav_info(file,&info) && fseeko(file,(off_t)info.data_offset,SEEK_SET)==0;
    memset(peaks,0,FV_PLAYBACK_BINS*sizeof(*peaks));
    uint8_t pcm[8192]; uint64_t at=0;
    while (ok && at<info.frames) {
        if (cancel && atomic_load(cancel)) { ok=false;break; }
        const size_t frame_bytes=(size_t)info.channels*2u;
        const uint64_t remaining=info.frames-at;
        const size_t frames=remaining<sizeof(pcm)/frame_bytes ? (size_t)remaining : sizeof(pcm)/frame_bytes;
        if (fread(pcm,frame_bytes,frames,file)!=frames) { ok=false;break; }
        for (size_t i=0;i<frames;i++) {
            const size_t bin=(size_t)((at+i)*FV_PLAYBACK_BINS/info.frames);
            for (size_t ch=0;ch<info.channels;ch++) {
                const uint16_t raw=le16(pcm+i*frame_bytes+ch*2u);
                const int32_t value=raw>=32768u ? (int32_t)raw-65536 : (int32_t)raw;
                const float level=(float)(value<0 ? -value : value)/32768.0f;
                if (level>peaks[bin]) peaks[bin]=level;
            }
        }
        at+=frames;
    }
    fclose(file);
    if (ok) fv_waveform_normalize(peaks);
    return ok;
}
bool fv_wav_load_mono_48k(const char *path,int16_t **samples,size_t *frames) {
    if (!path || !samples || !frames) return false;
    FILE *file=fopen(path,"rb");if (!file) return false;
    FvWavInfo info;
    bool ok=wav_info(file,&info) && info.channels==1 && info.sample_rate==48000 &&
            info.frames<=48000u*600u && info.frames<=SIZE_MAX/sizeof(int16_t);
    int16_t *pcm=NULL;
    if (ok) { pcm=malloc((size_t)info.frames*sizeof(*pcm));ok=pcm!=NULL; }
    if (ok) ok=fseeko(file,(off_t)info.data_offset,SEEK_SET)==0 && fread(pcm,2,(size_t)info.frames,file)==info.frames;
    fclose(file);
    if (!ok) { free(pcm);return false; }
    /* Decode explicitly so this helper also works on big-endian hosts. */
    for (size_t i=0;i<(size_t)info.frames;i++) {
        const uint16_t raw=le16((const uint8_t *)&pcm[i]);
        const int32_t value=raw>=32768u ? (int32_t)raw-65536 : (int32_t)raw;
        pcm[i]=(int16_t)value;
    }
    *samples=pcm;*frames=(size_t)info.frames;return true;
}
int64_t fv_playback_clamp_seek(int64_t ms,int64_t duration) {
    if (ms<0 || duration<=0) return 0;
    return ms>duration ? duration : ms;
}
size_t fv_adts_sync_offset(const uint8_t *buf,size_t len) {
    if (!buf || len<7u) return 0u;
    if (memcmp(buf,"ID3",3)==0 || memcmp(buf,"OggS",4)==0 ||
        memcmp(buf,"fLaC",4)==0 || memcmp(buf,"RIFF",4)==0) return 0u;
    if (buf[0]==0xFFu && (buf[1]&0xE0u)==0xE0u && (buf[1]&0x06u)!=0x00u) return 0u;
    for (size_t i=0;i+7u<=len;i++) {
        if (buf[i]!=0xFFu || (buf[i+1]&0xF6u)!=0xF0u) continue;
        const size_t flen=(((size_t)buf[i+3]&0x03u)<<11)|((size_t)buf[i+4]<<3)|(((size_t)buf[i+5]&0xE0u)>>5);
        if (flen<7u || i+flen+2u>len) continue;
        if (buf[i+flen]==0xFFu && (buf[i+flen+1]&0xF6u)==0xF0u) return i;
    }
    return 0u;
}
static bool valid_marker_kinds(unsigned kinds) {
    const unsigned mask = FV_MARK_FRAGMENT | FV_MARK_REM_POD | FV_MARK_HIT |
                          FV_MARK_BOOKMARK | FV_MARK_LEGACY;
    return kinds != 0u && (kinds & ~mask) == 0u;
}

static size_t timeline_position(const FvTimeline *timeline, int64_t ms) {
    size_t pos = 0;
    while (pos < timeline->count && timeline->markers[pos].ms < ms) pos++;
    return pos;
}

static bool timeline_add_at(FvTimeline *timeline, size_t pos, int64_t ms, unsigned kinds) {
    FvTimelineMarker *marker = timeline->markers + pos;
    if (pos < timeline->count && marker->ms == ms) {
        marker->kinds |= kinds;
        return true;
    }
    if (timeline->count == FV_TIMELINE_MAX) return false;
    memmove(marker + 1, marker, (timeline->count - pos) * sizeof(*marker));
    *marker = (FvTimelineMarker){.ms = ms, .kinds = kinds};
    timeline->count++;
    return true;
}

bool fv_timeline_add(FvTimeline *timeline, int64_t ms, unsigned kinds) {
    if (!timeline || timeline->count > FV_TIMELINE_MAX || ms < 0 || !valid_marker_kinds(kinds))
        return false;
    return timeline_add_at(timeline, timeline_position(timeline, ms), ms, kinds);
}

const FvTimelineMarker *fv_timeline_step(const FvTimeline *timeline, int64_t ms, bool forward) {
    if (!timeline || timeline->count > FV_TIMELINE_MAX) return NULL;
    const size_t pos = timeline_position(timeline, ms);
    if (!forward) return pos ? &timeline->markers[pos - 1] : NULL;
    const size_t next = pos < timeline->count && timeline->markers[pos].ms == ms ? pos + 1 : pos;
    return next < timeline->count ? &timeline->markers[next] : NULL;
}

static bool marks_path(char *out, size_t cap, const char *recording, const char *suffix) {
    if (!recording || recording[0] != '/') return false;
    const int n = snprintf(out, cap, "%s%s", recording, suffix);
    return n >= 0 && (size_t)n < cap;
}

bool fv_timeline_load(const char *recording, FvTimeline *timeline) {
    if (!timeline) return false;
    timeline->count = 0;
    char path[544];
    if (!marks_path(path, sizeof(path), recording, ".fvmarks")) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT;
    char line[64];
    const bool header = fgets(line, sizeof(line), file) != NULL;
    const bool legacy = header && strcmp(line, "FieldVox bookmarks 1\n") == 0;
    bool ok = legacy || (header && strcmp(line, "FieldVox bookmarks 2\n") == 0);
    while (ok && fgets(line, sizeof(line), file)) {
        char *end;
        errno = 0;
        const long long ms = strtoll(line, &end, 10);
        if (errno || end == line || ms < 0) {
            ok = false;
            break;
        }
        unsigned long kinds = FV_MARK_LEGACY;
        if (!legacy) {
            if (*end != ' ' || end[1] < '0' || end[1] > '9') {
                ok = false;
                break;
            }
            kinds = strtoul(end + 1, &end, 10);
        }
        ok = !errno && kinds <= UINT_MAX && strcmp(end, "\n") == 0 &&
             fv_timeline_add(timeline, (int64_t)ms, (unsigned)kinds);
    }
    if (ferror(file)) ok = false;
    fclose(file);
    if (!ok) timeline->count = 0;
    return ok;
}

bool fv_timeline_save(const char *recording, const FvTimeline *timeline) {
    if (!timeline || timeline->count > FV_TIMELINE_MAX) return false;
    for (size_t i = 0; i < timeline->count; i++) {
        const FvTimelineMarker *m = &timeline->markers[i];
        if (m->ms < 0 || !valid_marker_kinds(m->kinds) ||
            (i && m->ms <= timeline->markers[i - 1].ms)) return false;
    }
    char path[544], temp[560];
    if (!marks_path(path, sizeof(path), recording, ".fvmarks") ||
        !marks_path(temp, sizeof(temp), recording, ".fvmarks.tmp-XXXXXX")) return false;
    const int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temp);
        return false;
    }
    bool ok = fputs("FieldVox bookmarks 2\n", file) >= 0;
    for (size_t i = 0; ok && i < timeline->count; i++) {
        const FvTimelineMarker *m = &timeline->markers[i];
        ok = fprintf(file, "%lld %u\n", (long long)m->ms, m->kinds) > 0;
    }
    if (fflush(file) != 0 || fsync(fd) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}

void fv_marker_recorder_reset(FvMarkerRecorder *rec) {
    if (!rec) return;
    *rec = (FvMarkerRecorder){.last_fragment_ms = -1, .last_rem_pod_ms = -1};
}

bool fv_marker_recorder_record(FvMarkerRecorder *rec, int64_t elapsed_ms, unsigned kinds) {
    const unsigned automatic = FV_MARK_FRAGMENT | FV_MARK_REM_POD;
    if (!rec || rec->timeline.count > FV_TIMELINE_MAX || elapsed_ms < 0 || !kinds ||
        (kinds & ~(automatic | FV_MARK_HIT)) != 0u)
        return false;
    FvTimeline *timeline = &rec->timeline;
    const bool new_fragment = (kinds & FV_MARK_FRAGMENT) &&
        (rec->last_fragment_ms < 0 || elapsed_ms - rec->last_fragment_ms >= 500);
    const bool new_rem_pod = (kinds & FV_MARK_REM_POD) &&
        (rec->last_rem_pod_ms < 0 || elapsed_ms - rec->last_rem_pod_ms >= 750);
    size_t pos = timeline_position(timeline, elapsed_ms);
    const bool coincident = pos < timeline->count && timeline->markers[pos].ms == elapsed_ms;
    if (!(kinds & FV_MARK_HIT) && !new_fragment && !new_rem_pod && !coincident) return false;

    if (!timeline_add_at(timeline, pos, elapsed_ms, kinds)) {
        if (!(kinds & FV_MARK_HIT)) return false;
        size_t oldest_auto = 0;
        while (oldest_auto < timeline->count &&
               (timeline->markers[oldest_auto].kinds & FV_MARK_HIT)) oldest_auto++;
        if (oldest_auto == timeline->count) return false;
        FvTimelineMarker *oldest = &timeline->markers[oldest_auto];
        timeline->count--;
        memmove(oldest, oldest + 1, (timeline->count - oldest_auto) * sizeof(*oldest));
        if (oldest_auto < pos) pos--;
        if (!timeline_add_at(timeline, pos, elapsed_ms, kinds)) return false;
    }
    if (kinds & FV_MARK_FRAGMENT) rec->last_fragment_ms = elapsed_ms;
    if (kinds & FV_MARK_REM_POD) rec->last_rem_pod_ms = elapsed_ms;
    return true;
}

void fv_waveform_accumulate_sample(float peaks[FV_PLAYBACK_BINS], int64_t pts_us,
                                   int64_t duration_us, float abs_level) {
    if (!peaks || duration_us <= 0 || duration_us > INT64_MAX / FV_PLAYBACK_BINS ||
        pts_us < 0 || !isfinite(abs_level)) return;
    if (pts_us >= duration_us) {
        if (pts_us - duration_us <= 250000LL) pts_us = duration_us - 1;
        else return;
    }
    size_t bin = (size_t)((uint64_t)pts_us * FV_PLAYBACK_BINS / (uint64_t)duration_us);
    if (bin >= FV_PLAYBACK_BINS) bin = FV_PLAYBACK_BINS - 1u;
    const float clamped = fmaxf(0.0f, fminf(1.0f, abs_level));
    if (clamped > peaks[bin]) peaks[bin] = clamped;
}

void fv_waveform_normalize(float peaks[FV_PLAYBACK_BINS]) {
    if (!peaks) return;
    float max_peak = 0.0f;
    for (size_t i = 0; i < FV_PLAYBACK_BINS; i++) {
        if (isfinite(peaks[i]) && peaks[i] > max_peak) max_peak = peaks[i];
    }
    if (max_peak <= 0.005f || max_peak >= 0.9f) return;
    const float gain = fminf(4.0f, 0.9f / max_peak);
    for (size_t i = 0; i < FV_PLAYBACK_BINS; i++) {
        peaks[i] = fminf(1.0f, fmaxf(0.0f, peaks[i] * gain));
    }
}
