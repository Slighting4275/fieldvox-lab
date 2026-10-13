#define _POSIX_C_SOURCE 200809L
#include "../../android/app/src/main/cpp/fv_fragment_engine.h"
#include "../../android/app/src/main/cpp/fv_fragment_store.h"
#include "../../android/app/src/main/cpp/fv_vox_controls.h"
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../android/app/src/main/cpp/fv_station_policy.h"
#include "../../android/app/src/main/cpp/fv_environment_reverb.h"

static constexpr double pi = 3.14159265358979323846;
static int16_t source[48000], output[96000], clean[96000];
static FvVoxDecimalTrigger normal(void) {
    return (FvVoxDecimalTrigger){.passed=true, .pitch_factor=1.0f, .speed_factor=1.0f,
        .grain_duration_ms=400, .seek_offset=0.0f, .prefer_live=true};
}
static double frequency(const int16_t *pcm, size_t frames) {
    size_t crosses = 0, first = 0, last = 0;
    for (size_t i = 2049; i + 2048 < frames; i++) if (pcm[i-1] <= 0 && pcm[i] > 0) {
        if (!crosses) first = i;
        last = i; crosses++;
    }
    assert(crosses > 2 && last > first);
    return (double)(crosses-1u) * 48000.0 / (double)(last-first);
}
static void test_pitch_and_tempo(void) {
    for (size_t i = 0; i < 19200; i++) source[i] = (int16_t)(12000.0*sin(2.0*pi*440.0*(double)i/48000.0));
    const float pitches[] = {0.6f, 1.0f, 1.2f};
    const float speeds[] = {0.7f, 1.0f, 1.2f};
    for (unsigned p=0; p<3; p++) for (unsigned s=0; s<3; s++) {
        const size_t frames = fv_fragment_transform(source,19200,pitches[p],speeds[s],output,96000);
        assert(frames == (size_t)llround(19200.0/(double)speeds[s]));
        const double measured = frequency(output,frames), wanted = 440.0*(double)pitches[p];
        printf("pitch=%.2f speed=%.2f frequency=%.2fHz frames=%zu\n",(double)pitches[p],(double)speeds[s],measured,frames);
        assert(fabs(measured-wanted) < wanted*0.04);
    }
    assert(fv_fragment_transform(source,19200,NAN,1,output,96000)==0);
    assert(fv_fragment_transform(source,19200,1,0,output,96000)==0);
    assert(fv_fragment_transform(source,19200,1,1,output,100)==0);
}
static void test_scheduling(void) {
    FvFragmentEngine *e=fv_fragment_create(); assert(e);
    fv_fragment_set_settings(e,(FvFragmentSettings){.echo=0.75f,.sweep_ms=200});
    memset(source,0,sizeof(source)); source[0]=10000;
    FvVoxDecimalTrigger t=normal();
    assert(!fv_fragment_trigger_pcm(e,&t,source,19200,1,0,true));
    assert(fv_fragment_trigger_pcm(e,&t,source,19200,1,0,false));
    assert(!fv_fragment_trigger_pcm(e,&t,source,19200,2,48000,false));
    FvFragmentStatus status;
    fv_fragment_render(e,clean,output,60000,0,&status);
    assert(output[0]==10000 && output[7200]==600 && output[14400]==180 && output[21600]==54 && output[28800]==16);
    assert(output[36000]==0 && clean[7200]==0 && clean[28800]==0);
    assert(!fv_fragment_trigger_pcm(e,&t,source,19200,1,4u*48000u,false));
    assert(fv_fragment_trigger_pcm(e,&t,source,19200,1,5u*48000u,false));
    fv_fragment_destroy(e);

    e=fv_fragment_create(); assert(e);
    fv_fragment_set_settings(e,(FvFragmentSettings){.reverb=1.0f,.sweep_ms=200});
    assert(fv_fragment_trigger_pcm(e,&t,source,19200,1,0,false));
    assert(!fv_fragment_trigger_pcm(e,&t,source,19200,2,9u*48000u,false));
    assert(fv_fragment_trigger_pcm(e,&t,source,19200,2,10u*48000u,false));
    fv_fragment_destroy(e);
}
static void test_bank_and_live(void) {
    FvFragmentEngine *e=fv_fragment_create(); assert(e);
    fv_fragment_set_settings(e,(FvFragmentSettings){.sweep_ms=200});
    FvVoxDecimalTrigger t=normal();
    for (size_t i=0;i<19200;i++) source[i]=10000;
    assert(fv_fragment_cache_add(e,source,19200,7));
    for (unsigned i=0;i<5;i++) assert(fv_fragment_trigger(e,&t,true,(uint64_t)i*5u*48000u,false));
    assert(!fv_fragment_trigger(e,&t,true,25u*48000u,false));
    for (unsigned i=0;i<105;i++) assert(fv_fragment_cache_add(e,source,19200,(int)i));
    assert(fv_fragment_cache_count(e)==100u);
    fv_fragment_destroy(e);
    e=fv_fragment_create(); assert(e);
    fv_fragment_set_settings(e,(FvFragmentSettings){.sweep_ms=200});
    assert(!fv_fragment_feed(e,3,42,source,19200));
    t.channel_hop=3;
    assert(fv_fragment_trigger(e,&t,false,0,false));
    FvFragmentStatus status;
    fv_fragment_render(e,clean,output,1024,0,&status);
    assert(status.station==42 && !status.cached && status.admitted==1 && output[100]==10000);
    fv_fragment_reset_playback(e);
    fv_fragment_render(e,clean,output,1024,1024,&status);
    for (size_t i=0;i<1024;i++) assert(output[i]==0);
    fv_fragment_destroy(e);
}
static void test_store(void) {
    char dir[512]; const char *temp=getenv("TMPDIR");
    snprintf(dir,sizeof(dir),"%s/fieldvox-fragments-XXXXXX",temp ? temp : "/tmp"); assert(mkdtemp(dir));
    FvFragmentEngine *e=fv_fragment_create(); assert(e);
    assert(fv_fragment_store_start(dir,e));
    assert(fv_fragment_store_submit(source,19200,42));
    fv_fragment_store_stop(); fv_fragment_destroy(e);
    e=fv_fragment_create(); assert(e);
    assert(fv_fragment_store_start(dir,e));
    assert(fv_fragment_cache_count(e)==1u);
    fv_fragment_store_stop(); fv_fragment_destroy(e);
    char cache[544]; snprintf(cache,sizeof(cache),"%s/fieldvox-grain-cache",dir);
    DIR *d=opendir(cache); assert(d); struct dirent *entry;
    while ((entry=readdir(d))) if (entry->d_name[0]!='.') {
        char path[832]; snprintf(path,sizeof(path),"%s/%s",cache,entry->d_name); assert(unlink(path)==0);
    }
    closedir(d); assert(rmdir(cache)==0); assert(rmdir(dir)==0);
}
static void test_pause_silences_static_and_tails(void) {
    FvFragmentEngine *e=fv_fragment_create(); assert(e);
    FvFragmentSettings settings={.echo=0.75f,.reverb=0.5f,.noise=true,.sweep_ms=200};
    fv_fragment_set_settings(e,settings);
    for (size_t i=0;i<19200;i++) source[i]=10000;
    assert(fv_fragment_cache_add(e,source,19200,7));
    FvVoxDecimalTrigger t=normal();
    assert(fv_fragment_trigger(e,&t,true,0,false));
    FvFragmentStatus status;
    fv_fragment_render(e,clean,output,1024,0,&status);
    assert(status.playing && status.voices==1 && output[100]!=0);
    settings.paused=true; fv_fragment_set_settings(e,settings);
    assert(!fv_fragment_trigger(e,&t,true,10u*48000u,false));
    fv_fragment_render(e,clean,output,48000,1024,&status);
    for (size_t i=0;i<48000;i++) assert(clean[i]==0 && output[i]==0);
    assert(!status.playing && status.voices==0 && status.cache_count==1);
    settings.paused=false; fv_fragment_set_settings(e,settings);
    fv_fragment_render(e,clean,output,1024,10u*48000u,&status);
    bool static_audible=false;
    for (size_t i=0;i<1024;i++) { assert(clean[i]==0); static_audible|=output[i]!=0; }
    assert(static_audible && !status.playing && status.voices==0);
    assert(fv_fragment_trigger(e,&t,true,10u*48000u,false));
    fv_fragment_destroy(e);
}
static void test_speech_band(void) {
    int16_t mic[1024]; FvVoxDucker state={0};
    for (unsigned i=0;i<1024;i++) mic[i]=(int16_t)(16000.0*sin(2.0*pi*1000.0*(double)i/48000.0));
    assert(!fv_vox_detect_voice(&state,mic,1024,0,48000));
    for (unsigned i=0;i<1024;i++) mic[i]=(int16_t)(16000.0*sin(2.0*pi*150.0*(double)i/48000.0));
    assert(fv_vox_detect_voice(&state,mic,1024,1024,48000));
    assert(fv_vox_detect_voice(&state,NULL,0,97023,48000));
    assert(!fv_vox_detect_voice(&state,NULL,0,97024,48000));
}
static void test_request_selection_and_timing(void) {
    FvFragmentEngine *e=fv_fragment_create(); assert(e);
    fv_fragment_set_settings(e,(FvFragmentSettings){.echo=0.75f,.reverb=0.0f,.sweep_ms=200});
    memset(source,0,sizeof(source)); source[0]=10000;
    FvVoxDecimalTrigger t=normal();
    assert(fv_fragment_cache_add(e,source,19200,7));
    assert(fv_fragment_cache_add(e,source,19200,8));
    /* An unavailable selected live deck must not walk to another deck or cache. */
    assert(!fv_fragment_feed(e,3,42,source,19200));
    t.channel_hop=2;
    assert(!fv_fragment_trigger(e,&t,false,0,false));
    assert(fv_fragment_begin_live(e,&t,42,0,false));
    assert(fv_fragment_live_pending(e));
    assert(!fv_fragment_trigger(e,&t,true,48000,false));
    assert(fv_fragment_finish_live(e,source,19200,48000,false));
    assert(!fv_fragment_live_pending(e));
    /* Cooldown is measured from request time, not completion time. */
    assert(fv_fragment_begin_live(e,&t,43,2u*48000u,false));
    assert(!fv_fragment_finish_live(e,NULL,0,2u*48000u,false));
    assert(!fv_fragment_begin_live(e,&t,44,3u*48000u,false));
    assert(fv_fragment_begin_live(e,&t,44,4u*48000u,false));
    fv_fragment_finish_live(e,NULL,0,4u*48000u,true);
    fv_fragment_reset_playback(e);
    t.channel_hop=0;
    for(unsigned i=0;i<5;i++) assert(fv_fragment_trigger(e,&t,true,(uint64_t)(10u+i*2u)*48000u,false));
    /* The selected exhausted cache slot does not walk to the unused slot. */
    assert(!fv_fragment_trigger(e,&t,true,20u*48000u,false));
    t.channel_hop=1; assert(fv_fragment_trigger(e,&t,true,20u*48000u,false));
    fv_fragment_destroy(e);

    e=fv_fragment_create(); assert(e);t=normal();
    fv_fragment_set_settings(e,(FvFragmentSettings){.echo=0.75f,.sweep_ms=200});
    assert(fv_fragment_trigger_pcm(e,&t,source,19200,1,0,false));
    FvFragmentStatus status;
    fv_fragment_render(e,clean,output,7200,0,&status); assert(status.voices==1);
    fv_fragment_render(e,clean,output,1,7200,&status); assert(status.voices==2 && output[0]==600);
    fv_fragment_reset_playback(e);
    fv_fragment_render(e,clean,output,30000,7201,&status);
    for(size_t i=0;i<30000;i++) assert(output[i]==0);
    fv_fragment_destroy(e);
}
static void test_language_group_and_reverb(void) {
    char languages[24][4]; for(unsigned i=0;i<24;i++) memcpy(languages[i],i<12 ? "en\0" : "es\0",4);
    unsigned a[10],b[10];
    assert(fv_station_group(languages,24,"en",123,a)==10u);
    assert(fv_station_group(languages,24,"en",456,b)==10u);
    assert(memcmp(a,b,sizeof(a))!=0);
    for(unsigned i=0;i<10;i++) { assert(a[i]<12u); for(unsigned j=0;j<i;j++) assert(a[i]!=a[j]); }
    FvEnvironmentReverb *r=fv_environment_reverb_create(); assert(r);
    float send[64]={0},wet[64]; send[0]=16000;
    fv_environment_reverb_set_mix(r,1.0f);
    bool tail=false;
    for(unsigned block=0;block<200;block++) {
        fv_environment_reverb_process(r,send,wet,64);
        for(unsigned i=0;i<64;i++) { assert(isfinite(wet[i])); if(block<37u) assert(wet[i]==0.0f);tail|=fabsf(wet[i])>0.01f; }
        memset(send,0,sizeof(send));
    }
    assert(tail);
    fv_environment_reverb_reset(r);fv_environment_reverb_set_mix(r,0.0f);
    fv_environment_reverb_process(r,send,wet,64);for(unsigned i=0;i<64;i++) assert(wet[i]==0.0f);
    fv_environment_reverb_destroy(r);
}
int main(void) {
    test_pitch_and_tempo(); test_scheduling(); test_bank_and_live(); test_store(); test_speech_band();
    test_pause_silences_static_and_tails();
    test_request_selection_and_timing(); test_language_group_and_reverb();
    puts("Fragment engine: pitch/tempo, four echoes, station/global cooldown, speech suppression, cache limits/reuse, live feed, persistent offline reload and stopped-playback silence passed.");
    return 0;
}
