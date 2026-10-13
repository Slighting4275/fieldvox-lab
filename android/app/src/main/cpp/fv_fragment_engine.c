#define _POSIX_C_SOURCE 200809L
#include "fv_fragment_engine.h"
#include "fv_environment_reverb.h"
#include "third_party/sonic/sonic.h"
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif
#define MAX_GRAIN 48000u
#define MAX_RENDER (MAX_GRAIN * 2u)
#define FEED_FRAMES (FV_FRAGMENT_RATE * 4u)
#define ECHO_EVENTS (FV_FRAGMENT_VOICES * 4u)

typedef struct { int16_t *pcm; size_t frames; int station, fd; unsigned uses; } Clip;
typedef struct { int16_t *pcm; size_t count; int station; } Deck;
typedef struct { size_t frames; unsigned references; int16_t pcm[]; } Audio;
typedef struct {
    Audio *audio;
    uint64_t start, release;
    float volume, send;
    int station;
    bool cached, primary;
} Voice;
typedef struct { Audio *audio; uint64_t due; float volume; int station; bool cached; } Echo;
struct FvFragmentEngine {
    Deck decks[FV_FRAGMENT_DECKS];
    Clip cache[FV_FRAGMENT_CACHE_MAX];
    size_t cache_count;
    Voice voices[FV_FRAGMENT_VOICES];
    Echo echoes[ECHO_EVENTS];
    FvFragmentSettings settings;
    uint64_t last_trigger, station_trigger[512];
    bool triggered, station_seen[512], live_pending;
    FvVoxDecimalTrigger live_trigger;
    int live_station;
    unsigned admitted;
    int last_station;
    bool last_cached;
    uint32_t rng;
    float pitch, speed;
    int grain_ms;
    float seek_fraction;
    FvEnvironmentReverb *reverb;
    FvFragmentSpeaker speaker;
    void *speaker_context;
};
static int16_t saturate(float value) {
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return (int16_t)lrintf(value);
}
static float unit(float value) { return isfinite(value) ? fmaxf(0.0f, fminf(1.0f, value)) : 0.0f; }
static void release_audio(Audio **audio) {
    if (*audio && --(*audio)->references == 0u) free(*audio);
    *audio = NULL;
}
/* AudioFlinger's Sonic tempo conversion followed by pitch-rate conversion.
 * Sinc resampling in Sonic avoids the former linear interpolation and fixed
 * waveform-search window. Flush handles short fragments and delayed samples. */
