/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
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
    FvTimeline timeline = {0};
    snprintf(marks_path,sizeof(marks_path),"%s.fvmarks",path);
    assert(fv_timeline_load(path, &timeline) && timeline.count == 0);
    FILE *f = fopen(marks_path, "wb");
    assert(f);
    assert(fputs("FieldVox bookmarks 1\n500\n0\n200\n200\n", f) >= 0);
    assert(fclose(f) == 0);
    assert(fv_timeline_load(path, &timeline) && timeline.count == 3u);
    assert(timeline.markers[0].ms == 0 && timeline.markers[0].kinds == FV_MARK_LEGACY);
    assert(timeline.markers[1].ms == 200 && timeline.markers[1].kinds == FV_MARK_LEGACY);
    assert(timeline.markers[2].ms == 500 && timeline.markers[2].kinds == FV_MARK_LEGACY);
    assert(fv_timeline_step(&timeline, 200, true)->ms == 500);
    assert(fv_timeline_step(&timeline, 200, false)->ms == 0);
    assert(!fv_timeline_step(&timeline, 500, true) && !fv_timeline_step(&timeline, 0, false));
    assert(fv_timeline_add(&timeline, 200, FV_MARK_BOOKMARK) && timeline.count == 3u);
    assert(fv_timeline_add(&timeline, 300, FV_MARK_FRAGMENT | FV_MARK_HIT));
    assert(!fv_timeline_add(&timeline, -1, FV_MARK_BOOKMARK));
    assert(!fv_timeline_add(&timeline, 100, 0u));
    assert(!fv_timeline_add(&timeline, 100, 128u));
    assert(fv_timeline_save(path, &timeline));
    timeline = (FvTimeline){0};
    assert(fv_timeline_load(path, &timeline) && timeline.count == 4u);
    assert(timeline.markers[1].kinds == (FV_MARK_LEGACY | FV_MARK_BOOKMARK));
    assert(timeline.markers[2].ms == 300 && timeline.markers[2].kinds == (FV_MARK_FRAGMENT | FV_MARK_HIT));
    f = fopen(marks_path, "ab");
    assert(f);
    assert(fputs("600 32\n", f) >= 0);
    assert(fclose(f) == 0);
    assert(!fv_timeline_load(path, &timeline) && timeline.count == 0);
    unlink(marks_path);
    for (int64_t i = 0; i < FV_TIMELINE_MAX; i++) assert(fv_timeline_add(&timeline, i, FV_MARK_BOOKMARK));
    assert(!fv_timeline_add(&timeline, FV_TIMELINE_MAX, FV_MARK_BOOKMARK));
    assert(fv_playback_clamp_seek(INT64_MIN,1000)==0 && fv_playback_clamp_seek(INT64_MAX,1000)==1000);
    assert(fv_playback_clamp_seek(999,1000)==999 && fv_playback_clamp_seek(9,0)==0);
    /* Leading partial Icecast AAC bytes before a verified ADTS frame pair must be skipped. */
    uint8_t adts_stream[512];
    memset(adts_stream,0x55,sizeof(adts_stream));
    adts_stream[43]=0xFFu; adts_stream[44]=0xF1u; adts_stream[45]=0x50u;
    adts_stream[46]=0x80u; adts_stream[47]=0x17u; adts_stream[48]=0x9Fu; adts_stream[49]=0xFCu;
    /* frame_len = (0 << 11) | (0x17 << 3) | (0x9F >> 5) = 184 + 4 = 188 */
    adts_stream[43u+188u]=0xFFu; adts_stream[44u+188u]=0xF1u;
    assert(fv_adts_sync_offset(adts_stream,sizeof(adts_stream))==43u);
    assert(fv_adts_sync_offset(adts_stream+43u,sizeof(adts_stream)-43u)==0u);
    const uint8_t mp3_hdr[8]={0xFFu,0xFBu,0x92u,0x64u,0,0,0,0};
    assert(fv_adts_sync_offset(mp3_hdr,sizeof(mp3_hdr))==0u);
    for (int i=0;i<12;i++) {
        snprintf(other,sizeof(other),"%s/session-%02d.mp4",directory,i);f=fopen(other,"wb");assert(f);fputc(0,f);fclose(f);
    }
    snprintf(other,sizeof(other),"%s/symlink.wav",directory);assert(symlink(path,other)==0);
    FvRecordingCatalog catalog={0};assert(fv_recording_catalog_load(&catalog,directory,NULL,NULL,NULL));
    assert(catalog.count==13);
    for (size_t i=1;i<catalog.count;i++) assert(catalog.items[i-1].modified>=catalog.items[i].modified);
    assert(fv_recording_catalog_load(&catalog,directory,NULL,path,NULL) && catalog.count==12);
    fv_recording_catalog_free(&catalog);assert(!catalog.items && !catalog.count);
    unlink(other);for (int i=0;i<12;i++) { snprintf(other,sizeof(other),"%s/session-%02d.mp4",directory,i);unlink(other); }

    /* Keep the cause of each recorded marker, including events on the same audio block. */
    FvMarkerRecorder recorder;
    fv_marker_recorder_reset(&recorder);
    assert(!fv_marker_recorder_record(&recorder, -10, FV_MARK_FRAGMENT));
    assert(fv_marker_recorder_record(&recorder, 1200, FV_MARK_FRAGMENT));
    assert(fv_marker_recorder_record(&recorder, 1500, FV_MARK_REM_POD));
    assert(fv_marker_recorder_record(&recorder, 1500, FV_MARK_HIT | FV_MARK_REM_POD));
    assert(!fv_marker_recorder_record(&recorder, 1600, FV_MARK_REM_POD | FV_MARK_FRAGMENT));
    assert(fv_marker_recorder_record(&recorder, 1950, FV_MARK_REM_POD | FV_MARK_HIT));
    assert(recorder.timeline.count == 3u);
    assert(recorder.timeline.markers[1].ms == 1500 &&
           recorder.timeline.markers[1].kinds == (FV_MARK_REM_POD | FV_MARK_HIT));
    assert(recorder.timeline.markers[2].kinds == (FV_MARK_REM_POD | FV_MARK_HIT));
    assert(fv_timeline_save(path, &recorder.timeline));
    assert(fv_timeline_load(path, &timeline) && timeline.count == 3u);
    assert(timeline.markers[0].kinds == FV_MARK_FRAGMENT);
    assert(timeline.markers[2].kinds == (FV_MARK_REM_POD | FV_MARK_HIT));
    unlink(marks_path);

    fv_marker_recorder_reset(&recorder);
    for (size_t i = 0; i < FV_TIMELINE_MAX; i++)
        assert(fv_marker_recorder_record(&recorder, (int64_t)i * 500, FV_MARK_FRAGMENT));
    assert(fv_marker_recorder_record(&recorder, 0, FV_MARK_HIT));
    assert(fv_marker_recorder_record(&recorder, 200000, FV_MARK_HIT));
    assert(recorder.timeline.count == FV_TIMELINE_MAX);
    assert(recorder.timeline.markers[0].kinds == (FV_MARK_FRAGMENT | FV_MARK_HIT));
    assert(recorder.timeline.markers[1].ms == 1000);
    assert(recorder.timeline.markers[FV_TIMELINE_MAX - 1].ms == 200000);

    /* Compressed/AAC waveform binning & quiet-signal peak normalization */
    float norm_bins[FV_PLAYBACK_BINS] = {0};
    fv_waveform_accumulate_sample(norm_bins, 0, 1000000LL, 0.05f);
    fv_waveform_accumulate_sample(norm_bins, 1050000LL, 1000000LL, 0.10f); /* slight trailing PTS clamped */
    fv_waveform_accumulate_sample(norm_bins, 2000000LL, 1000000LL, 0.50f); /* far out of bounds ignored */
    assert(fabsf(norm_bins[0] - 0.05f) < 0.001f);
    assert(fabsf(norm_bins[FV_PLAYBACK_BINS - 1] - 0.10f) < 0.001f);
    fv_waveform_normalize(norm_bins);
    assert(fabsf(norm_bins[FV_PLAYBACK_BINS - 1] - 0.40f) < 0.01f); /* 0.10 * 4.0 bounded gain */
    assert(fabsf(norm_bins[0] - 0.20f) < 0.01f);

    unlink(path);assert(rmdir(directory)==0);
    puts("Local recording tests passed: PCM metadata/peaks, malformed files, paged catalog, bookmarks, seek bounds.");
    return 0;
}
