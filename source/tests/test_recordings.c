#define _POSIX_C_SOURCE 200809L
#include "../../android/app/src/main/cpp/fv_recording_data.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void u16(FILE *f,uint16_t v) { fputc(v&255,f);fputc(v>>8,f); }
static void u32(FILE *f,uint32_t v) { u16(f,(uint16_t)v);u16(f,(uint16_t)(v>>16)); }
static void wav(const char *path,uint16_t channels) {
    FILE *f=fopen(path,"wb");assert(f);
    fwrite("RIFF",1,4,f);u32(f,48+1024u*channels*2u);fwrite("WAVEfmt ",1,8,f);
    u32(f,16);u16(f,1);u16(f,channels);u32(f,48000);u32(f,48000u*channels*2u);u16(f,(uint16_t)(channels*2u));u16(f,16);
    fwrite("JUNK",1,4,f);u32(f,3);fwrite("abc\0",1,4,f);
    fwrite("data",1,4,f);u32(f,1024u*channels*2u);
    for (int i=0;i<1024;i++) for (uint16_t ch=0;ch<channels;ch++) u16(f,(uint16_t)(i<16 || ch ? 32768u : (i>=512 ? 32767u : 0u)));
    int rc = fclose(f);
    assert(rc == 0);
}
int main(void) {
    char directory[]="/tmp/fieldvox-recording-test-XXXXXX";assert(mkdtemp(directory));
    char path[512],other[512],marks_path[544];
    snprintf(path,sizeof(path),"%s/session.wav",directory);wav(path,1);
    FvWavInfo info={0};assert(fv_wav_read_info(path,&info));
    assert(info.sample_rate==48000 && info.channels==1 && info.frames==1024 && info.data_offset==56);
    float peaks[FV_PLAYBACK_BINS];atomic_bool cancel=false;
    assert(fv_wav_waveform(path,peaks,&cancel));
    assert(peaks[0]==1.0f && peaks[3]==1.0f && peaks[4]==0.0f && peaks[128]==32767.0f/32768.0f);
    atomic_store(&cancel,true);assert(!fv_wav_waveform(path,peaks,&cancel));
    int16_t *pcm=NULL;size_t frames=0;assert(fv_wav_load_mono_48k(path,&pcm,&frames));
    assert(frames==1024 && pcm[0]==-32768 && pcm[511]==0 && pcm[512]==32767);free(pcm);
    snprintf(other,sizeof(other),"%s/stereo.wav",directory);wav(other,2);
    assert(fv_wav_read_info(other,&info) && info.channels==2);
    assert(fv_wav_waveform(other,peaks,NULL) && peaks[64]==1.0f);
    assert(!fv_wav_load_mono_48k(other,&pcm,&frames));
    assert(truncate(other,55)==0);assert(!fv_wav_read_info(other,&info));
    assert(!fv_wav_waveform(other,peaks,NULL));unlink(other);
    int64_t marks[FV_BOOKMARKS_MAX]={0};size_t count=0;
    assert(fv_bookmark_add(marks,&count,500));assert(fv_bookmark_add(marks,&count,0));assert(fv_bookmark_add(marks,&count,200));
    assert(fv_bookmark_add(marks,&count,200) && count==3);
    assert(!fv_bookmark_add(marks,&count,-1));
    assert(marks[0]==0 && marks[1]==200 && marks[2]==500);
    assert(fv_bookmark_next(marks,count,200,true)==500 && fv_bookmark_next(marks,count,200,false)==0);
    assert(fv_bookmark_next(marks,count,500,true)==-1 && fv_bookmark_next(marks,count,0,false)==-1);
    assert(fv_bookmarks_save(path,marks,count));memset(marks,0,sizeof(marks));count=0;
    assert(fv_bookmarks_load(path,marks,&count) && count==3 && marks[2]==500);
    assert(fv_bookmarks_save(path,marks,count));
    snprintf(marks_path,sizeof(marks_path),"%s.fvmarks",path);
    FILE *f=fopen(marks_path,"ab");assert(f);fputs("invalid\n",f);fclose(f);
    assert(!fv_bookmarks_load(path,marks,&count) && count==0);unlink(marks_path);
    for (int64_t i=0;i<FV_BOOKMARKS_MAX;i++) assert(fv_bookmark_add(marks,&count,i));
    assert(!fv_bookmark_add(marks,&count,FV_BOOKMARKS_MAX));
    assert(fv_playback_clamp_seek(INT64_MIN,1000)==0 && fv_playback_clamp_seek(INT64_MAX,1000)==1000);
    assert(fv_playback_clamp_seek(999,1000)==999 && fv_playback_clamp_seek(9,0)==0);
    for (int i=0;i<12;i++) {
        snprintf(other,sizeof(other),"%s/session-%02d.mp4",directory,i);f=fopen(other,"wb");assert(f);fputc(0,f);fclose(f);
    }
    snprintf(other,sizeof(other),"%s/symlink.wav",directory);assert(symlink(path,other)==0);
    FvRecordingCatalog catalog={0};assert(fv_recording_catalog_load(&catalog,directory,NULL,NULL,NULL));
    assert(catalog.count==13);
    for (size_t i=1;i<catalog.count;i++) assert(catalog.items[i-1].modified>=catalog.items[i].modified);
    assert(fv_recording_catalog_load(&catalog,directory,NULL,path,NULL) && catalog.count==12);
    fv_recording_catalog_free(&catalog);assert(!catalog.items && !catalog.count);
    uint8_t y[]={235,235,235,235},uv[]={128,128};
    const uint8_t *planes[]={y,uv,uv+1};size_t lengths[]={4,2,1};int row[]={2,2,2},pixel[]={1,2,2};
    uint32_t image[5]={0,0,0,0,0x12345678};
    assert(fv_yuv420_to_argb(image,2,2,2,2,0,0,2,2,planes,lengths,row,pixel));
    for (int i=0;i<4;i++) assert(image[i]==0xffffffffu);
    assert(image[4]==0x12345678);
    memset(y,16,sizeof(y));assert(fv_yuv420_to_argb(image,2,2,2,2,0,0,2,2,planes,lengths,row,pixel));assert(image[0]==0xff000000u);
    memset(y,63,sizeof(y));uv[0]=102;uv[1]=240;
    assert(fv_yuv420_to_argb(image,2,2,2,2,0,0,2,2,planes,lengths,row,pixel));assert((image[0]>>16&255u)>=254u && (image[0]>>8&255u)<=1u && (image[0]&255u)<=1u);
    lengths[2]=0;assert(!fv_yuv420_to_argb(image,2,2,2,2,0,0,2,2,planes,lengths,row,pixel));
    lengths[2]=1;row[0]=0;assert(!fv_yuv420_to_argb(image,2,2,2,2,0,0,2,2,planes,lengths,row,pixel));
    row[0]=2;assert(!fv_yuv420_to_argb(image,2,2,2,2,-1,0,2,2,planes,lengths,row,pixel));
    unlink(other);for (int i=0;i<12;i++) { snprintf(other,sizeof(other),"%s/session-%02d.mp4",directory,i);unlink(other); }
    unlink(path);assert(rmdir(directory)==0);
    puts("Local recording tests passed: PCM metadata/peaks, malformed files, paged catalog, bookmarks, seek bounds, padded/interleaved YUV and color.");
    return 0;
}
