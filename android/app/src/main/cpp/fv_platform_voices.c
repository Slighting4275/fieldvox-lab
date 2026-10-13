#define _GNU_SOURCE
#include "fv_platform_voices.h"
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"FieldVoxVoices",__VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,"FieldVoxVoices",__VA_ARGS__)
#define PLAYERS 10u
#define EVENTS 64u
/* Four C23 preparation workers share ten individually locked platform players.
 * Audio/UI threads enqueue owned sources and never wait for prepare or JNI. */
typedef struct {
    int fd;
    unsigned references;
    FvVoxDecimalTrigger trigger;
    FvFragmentSettings settings;
    int seek_ms, play_ms;
    uint64_t start;
} Source;
typedef struct { Source *source; uint64_t due; unsigned generation; float volume; bool primary; } Event;
typedef struct { pthread_mutex_t mutex; jobject object; bool busy, playing, muted; uint64_t stop, release; unsigned generation; float volume, send; } Player;
struct FvPlatformVoices {
    JavaVM *vm;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool running, initialized, healthy, enabled, muted;
    unsigned generation;
    float reverb;
    Event events[EVENTS];
};
typedef struct {
    jclass media, parcel;
    jobject attributes, effect;
    jmethodID constructor, reset, release, attributes_set, data_source, prepare, duration, seek,
        volume, attach, send, params_get, params_set, start, pause;
    jmethodID adopt, descriptor, close, speed, pitch;
    jmethodID effect_release, effect_id, effect_enable, diffusion, density, decay, decay_hf,
        delay, level, room, room_hf, decay_get;
} Api;
static uint64_t millis(void) {
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC,&t)!=0) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static float unit(float x) { return isfinite(x) ? fmaxf(0.0f,fminf(1.0f,x)) : 0.0f; }
static bool checked(JNIEnv *env,const char *operation) {
    if (!(*env)->ExceptionCheck(env)) return true;
    LOGW("platform exception: %s",operation);
    (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); return false;
}
static jmethodID method(JNIEnv *env,jclass c,const char *name,const char *signature) {
    if (!c) return NULL;
    jmethodID id=(*env)->GetMethodID(env,c,name,signature);
    return checked(env,name) ? id : NULL;
}
static void source_release(Source *source) {
    if (source && --source->references==0u) { close(source->fd); free(source); }
}
/* Caller holds queue mutex; the worker owns popped references. */
static void clear_queue(FvPlatformVoices *v) {
    for (unsigned i=0;i<EVENTS;i++) if (v->events[i].source) {
        source_release(v->events[i].source); v->events[i].source=NULL;
    }
}
static jclass find_class(JNIEnv *env,const char *name) {
    jclass c=(*env)->FindClass(env,name);
    return checked(env,name) ? c : NULL;
}
static jclass global_class(JNIEnv *env,const char *name) {
    jclass local=find_class(env,name); if (!local) return NULL;
    jclass global=(*env)->NewGlobalRef(env,local);
    (*env)->DeleteLocalRef(env,local);
    return checked(env,"class reference") ? global : NULL;
}
static bool init_api(JNIEnv *env,Api *a,Player players[PLAYERS]) {
    memset(a,0,sizeof(*a));
    a->media=global_class(env,"android/media/MediaPlayer");
    a->parcel=global_class(env,"android/os/ParcelFileDescriptor");
    jclass params=find_class(env,"android/media/PlaybackParams");
    jclass builder=find_class(env,"android/media/AudioAttributes$Builder");
    jclass reverb=find_class(env,"android/media/audiofx/EnvironmentalReverb");
    if (!checked(env,"classes") || !a->media || !a->parcel || !params || !builder || !reverb) return false;
#define M(field,cls,name,sig) a->field=method(env,cls,name,sig); if (!checked(env,name) || !a->field) return false
    M(constructor,a->media,"<init>","()V");
    M(reset,a->media,"reset","()V"); M(release,a->media,"release","()V");
    M(attributes_set,a->media,"setAudioAttributes","(Landroid/media/AudioAttributes;)V");
    M(data_source,a->media,"setDataSource","(Ljava/io/FileDescriptor;JJ)V");
    M(prepare,a->media,"prepare","()V"); M(duration,a->media,"getDuration","()I");
    M(seek,a->media,"seekTo","(I)V"); M(volume,a->media,"setVolume","(FF)V");
    M(attach,a->media,"attachAuxEffect","(I)V"); M(send,a->media,"setAuxEffectSendLevel","(F)V");
    M(params_get,a->media,"getPlaybackParams","()Landroid/media/PlaybackParams;");
    M(params_set,a->media,"setPlaybackParams","(Landroid/media/PlaybackParams;)V");
    M(start,a->media,"start","()V"); M(pause,a->media,"pause","()V");
    a->adopt=(*env)->GetStaticMethodID(env,a->parcel,"adoptFd","(I)Landroid/os/ParcelFileDescriptor;");
    if (!checked(env,"adoptFd") || !a->adopt) return false;
    M(descriptor,a->parcel,"getFileDescriptor","()Ljava/io/FileDescriptor;"); M(close,a->parcel,"close","()V");
    M(speed,params,"setSpeed","(F)Landroid/media/PlaybackParams;");
    M(pitch,params,"setPitch","(F)Landroid/media/PlaybackParams;");
    M(effect_release,reverb,"release","()V"); M(effect_id,reverb,"getId","()I");
    M(effect_enable,reverb,"setEnabled","(Z)I");
    M(diffusion,reverb,"setDiffusion","(S)V"); M(density,reverb,"setDensity","(S)V");
    M(decay,reverb,"setDecayTime","(I)V"); M(decay_hf,reverb,"setDecayHFRatio","(S)V");
    M(delay,reverb,"setReverbDelay","(I)V"); M(level,reverb,"setReverbLevel","(S)V");
    M(room,reverb,"setRoomLevel","(S)V"); M(room_hf,reverb,"setRoomHFLevel","(S)V");
    M(decay_get,reverb,"getDecayTime","()I");
#undef M
    jmethodID bc=method(env,builder,"<init>","()V");
    jmethodID usage=method(env,builder,"setUsage","(I)Landroid/media/AudioAttributes$Builder;");
    jmethodID content=method(env,builder,"setContentType","(I)Landroid/media/AudioAttributes$Builder;");
    jmethodID build=method(env,builder,"build","()Landroid/media/AudioAttributes;");
    jmethodID effect_constructor=method(env,reverb,"<init>","(II)V");
    if (!bc || !usage || !content || !build || !effect_constructor || !checked(env,"constructors")) return false;
    jobject b=(*env)->NewObject(env,builder,bc);
    if (!checked(env,"attribute builder") || !b) return false;
    (*env)->CallObjectMethod(env,b,usage,1);
    if (!checked(env,"attribute usage")) return false;
    (*env)->CallObjectMethod(env,b,content,2);
    if (!checked(env,"attribute setup")) return false;
    jobject attr=(*env)->CallObjectMethod(env,b,build);
    if (!checked(env,"attributes") || !attr) return false;
    a->attributes=(*env)->NewGlobalRef(env,attr);
    if (!checked(env,"attribute reference") || !a->attributes) return false;
    jobject fx=(*env)->NewObject(env,reverb,effect_constructor,1,0);
    if (!checked(env,"environmental reverb session 0") || !fx) return false;
    a->effect=(*env)->NewGlobalRef(env,fx);
    if (!checked(env,"global references") || !a->attributes || !a->effect) return false;
    for (unsigned i=0;i<PLAYERS;i++) {
        jobject p=(*env)->NewObject(env,a->media,a->constructor);
        if (!checked(env,"player constructor") || !p) return false;
        players[i].object=(*env)->NewGlobalRef(env,p);
        if (!checked(env,"player reference") || !players[i].object) return false;
        (*env)->DeleteLocalRef(env,p);
    }
    return true;
}
static void effect_mix(JNIEnv *env,Api *a,float mix,bool enabled) {
    if (!a->effect) return;
    if (!enabled || mix<=0.0f) {
        (*env)->CallIntMethod(env,a->effect,a->effect_enable,(jboolean)false);
        checked(env,"disable reverb");
        if (mix<=0.0f) return;
    }
    /* Keep the platform's validation and clamping; do not invent a different
     * native decay limit. A rejected setting stops this update sequence. */
#define FX(id,val) (*env)->CallVoidMethod(env,a->effect,a->id,val); if (!checked(env,#id)) { \
    const jint retained=(*env)->CallIntMethod(env,a->effect,a->decay_get); \
    if (checked(env,"retained decay")) LOGW("reverb update rejected: requested=%dms retained=%dms",(int)(9000.0f*mix),(int)retained); \
    return; }
    FX(diffusion,(jshort)(1000.0f*mix)); FX(density,(jshort)1000);
    FX(decay,(jint)(9000.0f*mix)); FX(decay_hf,(jshort)1000);
    FX(delay,(jint)50); FX(level,(jshort)(10000.0f*mix-10000.0f));
    FX(room,(jshort)-1000); FX(room_hf,(jshort)0);