size_t fv_fragment_transform(const int16_t *source, size_t frames, float pitch, float speed,
                             int16_t *output, size_t capacity) {
    if (!source || !output || !frames || frames > MAX_GRAIN || !isfinite(pitch) || !isfinite(speed) ||
        pitch < 0.5f || pitch > 2.0f || speed < 0.5f || speed > 2.0f) return 0;
    const size_t wanted = (size_t)llround((double)frames/(double)speed);
    if (!wanted || wanted > capacity || wanted > MAX_RENDER) return 0;
    if (pitch == 1.0f && speed == 1.0f) { memcpy(output, source, frames*sizeof(*output)); return frames; }
    sonicStream stream = sonicCreateStream((int)FV_FRAGMENT_RATE, 1);
    if (!stream) return 0;
    sonicSetSpeed(stream, speed/pitch); sonicSetRate(stream, pitch); sonicSetQuality(stream, 1);
    const bool ok = sonicWriteShortToStream(stream, source, (int)frames) && sonicFlushStream(stream);
    const int read = ok ? sonicReadShortFromStream(stream, output, (int)wanted) : 0;
    sonicDestroyStream(stream);
    if (read <= 0) return 0;
    if ((size_t)read < wanted) memset(output+(size_t)read, 0, (wanted-(size_t)read)*sizeof(*output));
    return wanted;
}
FvFragmentEngine *fv_fragment_create(void) {
    FvFragmentEngine *e=calloc(1,sizeof(*e));
    if (!e) return NULL;
    e->reverb=fv_environment_reverb_create();
    if (!e->reverb) { free(e); return NULL; }
    e->rng=0x91873645u; e->last_station=-1;
    for (unsigned i=0;i<FV_FRAGMENT_DECKS;i++) e->decks[i].station=-1;
    return e;
}
void fv_fragment_reset_playback(FvFragmentEngine *e) {
    if (!e) return;
    for (unsigned i=0;i<FV_FRAGMENT_VOICES;i++) release_audio(&e->voices[i].audio);
    for (unsigned i=0;i<ECHO_EVENTS;i++) release_audio(&e->echoes[i].audio);
    for (unsigned i=0;i<FV_FRAGMENT_DECKS;i++) e->decks[i].count=0;
    fv_environment_reverb_reset(e->reverb);
    e->triggered=false; e->live_pending=false;
}
void fv_fragment_destroy(FvFragmentEngine *e) {
    if (!e) return;
    fv_fragment_reset_playback(e);
    for (size_t i=0;i<e->cache_count;i++) { free(e->cache[i].pcm); if (e->cache[i].fd>=0) close(e->cache[i].fd); }
    for (unsigned i=0;i<FV_FRAGMENT_DECKS;i++) free(e->decks[i].pcm);
    fv_environment_reverb_destroy(e->reverb); free(e);
}
void fv_fragment_set_settings(FvFragmentEngine *e,FvFragmentSettings settings) {
    if (!e) return;
    settings.echo=unit(settings.echo); settings.reverb=unit(settings.reverb);
    if (settings.sweep_ms<100u) settings.sweep_ms=100u;
    if (settings.sweep_ms>1000u) settings.sweep_ms=1000u;
    if (settings.paused && !e->settings.paused) fv_fragment_reset_playback(e);
    if (settings.reverb != e->settings.reverb) fv_environment_reverb_set_mix(e->reverb,settings.reverb);
    e->settings=settings;
}
void fv_fragment_set_speaker(FvFragmentEngine *e,FvFragmentSpeaker speaker,void *context) {
    if (e) { e->speaker=speaker; e->speaker_context=context; }
}
bool fv_fragment_cache_add(FvFragmentEngine *e,const int16_t *pcm,size_t frames,int station) {
    return fv_fragment_cache_add_source(e,pcm,frames,station,-1);
}
bool fv_fragment_cache_add_source(FvFragmentEngine *e,const int16_t *pcm,size_t frames,int station,int source_fd) {
    if (!e || !pcm || !frames || frames>FV_FRAGMENT_CLIP_FRAMES) return false;
    int16_t *copy=malloc(frames*sizeof(*copy)); if (!copy) return false;
    const int fd=source_fd>=0 ? fcntl(source_fd,F_DUPFD_CLOEXEC,0) : -1;
    if (source_fd>=0 && fd<0) { free(copy); return false; }
    memcpy(copy,pcm,frames*sizeof(*copy));
    if (e->cache_count==FV_FRAGMENT_CACHE_MAX) {
        free(e->cache[0].pcm); if (e->cache[0].fd>=0) close(e->cache[0].fd);
        memmove(e->cache,e->cache+1,(FV_FRAGMENT_CACHE_MAX-1u)*sizeof(e->cache[0])); e->cache_count--;
    }
    e->cache[e->cache_count++]=(Clip){.pcm=copy,.frames=frames,.station=station,.fd=fd}; return true;
}
size_t fv_fragment_cache_count(const FvFragmentEngine *e) { return e ? e->cache_count : 0u; }
const int16_t *fv_fragment_cache_latest(const FvFragmentEngine *e,size_t *frames,int *station) {
    if (!e || !e->cache_count || !frames || !station) return NULL;
    const Clip *c=&e->cache[e->cache_count-1u]; *frames=c->frames; *station=c->station; return c->pcm;
}
/* Compatibility feeder for host callers. Production LIVE uses completed network clips. */
bool fv_fragment_feed(FvFragmentEngine *e,int deck,int station,const int16_t *pcm,size_t frames) {
    if (!e || !pcm || deck<0 || deck>=FV_FRAGMENT_DECKS || station<0 || frames>FV_FRAGMENT_CLIP_FRAMES) return false;
    Deck *d=&e->decks[deck];
    if (!d->pcm) { d->pcm=malloc(FEED_FRAMES*sizeof(*d->pcm)); if (!d->pcm) return false; }
    if (d->station != station) { d->count=0; d->station=station; }
    bool added=false;
    while (frames) {
        size_t n=FEED_FRAMES-d->count; if (n>frames) n=frames;
        memcpy(d->pcm+d->count,pcm,n*sizeof(*pcm)); d->count+=n; pcm+=n; frames-=n;
        if (d->count==FEED_FRAMES) { added=fv_fragment_cache_add(e,d->pcm,d->count,station)||added; d->count=0; }
    }
    return added;
}
static bool valid_trigger(const FvVoxDecimalTrigger *t,uint64_t clock) {
    return t && t->passed && clock <= UINT64_MAX-600000u && isfinite(t->pitch_factor) &&
        isfinite(t->speed_factor) && isfinite(t->seek_offset) && t->channel_hop>=0 &&
        t->grain_duration_ms>=1 && t->grain_duration_ms<=1000;
}
static bool eligible(const FvFragmentEngine *e,int station,uint64_t clock,bool speech) {
    if (speech || e->settings.paused || e->live_pending) return false;
    const uint64_t cooldown=(uint64_t)((2000.0+8000.0*(double)e->settings.reverb)*48.0);
    if (e->triggered && (clock<e->last_trigger || clock-e->last_trigger<cooldown)) return false;
    return station<0 || station>=512 || !e->station_seen[station] ||
        (clock>=e->station_trigger[station] && clock-e->station_trigger[station]>=5u*FV_FRAGMENT_RATE);
}
static int free_slot(FvFragmentEngine *e,uint64_t clock) {
    int slot=-1;
    for (unsigned i=0;i<FV_FRAGMENT_VOICES;i++) {
        Voice *v=&e->voices[i];
        if (v->audio && clock>=v->release) release_audio(&v->audio);
        if (slot<0 && !v->audio) slot=(int)i;
    }
    return slot;
}
static bool admit(FvFragmentEngine *e,const FvVoxDecimalTrigger *t,const int16_t *pcm,
                   size_t frames,int station,uint64_t clock,bool cached,int source_fd) {
    if (!valid_trigger(t,clock) || !pcm || !frames || frames>FV_FRAGMENT_CLIP_FRAMES) return false;
    const int slot=free_slot(e,clock); if (slot<0) return false;
    size_t seek=(size_t)((double)unit(t->seek_offset)*(double)frames);
    size_t duration=(size_t)t->grain_duration_ms*48u; if (duration>frames) duration=frames;
    if (seek>frames-duration) seek=frames>duration+480u ? frames-duration-480u : 0u;
    Audio *audio=malloc(sizeof(*audio)+MAX_RENDER*sizeof(*audio->pcm)); if (!audio) return false;
    audio->frames=fv_fragment_transform(pcm+seek,duration,t->pitch_factor,t->speed_factor,audio->pcm,MAX_RENDER);
    if (!audio->frames) { free(audio); return false; }
    if (e->speaker && !e->speaker(e->speaker_context,source_fd,pcm,frames,t,e->settings)) { free(audio); return false; }
    audio->references=1;
    e->voices[slot]=(Voice){.audio=audio,.start=clock,.release=clock+audio->frames+(uint64_t)(432000.0f*e->settings.reverb),
        .volume=1.0f,.send=e->settings.reverb,.station=station,.cached=cached,.primary=true};
    e->admitted++; e->pitch=t->pitch_factor; e->speed=t->speed_factor; e->grain_ms=(int)(duration/48u);
    e->seek_fraction=(float)((double)seek/(double)frames); e->last_station=station; e->last_cached=cached;
    if (e->settings.echo>0.0f) {
        const uint64_t interval=(uint64_t)(e->settings.echo*200.0f)*48u;
        for (int tap=1;tap<=4;tap++) for (unsigned i=0;i<ECHO_EVENTS;i++) if (!e->echoes[i].audio) {
            audio->references++;
            e->echoes[i]=(Echo){.audio=audio,.due=clock+(uint64_t)tap*interval,
                .volume=fv_vox_echo_tap_volume(1.0f,tap),.station=station,.cached=cached}; break;
        }
    }
    return true;
}
static void mark_trigger(FvFragmentEngine *e,int station,uint64_t clock) {
    e->last_trigger=clock; e->triggered=true;
    if (station>=0 && station<512) { e->station_seen[station]=true; e->station_trigger[station]=clock; }
}
bool fv_fragment_trigger_pcm(FvFragmentEngine *e,const FvVoxDecimalTrigger *t,const int16_t *pcm,
                             size_t frames,int station,uint64_t clock,bool speech) {
    if (!e || !valid_trigger(t,clock) || !eligible(e,station,clock,speech)) return false;
    if (!admit(e,t,pcm,frames,station,clock,false,-1)) return false;
    mark_trigger(e,station,clock); return true;
}
bool fv_fragment_begin_live(FvFragmentEngine *e,const FvVoxDecimalTrigger *t,int station,uint64_t clock,bool speech) {
    if (!e || station<0 || station>=512 || !valid_trigger(t,clock) || !eligible(e,station,clock,speech)) return false;
    mark_trigger(e,station,clock); e->live_trigger=*t; e->live_station=station; e->live_pending=true; return true;
}
bool fv_fragment_finish_live(FvFragmentEngine *e,const int16_t *pcm,size_t frames,uint64_t clock,bool speech) {
    return fv_fragment_finish_live_source(e,pcm,frames,-1,clock,speech);
}
bool fv_fragment_finish_live_source(FvFragmentEngine *e,const int16_t *pcm,size_t frames,int source_fd,uint64_t clock,bool speech) {
    if (!e || !e->live_pending) return false;
    e->live_pending=false;
    if (!pcm || !frames || frames>FV_FRAGMENT_CLIP_FRAMES) return false;
    fv_fragment_cache_add_source(e,pcm,frames,e->live_station,source_fd);
    return !speech && !e->settings.paused && admit(e,&e->live_trigger,pcm,frames,e->live_station,clock,false,source_fd);
}
bool fv_fragment_live_pending(const FvFragmentEngine *e) { return e && e->live_pending; }
bool fv_fragment_trigger(FvFragmentEngine *e,const FvVoxDecimalTrigger *t,bool local_only,uint64_t clock,bool speech) {
    if (!e || !valid_trigger(t,clock)) return false;
    if (!local_only && t->prefer_live) {
        Deck *d=&e->decks[(unsigned)t->channel_hop%FV_FRAGMENT_DECKS];
        return d->count >= (size_t)t->grain_duration_ms*48u &&
            fv_fragment_trigger_pcm(e,t,d->pcm,d->count,d->station,clock,speech);
    }
    if (!e->cache_count || !eligible(e,-1,clock,speech)) return false;
    size_t index=(size_t)(unsigned)t->channel_hop;
    if (index>e->cache_count) index%=e->cache_count;
    if (index==e->cache_count) return false;
    Clip *c=&e->cache[index];
    if (c->uses>=5u) return false;
    c->uses++; mark_trigger(e,-1,clock);
    return admit(e,t,c->pcm,c->frames,c->station,clock,true,c->fd);
}
static void dispatch_echoes(FvFragmentEngine *e,uint64_t clock) {
    for (unsigned i=0;i<ECHO_EVENTS;i++) {
        Echo *event=&e->echoes[i]; if (!event->audio || clock<event->due) continue;
        const int slot=free_slot(e,clock);
        if (slot>=0 && clock-event->due<event->audio->frames) {
            e->voices[slot]=(Voice){.audio=event->audio,.start=event->due,
                .release=event->due+event->audio->frames,.volume=event->volume,.station=event->station,.cached=event->cached};
            event->audio=NULL; /* transfer the event's reference to the player */
        } else release_audio(&event->audio);
    }
}
void fv_fragment_render(FvFragmentEngine *e,int16_t *clean,int16_t *processed,size_t frames,uint64_t clock,FvFragmentStatus *status) {
    fv_fragment_render_aux(e,clean,processed,NULL,frames,clock,status);
}
void fv_fragment_render_aux(FvFragmentEngine *e,int16_t *clean,int16_t *processed,int16_t *aux,size_t frames,uint64_t clock,FvFragmentStatus *status) {
    if (!e || !clean || !processed || frames>UINT64_MAX-clock) return;
    FvFragmentStatus s={.station=e->last_station,.cached=e->last_cached,.cache_count=(unsigned)e->cache_count,
        .admitted=e->admitted,.pitch=e->pitch,.speed=e->speed,.grain_ms=e->grain_ms,.seek_fraction=e->seek_fraction};
    if (e->settings.paused) {
        memset(clean,0,frames*sizeof(*clean)); memset(processed,0,frames*sizeof(*processed));
        if (aux) memset(aux,0,frames*sizeof(*aux));
        if (status) *status=s;
        return;
    }
    for (size_t off=0;off<frames;) {
        const size_t n=frames-off>64u ? 64u : frames-off;
        float sends[64]={0}, base[64]={0}, wet[64]={0};
        for (size_t i=0;i<n;i++) {
            const uint64_t now=clock+off+i;
            dispatch_echoes(e,now);
            float dry=0.0f,original=0.0f,send=0.0f;
            for (unsigned j=0;j<FV_FRAGMENT_VOICES;j++) {
                Voice *v=&e->voices[j]; if (!v->audio) continue;
                if (now>=v->release) { release_audio(&v->audio); continue; }
                if (now<v->start || now-v->start>=v->audio->frames) continue;
                const float sample=(float)v->audio->pcm[now-v->start]*v->volume;
                dry+=sample; send+=sample*v->send; if (v->primary) original+=sample;
                s.station=v->station; s.playing=true; s.cached=v->cached;
            }
            clean[off+i]=saturate(original); sends[i]=send;
            float noise=0.0f; const uint64_t ms=now/48u;
            if (e->settings.noise && ms%((uint64_t)e->settings.sweep_ms+40u)<e->settings.sweep_ms) {
                e->rng^=e->rng<<13u; e->rng^=e->rng>>17u; e->rng^=e->rng<<5u;
                noise=(((float)(e->rng&65535u)/65535.0f)*0.4f-0.2f)*32767.0f*0.02f;
            }
            base[i]=dry+noise;
            if (aux) aux[off+i]=saturate(noise);
        }
        fv_environment_reverb_process(e->reverb,sends,wet,n);
        for (size_t i=0;i<n;i++) processed[off+i]=saturate(base[i]+wet[i]);
        off+=n;
    }
    for (unsigned i=0;i<FV_FRAGMENT_VOICES;i++) if (e->voices[i].audio) s.voices++;
    if (status) *status=s;
}
