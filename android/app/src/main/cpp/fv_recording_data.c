/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fv_recording_data.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

void fv_put_le16(uint8_t dst[static 2], uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8u) & 0xFFu);
}
void fv_put_le32(uint8_t dst[static 4], uint32_t value) {
    for (unsigned i = 0; i < 4u; i++) dst[i] = (uint8_t)(value >> (8u * i));
}
void fv_wav_build_header(uint8_t header[static 44], uint32_t sample_rate,
                         uint16_t channels, uint32_t pcm_bytes) {
    memset(header, 0, 44);
    const uint16_t block_align = (uint16_t)(channels * 2u);
    const uint32_t byte_rate = sample_rate * (uint32_t)block_align;
    memcpy(header, "RIFF", 4);
    fv_put_le32(header + 4, pcm_bytes + 36u);
    memcpy(header + 8, "WAVEfmt ", 8);
    fv_put_le32(header + 16, 16u);
    fv_put_le16(header + 20, 1u);
    fv_put_le16(header + 22, channels);
    fv_put_le32(header + 24, sample_rate);
    fv_put_le32(header + 28, byte_rate);
    fv_put_le16(header + 32, block_align);
    fv_put_le16(header + 34, 16u);
    memcpy(header + 36, "data", 4);
    fv_put_le32(header + 40, pcm_bytes);
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
    fclose(file);return ok;
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
bool fv_bookmark_add(int64_t marks[FV_BOOKMARKS_MAX],size_t *count,int64_t ms) {
    if (!marks || !count || *count>FV_BOOKMARKS_MAX || ms<0) return false;
    size_t i=0;while (i<*count && marks[i]<ms) i++;
    if (i<*count && marks[i]==ms) return true;
    if (*count==FV_BOOKMARKS_MAX) return false;
    memmove(marks+i+1,marks+i,(*count-i)*sizeof(*marks));marks[i]=ms;(*count)++;return true;
}
int64_t fv_bookmark_next(const int64_t *marks,size_t count,int64_t ms,bool forward) {
    if (!marks || count>FV_BOOKMARKS_MAX) return -1;
    if (forward) { for (size_t i=0;i<count;i++) if (marks[i]>ms) return marks[i]; }
    else { for (size_t i=count;i>0;i--) if (marks[i-1]<ms) return marks[i-1]; }
    return -1;
}
static bool marks_path(char *out,size_t cap,const char *recording,const char *suffix) {
    if (!recording || recording[0]!='/') return false;
    const int n=snprintf(out,cap,"%s%s",recording,suffix);return n>=0 && (size_t)n<cap;
}
bool fv_bookmarks_load(const char *recording,int64_t marks[FV_BOOKMARKS_MAX],size_t *count) {
    if (!marks || !count) return false;
    *count=0;char path[544];if (!marks_path(path,sizeof(path),recording,".fvmarks")) return false;
    FILE *file=fopen(path,"rb");if (!file) return errno==ENOENT;
    char line[64];bool ok=fgets(line,sizeof(line),file)!=NULL && strcmp(line,"FieldVox bookmarks 1\n")==0;
    while (ok && fgets(line,sizeof(line),file)) {
        char *end;errno=0;const long long ms=strtoll(line,&end,10);
        if (errno || end==line || strcmp(end,"\n")!=0 || ms<0 || !fv_bookmark_add(marks,count,ms)) ok=false;
    }
    if (ferror(file)) ok=false;
    fclose(file);if (!ok) *count=0;return ok;
}
bool fv_bookmarks_save(const char *recording,const int64_t *marks,size_t count) {
    if (!marks || count>FV_BOOKMARKS_MAX) return false;
    for (size_t i=0;i<count;i++) if (marks[i]<0 || (i && marks[i]<=marks[i-1])) return false;
    char path[544],temp[560];
    if (!marks_path(path,sizeof(path),recording,".fvmarks") || !marks_path(temp,sizeof(temp),recording,".fvmarks.tmp-XXXXXX")) return false;
    const int fd=mkstemp(temp);if (fd<0) return false;
    FILE *file=fdopen(fd,"wb");
    if (!file) { close(fd);unlink(temp);return false; }
    bool ok=fputs("FieldVox bookmarks 1\n",file)>=0;
    for (size_t i=0;ok && i<count;i++) ok=fprintf(file,"%lld\n",(long long)marks[i])>0;
    if (fflush(file)!=0 || fsync(fd)!=0) ok=false;
    if (fclose(file)!=0) ok=false;
    if (ok) ok=rename(temp,path)==0;
    if (!ok) unlink(temp);
    return ok;
}
static uint8_t channel(int value) { return (uint8_t)(value<0 ? 0 : (value>255 ? 255 : value)); }
bool fv_yuv420_to_argb(uint32_t *dst,int dw,int dh,int sw,int sh,int left,int top,int right,int bottom,
                       const uint8_t *planes[3],const size_t lengths[3],const int rs[3],const int ps[3]) {
    if (!dst || !planes || !lengths || !rs || !ps || dw<=0 || dh<=0 || dw>4096 || dh>4096 ||
        sw<=0 || sh<=0 || sw>8192 || sh>8192 || left<0 || top<0 || right>sw || bottom>sh || right<=left || bottom<=top) return false;
    for (int p=0;p<3;p++) {
        if (!planes[p] || rs[p]<=0 || ps[p]<=0 || rs[p]>1048576 || ps[p]>16) return false;
        const size_t xx=(size_t)(right-1)/(p ? 2u : 1u),yy=(size_t)(bottom-1)/(p ? 2u : 1u);
        if (xx*(size_t)ps[p]>=(size_t)rs[p] || yy*(size_t)rs[p]+xx*(size_t)ps[p]>=lengths[p]) return false;
    }
    for (int y=0;y<dh;y++) {
        const int sy=top+(int)((int64_t)y*(bottom-top)/dh);
        for (int x=0;x<dw;x++) {
            const int sx=left+(int)((int64_t)x*(right-left)/dw);
            const int l=(int)planes[0][(size_t)sy*(size_t)rs[0]+(size_t)sx*(size_t)ps[0]]-16;
            const int u=(int)planes[1][(size_t)(sy/2)*(size_t)rs[1]+(size_t)(sx/2)*(size_t)ps[1]]-128;
            const int v=(int)planes[2][(size_t)(sy/2)*(size_t)rs[2]+(size_t)(sx/2)*(size_t)ps[2]]-128;
            const uint32_t r=channel((298*l+459*v+128)>>8),g=channel((298*l-55*u-136*v+128)>>8),b=channel((298*l+541*u+128)>>8);
            dst[(size_t)y*(size_t)dw+(size_t)x]=0xff000000u | (r<<16u) | (g<<8u) | b;
        }
    }
    return true;
}