#undef FX
    const jint status=(*env)->CallIntMethod(env,a->effect,a->effect_enable,(jboolean)(enabled && mix>0.0f));
    if (!checked(env,"effect enable state")) return;
    const jint decay=(*env)->CallIntMethod(env,a->effect,a->decay_get);
    if (checked(env,"effect state")) LOGI("reverb requested=%dms accepted=%dms mix=%.2f status=%d",(int)(9000.0f*mix),(int)decay,(double)mix,(int)status);
}
static void reset_players(JNIEnv *env,Api *a,Player players[PLAYERS]) {
    for (unsigned i=0;i<PLAYERS;i++) if (players[i].object) {
        pthread_mutex_lock(&players[i].mutex);
        (*env)->CallVoidMethod(env,players[i].object,a->reset); checked(env,"reset");
        players[i].busy=players[i].playing=false;
        pthread_mutex_unlock(&players[i].mutex);
    }
}
static bool play(JNIEnv *env,Api *a,Player *player,Event *event,bool muted,float reverb,FvPlatformVoices *v) {
    Source *s=event->source;
    bool ok=false; jobject parcel=NULL;
    if ((*env)->PushLocalFrame(env,24)!=JNI_OK) { checked(env,"play frame"); return false; }
#define CALL(id,...) (*env)->CallVoidMethod(env,player->object,a->id __VA_OPT__(,) __VA_ARGS__); if (!checked(env,#id)) goto done
    CALL(reset); CALL(attributes_set,a->attributes);
    struct stat st;
    if (fstat(s->fd,&st)!=0 || st.st_size<=0) goto done;
    const int owned=fcntl(s->fd,F_DUPFD_CLOEXEC,0);
    if (owned<0) goto done;
    parcel=(*env)->CallStaticObjectMethod(env,a->parcel,a->adopt,owned);
    if (!parcel) { close(owned); checked(env,"adopt source descriptor"); goto done; }
    if (!checked(env,"adopt source descriptor")) goto done;
    jobject descriptor=(*env)->CallObjectMethod(env,parcel,a->descriptor);
    if (!checked(env,"file descriptor") || !descriptor) goto done;
    CALL(data_source,descriptor,(jlong)0,(jlong)st.st_size);
    (*env)->CallVoidMethod(env,parcel,a->close); parcel=NULL;
    if (!checked(env,"source descriptor close")) goto done;
    CALL(prepare);
    if (event->primary) {
        const jint total=(*env)->CallIntMethod(env,player->object,a->duration);
        if (!checked(env,"duration") || total<=0) goto done;
        const int grain=s->trigger.grain_duration_ms;
        int length=grain;
        s->seek_ms=(int)((float)total*unit(s->trigger.seek_offset));
        if (s->seek_ms>total-length) {
            s->seek_ms=total-length-10; if (s->seek_ms<0) s->seek_ms=0;
            if (length>total-s->seek_ms) length=total-s->seek_ms;
        }
        s->play_ms=(int)((float)length/s->trigger.speed_factor);
        if (s->play_ms<1) goto done;
    }
    CALL(seek,s->seek_ms);
    const float volume=muted ? 0.0f : event->volume;
    CALL(volume,volume,volume);
    if (event->primary) {
        const jint id=(*env)->CallIntMethod(env,a->effect,a->effect_id);
        if (!checked(env,"effect id")) goto done;
        CALL(attach,id); CALL(send,muted ? 0.0f : reverb);
    }
    jobject params=(*env)->CallObjectMethod(env,player->object,a->params_get);
    if (!checked(env,"playback parameters") || !params) goto done;
    (*env)->CallObjectMethod(env,params,a->speed,s->trigger.speed_factor);
    if (!checked(env,"setSpeed")) goto done;
    (*env)->CallObjectMethod(env,params,a->pitch,s->trigger.pitch_factor);
    if (!checked(env,"setPitch")) goto done;
    CALL(params_set,params);
    pthread_mutex_lock(&v->mutex);
    const bool current=v->running && v->enabled && v->generation==event->generation;
    pthread_mutex_unlock(&v->mutex);
    if (!current) goto done;
    CALL(start);
    const uint64_t now=millis();
    player->busy=player->playing=true; player->muted=muted; player->generation=event->generation;
    if (event->primary) s->start=now; player->volume=event->volume;
    player->send=event->primary ? reverb : 0.0f;
    player->stop=now+(uint64_t)(unsigned)s->play_ms;
    player->release=player->stop+(event->primary ? (uint64_t)(9000.0f*s->settings.reverb) : 0u);
    LOGI("%s seek=%d play=%d pitch=%.2f speed=%.2f gain=%.5f source=%lld bytes",event->primary ? "primary" : "echo",s->seek_ms,s->play_ms,(double)s->trigger.pitch_factor,(double)s->trigger.speed_factor,(double)event->volume,(long long)st.st_size);
    ok=true;
done:
    if (parcel) { (*env)->CallVoidMethod(env,parcel,a->close); checked(env,"source close after error"); }
    if (!ok) { (*env)->CallVoidMethod(env,player->object,a->reset); checked(env,"failed player reset"); player->busy=player->playing=false; }
    (*env)->PopLocalFrame(env,NULL);
#undef CALL
    return ok;
}
static void cleanup(JNIEnv *env,Api *a,Player players[PLAYERS]) {
    for (unsigned i=0;i<PLAYERS;i++) if (players[i].object) {
        if (a->release) { (*env)->CallVoidMethod(env,players[i].object,a->release); checked(env,"player release"); }
        (*env)->DeleteGlobalRef(env,players[i].object);
    }
    if (a->effect) {
        if (a->effect_release) { (*env)->CallVoidMethod(env,a->effect,a->effect_release); checked(env,"effect release"); }
        (*env)->DeleteGlobalRef(env,a->effect);
    }
    if (a->attributes) (*env)->DeleteGlobalRef(env,a->attributes);
    if (a->media) (*env)->DeleteGlobalRef(env,a->media);
    if (a->parcel) (*env)->DeleteGlobalRef(env,a->parcel);
}
/* Stop/release deadlines run independently of decoder preparation. A slow
 * prepare on one slot must never extend another grain's playback duration. */
