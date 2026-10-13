#define _POSIX_C_SOURCE 200809L
#include "fv_fragment_store.h"
#include "fv_recording_data.h"
#include "fv_local_bank.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { int16_t *pcm; size_t frames; int station, fd; } Job;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    bool started, running;
    Job jobs[8];
    size_t head, count;
    char directory[512];
    unsigned sequence;
} store = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
static void le32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8u * i));
}
/* Pair a compressed original with the WAV used by native recording/analysis.
 * pread keeps shared descriptor offsets unchanged while voices read the source. */
static bool save_original(int source, const char *wav) {
    char final[660], temp[680];
    int n=snprintf(final,sizeof(final),"%s.audio",wav);
    if (n<0 || (size_t)n>=sizeof(final)) return false;
    n=snprintf(temp,sizeof(temp),"%s.tmp-XXXXXX",final);
    if (n<0 || (size_t)n>=sizeof(temp)) return false;
    const int fd=mkstemp(temp); if (fd<0) return false;
    bool ok=true; off_t off=0; uint8_t bytes[4096];
    for (;;) {
        const ssize_t got=pread(source,bytes,sizeof(bytes),off);
        if (got<0 && errno==EINTR) continue;
        if (got<0) { ok=false; break; }
        if (!got) break;
        size_t written=0;
        while (written<(size_t)got) {
            const ssize_t count=write(fd,bytes+written,(size_t)got-written);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) { ok=false; break; }
            written+=(size_t)count;
        }
        if (!ok) break;
        off+=got;
    }
    if (fsync(fd)!=0) ok=false;
    if (close(fd)!=0) ok=false;
    if (ok) ok=rename(temp,final)==0;
    if (!ok) unlink(temp);
    return ok;
}
static void prune_originals(void) {
    DIR *dir=opendir(store.directory); if (!dir) return;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        const size_t len=strlen(entry->d_name);
        if (len<15u || strncmp(entry->d_name,"clip-",5)!=0 || strcmp(entry->d_name+len-10u,".wav.audio")!=0) continue;
        char original[800],wav[800];
        const int n=snprintf(original,sizeof(original),"%s/%s",store.directory,entry->d_name);
        if (n<6 || (size_t)n>=sizeof(original)) continue;
        memcpy(wav,original,(size_t)n+1u); wav[n-6]=0;
        struct stat st;
        if (stat(wav,&st)!=0 && errno==ENOENT) unlink(original);
    }
    closedir(dir);
}
static bool save_job(const Job *job) {
    struct timespec time;
    if (clock_gettime(CLOCK_REALTIME, &time) != 0) return false;
    const uint64_t stamp = (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
    char final[640], temp[660];
    const int n = snprintf(final, sizeof(final), "%s/clip-%020llu-%03d-%06u.wav", store.directory,
                           (unsigned long long)stamp, job->station, store.sequence++);
    if (n < 0 || (size_t)n >= sizeof(final)) return false;
    const int nt = snprintf(temp, sizeof(temp), "%s.tmp-XXXXXX", final);
    if (nt < 0 || (size_t)nt >= sizeof(temp)) return false;
    int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temp); return false; }
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4); le32(header + 4, 36u + (uint32_t)job->frames * 2u);
    memcpy(header + 8, "WAVEfmt ", 8); le32(header + 16, 16u);
    header[20] = 1; header[22] = 1; le32(header + 24, FV_FRAGMENT_RATE);
    le32(header + 28, FV_FRAGMENT_RATE * 2u); header[32] = 2; header[34] = 16;
    memcpy(header + 36, "data", 4); le32(header + 40, (uint32_t)job->frames * 2u);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    uint8_t bytes[2048];
    for (size_t off = 0; ok && off < job->frames;) {
        size_t count = job->frames - off; if (count > 1024u) count = 1024u;
        for (size_t i = 0; i < count; i++) {
            const uint16_t v = (uint16_t)job->pcm[off + i];
            bytes[2u*i] = (uint8_t)v; bytes[2u*i+1u] = (uint8_t)(v >> 8u);
        }
        ok = fwrite(bytes, 2u, count, f) == count; off += count;
    }
    if (fflush(f) != 0 || fsync(fd) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(temp, final) == 0;
    if (!ok) unlink(temp);
    if (ok) {
        if (job->fd>=0) save_original(job->fd,final);
        fv_local_bank_enforce_cache_cap(store.directory, FV_FRAGMENT_CACHE_MAX);
        prune_originals();
    }
    return ok;
}
static void *store_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&store.mutex);
        while (store.running && !store.count) pthread_cond_wait(&store.changed, &store.mutex);
        if (!store.running && !store.count) { pthread_mutex_unlock(&store.mutex); break; }
        Job job = store.jobs[store.head];
        store.head = (store.head + 1u) % 8u; store.count--;
        pthread_mutex_unlock(&store.mutex);
        save_job(&job); free(job.pcm); if (job.fd>=0) close(job.fd);
    }
    return NULL;
}
static int only_clips(const struct dirent *entry) {
    const size_t len = strlen(entry->d_name);
    return len > 9u && strncmp(entry->d_name, "clip-", 5) == 0 && strcmp(entry->d_name + len - 4u, ".wav") == 0;
}
bool fv_fragment_store_start(const char *app_directory, FvFragmentEngine *engine) {
    if (store.started || !app_directory || !engine) return false;
    const int n = snprintf(store.directory, sizeof(store.directory), "%s/fieldvox-grain-cache-v2", app_directory);
    if (n < 0 || (size_t)n >= sizeof(store.directory)) return false;
    if (mkdir(store.directory, 0700) != 0 && errno != EEXIST) return false;
    fv_local_bank_enforce_cache_cap(store.directory, FV_FRAGMENT_CACHE_MAX);
    prune_originals();
    struct dirent **entries = NULL;
    const int count = scandir(store.directory, &entries, only_clips, alphasort);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            char path[800]; int station = -1;
            const char *separator=strchr(entries[i]->d_name+5,'-');
            if (separator) {
                char *end=NULL; errno=0;
                const long value=strtol(separator+1,&end,10);
                if (errno==0 && end!=separator+1 && *end=='-' && value>=-1 && value<512) station=(int)value;
            }
            const int len = snprintf(path, sizeof(path), "%s/%s", store.directory, entries[i]->d_name);
            int16_t *pcm = NULL; size_t frames = 0;
            if (len > 0 && (size_t)len < sizeof(path) && fv_wav_load_mono_48k(path, &pcm, &frames)) {
                if (frames <= FV_FRAGMENT_CLIP_FRAMES) {
                    char original[820]; snprintf(original,sizeof(original),"%s.audio",path);
                    int fd=open(original,O_RDONLY|O_CLOEXEC);
                    if (fd<0) fd=open(path,O_RDONLY|O_CLOEXEC);
                    fv_fragment_cache_add_source(engine,pcm,frames,station,fd);
                    if (fd>=0) close(fd);
                }
                free(pcm);
            }
            free(entries[i]);
        }
    }
    free(entries);
    store.running = true; store.head = store.count = 0;
    store.started = pthread_create(&store.worker, NULL, store_worker, NULL) == 0;
    if (!store.started) store.running = false;
    return store.started;
}
bool fv_fragment_store_submit(const int16_t *pcm, size_t frames, int station) {
    return fv_fragment_store_submit_source(pcm,frames,station,-1);
}
bool fv_fragment_store_submit_source(const int16_t *pcm, size_t frames, int station, int source_fd) {
    if (!pcm || !frames || frames > FV_FRAGMENT_CLIP_FRAMES) return false;
    int16_t *copy = malloc(frames * sizeof(*copy)); if (!copy) return false;
    const int fd=source_fd>=0 ? fcntl(source_fd,F_DUPFD_CLOEXEC,0) : -1;
    if (source_fd>=0 && fd<0) { free(copy); return false; }
    memcpy(copy, pcm, frames * sizeof(*copy));
    pthread_mutex_lock(&store.mutex);
    if (!store.started || !store.running || store.count == 8u) { pthread_mutex_unlock(&store.mutex); free(copy); if (fd>=0) close(fd); return false; }
    /* Ownership transfers to the queue; store_worker frees PCM after saving. */
    Job *slot = &store.jobs[(store.head + store.count) % 8u];
    slot->pcm = copy; slot->frames = frames; slot->station = station; slot->fd=fd;
    store.count++; pthread_cond_signal(&store.changed); pthread_mutex_unlock(&store.mutex);
    return true;
}
void fv_fragment_store_stop(void) {
    pthread_mutex_lock(&store.mutex);
    const bool started = store.started;
    store.running = false; pthread_cond_signal(&store.changed); pthread_mutex_unlock(&store.mutex);
    if (started) pthread_join(store.worker, NULL);
    pthread_mutex_lock(&store.mutex); store.started = false; pthread_mutex_unlock(&store.mutex);
}
