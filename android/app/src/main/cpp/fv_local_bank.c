#define _POSIX_C_SOURCE 200809L
#include "fv_local_bank.h"
#include "fv_recording_data.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    bool started, running, requested;
    uint64_t request_id;
    char requested_path[512], choice_path[544], cache_dir[512];
    int16_t *samples;
    size_t frames, cursor[FV_LOCAL_DECKS];
    FvLocalBankInfo info;
} bank={.mutex=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};

static bool save_choice(const char *path) {
    if (!bank.choice_path[0]) return false;
    char temp[560];const int n=snprintf(temp,sizeof(temp),"%s.tmp-XXXXXX",bank.choice_path);
    if (n<0 || (size_t)n>=sizeof(temp)) return false;
    int fd=mkstemp(temp);if (fd<0) return false;
    FILE *f=fdopen(fd,"wb");if (!f) { close(fd);unlink(temp);return false; }
    bool ok=fprintf(f,"%s\n",path)>0;
    if (fflush(f)!=0 || fsync(fd)!=0) ok=false;
    if (fclose(f)!=0) ok=false;
    if (ok) ok=rename(temp,bank.choice_path)==0;
    if (!ok) unlink(temp);
    return ok;
}
static void *bank_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&bank.mutex);
        while (bank.running && !bank.requested) pthread_cond_wait(&bank.changed,&bank.mutex);
        if (!bank.running) { pthread_mutex_unlock(&bank.mutex);break; }
        char path[512];snprintf(path,sizeof(path),"%s",bank.requested_path);
        const uint64_t request_id=bank.request_id;bank.requested=false;
        pthread_mutex_unlock(&bank.mutex);
        int16_t *samples=NULL;size_t frames=0;
        const bool ok=fv_wav_load_mono_48k(path,&samples,&frames);
        pthread_mutex_lock(&bank.mutex);
        if (!bank.running || request_id!=bank.request_id) { pthread_mutex_unlock(&bank.mutex);free(samples);continue; }
        bank.info.loading=false;bank.info.failed=!ok;
        if (ok) {
            free(bank.samples);bank.samples=samples;bank.frames=frames;
            bank.info.ready=true;bank.info.frames=frames;bank.info.generation++;
            for (int i=0;i<FV_LOCAL_DECKS;i++) bank.cursor[i]=(frames/FV_LOCAL_DECKS)*(size_t)i;
            snprintf(bank.info.path,sizeof(bank.info.path),"%s",path);
            const char *name=strrchr(path,'/');snprintf(bank.info.name,sizeof(bank.info.name),"%.255s",name ? name+1 : path);
            snprintf(bank.info.status,sizeof(bank.info.status),"Local WAV loaded • no internet required");
        } else snprintf(bank.info.status,sizeof(bank.info.status),"Choose a complete mono 48 kHz PCM16 WAV, up to 10 minutes");
        pthread_mutex_unlock(&bank.mutex);
        if (ok && !save_choice(path)) {
            pthread_mutex_lock(&bank.mutex);
            if (request_id==bank.request_id) snprintf(bank.info.status,sizeof(bank.info.status),"Local WAV loaded • choice could not be saved");
            pthread_mutex_unlock(&bank.mutex);
        }
    }
    return NULL;
}
bool fv_local_bank_init(const char *directory) {
    if (bank.started) return true;
    if (!directory || directory[0]!='/') return false;
    snprintf(bank.cache_dir,sizeof(bank.cache_dir),"%s",directory);
    int n=snprintf(bank.choice_path,sizeof(bank.choice_path),"%s/fieldvox-local-source",directory);
    if (n<0 || (size_t)n>=sizeof(bank.choice_path)) return false;
    pthread_mutex_lock(&bank.mutex);bank.running=true;bank.info=(FvLocalBankInfo){0};
    snprintf(bank.info.status,sizeof(bank.info.status),"Choose a WAV source in LIBRARY");pthread_mutex_unlock(&bank.mutex);
    bank.started=pthread_create(&bank.thread,NULL,bank_worker,NULL)==0;
    if (!bank.started) return false;
    FILE *f=fopen(bank.choice_path,"rb");
    if (f) {
        char path[512];
        if (fgets(path,sizeof(path),f)) {
            const size_t len=strlen(path);
            if (len>1 && path[len-1]=='\n') { path[len-1]=0;fv_local_bank_load(path); }
        }
        fclose(f);
    }
    return true;
}
void fv_local_bank_shutdown(void) {
    pthread_mutex_lock(&bank.mutex);bank.running=false;bank.requested=false;pthread_cond_signal(&bank.changed);pthread_mutex_unlock(&bank.mutex);
    if (bank.started) { pthread_join(bank.thread,NULL);bank.started=false; }
    pthread_mutex_lock(&bank.mutex);free(bank.samples);bank.samples=NULL;bank.frames=0;bank.info=(FvLocalBankInfo){0};pthread_mutex_unlock(&bank.mutex);
}
void fv_local_bank_load(const char *path) {
    if (!path || path[0] != '/' || strlen(path) >= sizeof(bank.requested_path) || strchr(path, '\n') || strstr(path, "..")) return;
    pthread_mutex_lock(&bank.mutex);
    if (bank.started) {
        snprintf(bank.requested_path,sizeof(bank.requested_path),"%s",path);
        bank.request_id++;bank.requested=true;bank.info.loading=true;bank.info.failed=false;
        snprintf(bank.info.status,sizeof(bank.info.status),"Loading local WAV…");pthread_cond_signal(&bank.changed);
    }
    pthread_mutex_unlock(&bank.mutex);
}
void fv_local_bank_info(FvLocalBankInfo *info) { if (info) { pthread_mutex_lock(&bank.mutex);*info=bank.info;pthread_mutex_unlock(&bank.mutex); } }
int fv_local_bank_pull(int deck,int16_t *out,int frames,FvLocalBankInfo *info) {
    if (!out || frames<=0 || deck<0 || deck>=FV_LOCAL_DECKS) return 0;
    pthread_mutex_lock(&bank.mutex);
    if (info) { *info=bank.info;info->source_offset=bank.cursor[deck]; }
    const bool ready=bank.samples && bank.frames;
    if (ready) {
        for (int i=0;i<frames;i++) {
            out[i]=bank.samples[bank.cursor[deck]++];
            if (bank.cursor[deck]>=bank.frames) bank.cursor[deck]=0;
        }
    } else memset(out,0,(size_t)frames*sizeof(*out));
    pthread_mutex_unlock(&bank.mutex);
    return ready ? frames : 0;
}
size_t fv_local_bank_fragment(float seek, size_t frames, int16_t *output, size_t capacity) {
    if (!output || !frames || frames > capacity || !isfinite(seek)) return 0;
    pthread_mutex_lock(&bank.mutex);
    if (!bank.samples || !bank.frames) { pthread_mutex_unlock(&bank.mutex); return 0; }
    if (frames > bank.frames) frames = bank.frames;
    size_t start = (size_t)((double)fmaxf(0.0f, fminf(1.0f, seek)) * (double)bank.frames);
    if (start > bank.frames - frames) start = bank.frames - frames;
    memcpy(output, bank.samples + start, frames * sizeof(*output));
    pthread_mutex_unlock(&bank.mutex);
    return frames;
}