typedef struct { FvPlatformVoices *voices; Api *api; Player *players; atomic_bool running, ready, available; } Timer;
static void *stop_timer(void *context) {
    Timer *timer=context; FvPlatformVoices *v=timer->voices; Api *a=timer->api;
    JNIEnv *env=NULL;
    const bool attached=(*v->vm)->AttachCurrentThread(v->vm,&env,NULL)==JNI_OK;
    atomic_store(&timer->available,attached); atomic_store(&timer->ready,true);
    if (!attached) return NULL;
    while (atomic_load(&timer->running)) {
        pthread_mutex_lock(&v->mutex); const bool enabled=v->enabled,muted=v->muted; const unsigned generation=v->generation; pthread_mutex_unlock(&v->mutex);
        const uint64_t now=millis();
        for (unsigned i=0;i<PLAYERS;i++) {
            Player *p=&timer->players[i];
            if (pthread_mutex_trylock(&p->mutex)!=0) continue;
            if (p->busy && (!enabled || p->generation!=generation)) {
                (*env)->CallVoidMethod(env,p->object,a->reset); checked(env,"cancel voice"); p->busy=p->playing=false;
            }
            if (p->playing && now>=p->stop) {
                (*env)->CallVoidMethod(env,p->object,a->pause); checked(env,"scheduled pause");
                LOGI("grain stop: late=%llums",(unsigned long long)(now-p->stop)); p->playing=false;
            }
            if (p->busy && now>=p->release) {
                (*env)->CallVoidMethod(env,p->object,a->reset); checked(env,"slot release"); p->busy=false;
            }
            if (p->busy && p->muted!=muted) {
                const float gain=muted ? 0.0f : p->volume;
                (*env)->CallVoidMethod(env,p->object,a->volume,gain,gain); checked(env,"timer monitor volume");
                (*env)->CallVoidMethod(env,p->object,a->send,muted ? 0.0f : p->send); checked(env,"timer monitor send");
                p->muted=muted;
            }
            pthread_mutex_unlock(&p->mutex);
        }
        struct timespec delay={.tv_sec=0,.tv_nsec=2000000L}; nanosleep(&delay,NULL);
    }
    (*v->vm)->DetachCurrentThread(v->vm); return NULL;
}
static bool claim_and_play(JNIEnv *env,Api *a,Player players[PLAYERS],Event *event,
    bool muted,float reverb,FvPlatformVoices *v,int *slot) {
    *slot=-1;
    for (unsigned i=0;i<PLAYERS;i++) {
        pthread_mutex_lock(&players[i].mutex);
        if (!players[i].busy) {
            *slot=(int)i; players[i].busy=true;
            const bool ok=play(env,a,&players[i],event,muted,reverb,v);
            pthread_mutex_unlock(&players[i].mutex); return ok;
        }
        pthread_mutex_unlock(&players[i].mutex);
    }
    return false;
}
typedef struct {
    FvPlatformVoices *voices; Api *api; Player *players;
    atomic_bool running, failed; atomic_uint ready;
} EchoWorkers;
static void *echo_worker(void *context) {
    EchoWorkers *pool=context; FvPlatformVoices *v=pool->voices; JNIEnv *env=NULL;
    const bool attached=(*v->vm)->AttachCurrentThread(v->vm,&env,NULL)==JNI_OK;
    if (!attached) atomic_store(&pool->failed,true);
    atomic_fetch_add(&pool->ready,1u);
    if (!attached) return NULL;
    while (atomic_load(&pool->running)) {
        Event event={0}; const uint64_t now=millis();
        pthread_mutex_lock(&v->mutex);
        const bool enabled=v->enabled,muted=v->muted; const float reverb=v->reverb;
        const unsigned generation=v->generation;
        unsigned next=EVENTS;
        for (unsigned i=0;i<EVENTS;i++) if (v->events[i].source && !v->events[i].primary && v->events[i].due<=now &&
            (next==EVENTS || v->events[i].due<v->events[next].due)) next=i;
        if (next<EVENTS) { event=v->events[next]; v->events[next].source=NULL; }
        pthread_mutex_unlock(&v->mutex);
        if (event.source) {
            int slot=-1;
            if (enabled && generation==event.generation) claim_and_play(env,pool->api,pool->players,&event,muted,reverb,v,&slot);
            pthread_mutex_lock(&v->mutex); source_release(event.source); pthread_mutex_unlock(&v->mutex);
        }
        pthread_mutex_lock(&v->mutex);
        if (atomic_load(&pool->running) && v->running) {
            struct timespec wake; clock_gettime(CLOCK_MONOTONIC,&wake); wake.tv_nsec+=5000000L;
            if (wake.tv_nsec>=1000000000L) { wake.tv_sec++; wake.tv_nsec-=1000000000L; }
            pthread_cond_timedwait(&v->changed,&v->mutex,&wake);
        }
        pthread_mutex_unlock(&v->mutex);
    }
    (*v->vm)->DetachCurrentThread(v->vm); return NULL;
}
static void *voice_worker(void *context) {
    FvPlatformVoices *v=context; JNIEnv *env=NULL; Api api={0}; Player players[PLAYERS]={0};
    unsigned initialized_mutexes=0;
    for (;initialized_mutexes<PLAYERS;initialized_mutexes++) if (pthread_mutex_init(&players[initialized_mutexes].mutex,NULL)!=0) break;
    const bool attached=(*v->vm)->AttachCurrentThread(v->vm,&env,NULL)==JNI_OK;
    bool healthy=attached && initialized_mutexes==PLAYERS && init_api(env,&api,players);
    Timer timer={.voices=v,.api=&api,.players=players,.running=true}; pthread_t timer_thread;
    const bool timer_started=healthy && pthread_create(&timer_thread,NULL,stop_timer,&timer)==0;
    while (timer_started && !atomic_load(&timer.ready)) {
        struct timespec delay={.tv_sec=0,.tv_nsec=1000000L}; nanosleep(&delay,NULL);
    }
    healthy=healthy && timer_started && atomic_load(&timer.available);
    EchoWorkers echo_pool={.voices=v,.api=&api,.players=players,.running=true};
    pthread_t echo_threads[3]; unsigned echo_count=0;
    if (healthy) for (;echo_count<3u;echo_count++) if (pthread_create(&echo_threads[echo_count],NULL,echo_worker,&echo_pool)!=0) break;
    while (atomic_load(&echo_pool.ready)<echo_count) {
        struct timespec delay={.tv_sec=0,.tv_nsec=1000000L}; nanosleep(&delay,NULL);
    }
    healthy=healthy && echo_count==3u && !atomic_load(&echo_pool.failed);
    pthread_mutex_lock(&v->mutex); v->initialized=true; v->healthy=healthy;
    pthread_cond_broadcast(&v->changed); pthread_mutex_unlock(&v->mutex);
    if (!healthy) goto done;
    LOGI("C23 platform voice pool active: %u players, session-0 environmental reverb",PLAYERS);
    unsigned generation=UINT_MAX; bool old_muted=false,old_enabled=false;
    float old_reverb=-1.0f;
    for (;;) {
        pthread_mutex_lock(&v->mutex);
        const bool running=v->running,enabled=v->enabled,muted=v->muted;
        const float reverb=v->reverb; const unsigned current=v->generation;
        Event event={0};
        const uint64_t now=millis(); unsigned next=EVENTS;
        for (unsigned i=0;i<EVENTS;i++) if (v->events[i].source && v->events[i].primary && v->events[i].due<=now &&
            (next==EVENTS || v->events[i].due<v->events[next].due)) next=i;
        if (next<EVENTS) { event=v->events[next]; v->events[next].source=NULL; }
        pthread_mutex_unlock(&v->mutex);
        if (!running) { pthread_mutex_lock(&v->mutex); source_release(event.source); pthread_mutex_unlock(&v->mutex); break; }
        if (current!=generation) { reset_players(env,&api,players); generation=current; }
        if (old_reverb!=reverb || old_muted!=muted || old_enabled!=enabled) {
            effect_mix(env,&api,reverb,enabled && !muted);
            for (unsigned i=0;i<PLAYERS;i++) {
                pthread_mutex_lock(&players[i].mutex);
                if (players[i].busy) {
                const float level=muted ? 0.0f : players[i].volume;
                (*env)->CallVoidMethod(env,players[i].object,api.volume,level,level); checked(env,"monitor volume");
                (*env)->CallVoidMethod(env,players[i].object,api.send,muted ? 0.0f : players[i].send); checked(env,"monitor send");
                players[i].muted=muted;
                }
                pthread_mutex_unlock(&players[i].mutex);
            }
            old_reverb=reverb; old_muted=muted; old_enabled=enabled;
        }
        if (event.source) {
            int slot=-1; bool played=false;
            if (enabled && event.generation==current)
                played=claim_and_play(env,&api,players,&event,muted,reverb,v,&slot);
            pthread_mutex_lock(&v->mutex);
            if (v->generation!=current || !v->enabled) {
                pthread_mutex_unlock(&v->mutex);
                if (played) {
                    pthread_mutex_lock(&players[slot].mutex);
                    if (players[slot].generation==event.generation) {
                        (*env)->CallVoidMethod(env,players[slot].object,api.reset); checked(env,"cancel prepared voice");
                        players[slot].busy=players[slot].playing=false;
                    }
                    pthread_mutex_unlock(&players[slot].mutex);
                }
            } else {
                if (!played && event.primary && slot>=0) {
                    v->healthy=false; v->enabled=false; v->generation++; clear_queue(v);
                    LOGW("Platform playback failed; switching speaker output to native PCM");
                }
                if (played && event.primary && event.source->settings.echo>0.0f) {
                    const uint64_t start=event.source->start;
                    const unsigned interval=(unsigned)(event.source->settings.echo*200.0f);
                    for (int tap=1;tap<=4;tap++) for (unsigned i=0;i<EVENTS;i++) if (!v->events[i].source) {
                        event.source->references++;
                        v->events[i]=(Event){.source=event.source,.generation=current,.due=start+(uint64_t)(unsigned)tap*interval,
                            .volume=fv_vox_echo_tap_volume(1.0f,tap),.primary=false}; break;
                    }
                }
                pthread_mutex_unlock(&v->mutex);
            }
            pthread_mutex_lock(&v->mutex); source_release(event.source); pthread_mutex_unlock(&v->mutex);
        }
        pthread_mutex_lock(&v->mutex);
        if (v->running) {
            struct timespec wake; clock_gettime(CLOCK_MONOTONIC,&wake); wake.tv_nsec+=5000000L;
            if (wake.tv_nsec>=1000000000L) { wake.tv_sec++; wake.tv_nsec-=1000000000L; }
            pthread_cond_timedwait(&v->changed,&v->mutex,&wake);
        }
        pthread_mutex_unlock(&v->mutex);
    }
done:
    atomic_store(&echo_pool.running,false);
    pthread_mutex_lock(&v->mutex); pthread_cond_broadcast(&v->changed); pthread_mutex_unlock(&v->mutex);
    for (unsigned i=0;i<echo_count;i++) pthread_join(echo_threads[i],NULL);
    if (timer_started) { atomic_store(&timer.running,false); pthread_join(timer_thread,NULL); }
    if (attached) { cleanup(env,&api,players); (*v->vm)->DetachCurrentThread(v->vm); }
    for (unsigned i=0;i<initialized_mutexes;i++) pthread_mutex_destroy(&players[i].mutex);
    return NULL;
}
FvPlatformVoices *fv_platform_voices_create(JavaVM *vm) {
    if (!vm) return NULL;
    FvPlatformVoices *v=calloc(1,sizeof(*v)); if (!v) return NULL;
    v->vm=vm; v->running=true; v->reverb=0.5f;
    if (pthread_mutex_init(&v->mutex,NULL)!=0) { free(v); return NULL; }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)!=0) { pthread_mutex_destroy(&v->mutex); free(v); return NULL; }
    const int clock_status=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    const int condition_status=clock_status==0 ? pthread_cond_init(&v->changed,&attr) : clock_status;
    pthread_condattr_destroy(&attr);
    if (condition_status!=0) { pthread_mutex_destroy(&v->mutex); free(v); return NULL; }
    if (pthread_create(&v->worker,NULL,voice_worker,v)!=0) {
        pthread_cond_destroy(&v->changed); pthread_mutex_destroy(&v->mutex); free(v); return NULL;
    }
    pthread_mutex_lock(&v->mutex);
    while (!v->initialized) pthread_cond_wait(&v->changed,&v->mutex);
    const bool healthy=v->healthy; pthread_mutex_unlock(&v->mutex);
    if (!healthy) { fv_platform_voices_destroy(v); return NULL; }
    return v;
}
void fv_platform_voices_destroy(FvPlatformVoices *v) {
    if (!v) return;
    pthread_mutex_lock(&v->mutex); v->running=false; v->enabled=false; v->generation++;
    clear_queue(v); pthread_cond_signal(&v->changed); pthread_mutex_unlock(&v->mutex);
    pthread_join(v->worker,NULL); pthread_cond_destroy(&v->changed); pthread_mutex_destroy(&v->mutex); free(v);
}
void fv_platform_voices_control(FvPlatformVoices *v,bool enabled,bool muted,float reverb) {
    if (!v) return;
    pthread_mutex_lock(&v->mutex);
    enabled=enabled && v->healthy;
    if (v->enabled!=enabled) { v->generation++; clear_queue(v); v->enabled=enabled; }
    v->muted=muted; v->reverb=unit(reverb); pthread_cond_signal(&v->changed); pthread_mutex_unlock(&v->mutex);
}
bool fv_platform_voices_healthy(FvPlatformVoices *v) {
    if (!v) return false;
    pthread_mutex_lock(&v->mutex); const bool healthy=v->healthy; pthread_mutex_unlock(&v->mutex); return healthy;
}
static void le32(uint8_t *at,uint32_t value) { for (unsigned i=0;i<4u;i++) at[i]=(uint8_t)(value>>(8u*i)); }
static bool write_all(int fd,const void *data,size_t bytes) {
    const uint8_t *p=data;
    while (bytes) { ssize_t n=write(fd,p,bytes); if (n<0 && errno==EINTR) continue; if (n<=0) return false; p+=(size_t)n; bytes-=(size_t)n; }
    return true;
}
static int pcm_source(const int16_t *pcm,size_t frames) {
    const int fd=memfd_create("fieldvox-source",MFD_CLOEXEC); if (fd<0) return -1;
    uint8_t header[44]={0}; memcpy(header,"RIFF",4); le32(header+4,36u+(uint32_t)frames*2u);
    memcpy(header+8,"WAVEfmt ",8); le32(header+16,16); header[20]=1; header[22]=1;
    le32(header+24,FV_FRAGMENT_RATE); le32(header+28,FV_FRAGMENT_RATE*2u); header[32]=2; header[34]=16;
    memcpy(header+36,"data",4); le32(header+40,(uint32_t)frames*2u);
    /* Android targets are little-endian; no conversion or short-grain flush. */
    if (!write_all(fd,header,sizeof(header)) || !write_all(fd,pcm,frames*sizeof(*pcm))) { close(fd); return -1; }
    return fd;
}
bool fv_platform_voices_submit(void *context,int source_fd,const int16_t *pcm,size_t frames,
    const FvVoxDecimalTrigger *trigger,FvFragmentSettings settings) {
    FvPlatformVoices *v=context;
    if (!v || !trigger || !pcm || !frames || frames>FV_FRAGMENT_CLIP_FRAMES ||
        !isfinite(trigger->pitch_factor) || !isfinite(trigger->speed_factor) ||
        trigger->pitch_factor<0.5f || trigger->pitch_factor>2.0f || trigger->speed_factor<0.5f || trigger->speed_factor>2.0f) return false;
    Source *s=calloc(1,sizeof(*s)); if (!s) return false;
    s->fd=source_fd>=0 ? fcntl(source_fd,F_DUPFD_CLOEXEC,0) : pcm_source(pcm,frames);
    if (s->fd<0) { free(s); return false; }
    s->references=1; s->trigger=*trigger; s->settings=settings;
    pthread_mutex_lock(&v->mutex);
    bool queued=false;
    if (v->running && v->enabled && v->healthy) for (unsigned i=0;i<EVENTS;i++) if (!v->events[i].source) {
        v->events[i]=(Event){.source=s,.due=millis(),.generation=v->generation,.volume=1.0f,.primary=true}; queued=true; break;
    }
    if (!queued) source_release(s); else pthread_cond_signal(&v->changed);
    pthread_mutex_unlock(&v->mutex); return queued;
}