typedef struct {
    char path[512];
    time_t mtime;
    long mtime_nsec;
} FvBankFileEntry;

static int cmp_bank_file_entry(const void *a, const void *b) {
    const FvBankFileEntry *fa = (const FvBankFileEntry *)a;
    const FvBankFileEntry *fb = (const FvBankFileEntry *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    if (fa->mtime_nsec < fb->mtime_nsec) return -1;
    if (fa->mtime_nsec > fb->mtime_nsec) return 1;
    return strcmp(fa->path, fb->path);
}

int fv_local_bank_enforce_cache_cap(const char *directory, size_t max_files) {
    if (!directory || !max_files) return 0;
    DIR *d = opendir(directory);
    if (!d) return 0;

    size_t cap = 128;
    size_t count = 0;
    FvBankFileEntry *entries = malloc(cap * sizeof(FvBankFileEntry));
    if (!entries) {
        closedir(d);
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        const char *ext = strrchr(de->d_name, '.');
        if (!ext || (strcmp(ext, ".wav") != 0 && strcmp(ext, ".pcm") != 0)) continue;

        char full_path[512];
        const int n = snprintf(full_path, sizeof(full_path), "%s/%s", directory, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full_path)) continue;

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (count >= cap) {
                cap *= 2;
                FvBankFileEntry *new_entries = realloc(entries, cap * sizeof(FvBankFileEntry));
                if (!new_entries) break;
                entries = new_entries;
            }
            snprintf(entries[count].path, sizeof(entries[count].path), "%s", full_path);
            entries[count].mtime = st.st_mtime;
            entries[count].mtime_nsec = st.st_mtim.tv_nsec;
            count++;
        }
    }
    closedir(d);

    int deleted = 0;
    if (count > max_files) {
        qsort(entries, count, sizeof(FvBankFileEntry), cmp_bank_file_entry);
        const size_t to_delete = count - max_files;
        for (size_t i = 0; i < to_delete; i++) {
            if (unlink(entries[i].path) == 0) {
                deleted++;
            }
        }
    }
    free(entries);
    return deleted;
}

size_t fv_local_bank_count(void) {
    char dir[512] = {0};
    pthread_mutex_lock(&bank.mutex);
    if (!bank.started || !bank.cache_dir[0]) {
        pthread_mutex_unlock(&bank.mutex);
        return 0;
    }
    snprintf(dir, sizeof(dir), "%s", bank.cache_dir);
    pthread_mutex_unlock(&bank.mutex);

    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        const char *ext = strrchr(de->d_name, '.');
        if (ext && (strcmp(ext, ".wav") == 0 || strcmp(ext, ".pcm") == 0)) {
            count++;
        }
    }
    closedir(d);
    return count;
}
