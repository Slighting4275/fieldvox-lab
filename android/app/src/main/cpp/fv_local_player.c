/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_local_player.h"
#include "fv_jni_helpers.h"
#include "fv_media_decode.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"FieldVoxPlayer",__VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,"FieldVoxPlayer",__VA_ARGS__)

typedef struct {
    char path[512];
    uint64_t generation;
    int64_t fallback_duration_us;
    atomic_bool cancel;
    pthread_t thread;
    bool started;
} WaveformJob;
static struct {
    pthread_mutex_t mutex;
    JavaVM *vm;
    pthread_t thread;
    bool started, backend_failed;
    atomic_bool running;
    uint64_t generation;
    bool pending_open, pending_close, desired_playing, desired_muted;
    bool scrub_resume, add_mark, timeline_readable;
    int64_t pending_seek, marker_target_ms;
    char path[512];
    FvLocalPlayerSnapshot state;
    uint32_t *pixels;
    int pixel_w, pixel_h;
    WaveformJob analysis;
} p = {.mutex=PTHREAD_MUTEX_INITIALIZER,.pending_seek=-1,.marker_target_ms=-1};

/* Caller holds p.mutex. A normal seek clears the marker selection. */
static void queue_seek_locked(int64_t ms, bool marker) {
    p.pending_seek = ms;
    p.marker_target_ms = marker ? ms : -1;
}

static int64_t marker_position_locked(bool *selected) {
    const int64_t position = p.pending_seek >= 0 ? p.pending_seek : p.state.position_ms;
    *selected = p.marker_target_ms >= 0 &&
                (p.pending_seek == p.marker_target_ms || !p.desired_playing ||
                 llabs(position - p.marker_target_ms) <= 250);
    return *selected ? p.marker_target_ms : position;
}

typedef struct {
    jclass cls, pfd_cls;
    jmethodID ctor, set_source, prepare, start, pause, release, seek, position, duration;
    jmethodID playing, width, height, volume, surface, adopt_fd, get_fd, close_fd;
} PlayerMethods;

static bool methods(JNIEnv *env,PlayerMethods *m) {
    m->cls=fv_jni_find_class(env,"FieldVoxPlayer","android/media/MediaPlayer",true);
    m->pfd_cls=fv_jni_find_class(env,"FieldVoxPlayer","android/os/ParcelFileDescriptor",true);
    if (!m->cls || !m->pfd_cls) return false;
#define METHOD(field,name,sig) m->field=fv_jni_method(env,"FieldVoxPlayer",m->cls,name,sig,true); if (!m->field) return false
    METHOD(ctor,"<init>","()V");METHOD(set_source,"setDataSource","(Ljava/io/FileDescriptor;JJ)V");
    METHOD(prepare,"prepare","()V");METHOD(start,"start","()V");METHOD(pause,"pause","()V");
    METHOD(release,"release","()V");METHOD(seek,"seekTo","(JI)V");
    METHOD(position,"getCurrentPosition","()I");METHOD(duration,"getDuration","()I");
    METHOD(playing,"isPlaying","()Z");METHOD(width,"getVideoWidth","()I");METHOD(height,"getVideoHeight","()I");
    METHOD(volume,"setVolume","(FF)V");METHOD(surface,"setSurface","(Landroid/view/Surface;)V");
#undef METHOD
    m->adopt_fd=fv_jni_static_method(env,"FieldVoxPlayer",m->pfd_cls,"adoptFd","(I)Landroid/os/ParcelFileDescriptor;",true);
    m->get_fd=fv_jni_method(env,"FieldVoxPlayer",m->pfd_cls,"getFileDescriptor","()Ljava/io/FileDescriptor;",true);
    m->close_fd=fv_jni_method(env,"FieldVoxPlayer",m->pfd_cls,"close","()V",true);
    return m->adopt_fd && m->get_fd && m->close_fd;
}
static bool cancelled(const WaveformJob *job) { return atomic_load(&job->cancel); }

static bool compressed_waveform(const WaveformJob *job,float peaks[FV_PLAYBACK_BINS]) {
    bool ok=false, input_eos=false, output_eos=false;
    uint64_t samples=0; int32_t sample_rate=0,channels=0; int encoding=2,idle=0;
    int fd=open(job->path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return false;
    struct stat st;
    AMediaExtractor *extractor=NULL; AMediaCodec *codec=NULL;
    int64_t duration_us=0;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 ||
        !fv_media_open_audio_track(fd,st.st_size,true,&extractor,&codec,&sample_rate,&channels,&duration_us)) goto done;
    if (duration_us<=0) duration_us=job->fallback_duration_us;
    if (duration_us<=0 || duration_us>INT64_MAX/FV_PLAYBACK_BINS) goto done;
    while (!output_eos && !cancelled(job) && idle<200) {
        bool progress=false;
        if (!input_eos) {
            const ssize_t index=AMediaCodec_dequeueInputBuffer(codec,0);
            if (index>=0) {
                size_t cap=0;uint8_t *input=AMediaCodec_getInputBuffer(codec,(size_t)index,&cap);
                if (!input) goto done;
                const ssize_t bytes=AMediaExtractor_readSampleData(extractor,input,cap);
                const int64_t timestamp=AMediaExtractor_getSampleTime(extractor);
                input_eos=bytes<0;
                if (bytes>(ssize_t)cap || AMediaCodec_queueInputBuffer(codec,(size_t)index,0,input_eos ? 0 : (size_t)bytes,
                    timestamp<0 ? 0 : (uint64_t)timestamp,input_eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0)!=AMEDIA_OK) goto done;
                if (!input_eos) AMediaExtractor_advance(extractor);
                progress=true;
            }
        }
        AMediaCodecBufferInfo info={0};
        const ssize_t index=AMediaCodec_dequeueOutputBuffer(codec,&info,5000);
        if (index==AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *output=AMediaCodec_getOutputFormat(codec);
            if (output) {
                AMediaFormat_getInt32(output,AMEDIAFORMAT_KEY_SAMPLE_RATE,&sample_rate);
                AMediaFormat_getInt32(output,AMEDIAFORMAT_KEY_CHANNEL_COUNT,&channels);
                AMediaFormat_getInt32(output,"pcm-encoding",&encoding);AMediaFormat_delete(output);
            }
            progress=true;
        } else if (index>=0) {
            size_t cap=0;uint8_t *output=AMediaCodec_getOutputBuffer(codec,(size_t)index,&cap);
            const size_t sample_bytes=encoding==4 ? 4u : 2u;
            bool valid=sample_rate>0 && sample_rate<=384000 && channels>0 && channels<=8 &&
                       (encoding==2 || encoding==4) && info.offset>=0 && info.size>=0 &&
                       (size_t)info.offset<=cap && (size_t)info.size<=cap-(size_t)info.offset &&
                       (!info.size || output!=NULL) && (size_t)info.size%((size_t)channels*sample_bytes)==0;
            if (valid && info.size>0 && info.presentationTimeUs>=0 &&
                info.presentationTimeUs<=duration_us+250000LL) {
                const size_t frames=(size_t)info.size/((size_t)channels*sample_bytes);
                for (size_t i=0;i<frames;i++) {
                    const int64_t delta=(int64_t)((uint64_t)i*1000000u/(uint64_t)sample_rate);
                    const int64_t pts=info.presentationTimeUs+delta;
                    for (int ch=0;ch<channels;ch++) {
                        const uint8_t *sample=output+(size_t)info.offset+(i*(size_t)channels+(size_t)ch)*sample_bytes;
                        float level;
                        if (encoding==4) { float v;memcpy(&v,sample,4);level=isfinite(v) ? fminf(1.0f,fabsf(v)) : 0.0f; }
                        else { int16_t v;memcpy(&v,sample,2);level=fabsf((float)v)/32768.0f; }
                        fv_waveform_accumulate_sample(peaks,pts,duration_us,level);
                    }
                }
                samples+=frames;
            }
            output_eos=(info.flags&AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)!=0;
            AMediaCodec_releaseOutputBuffer(codec,(size_t)index,false);
            if (!valid) goto done;
            progress=true;
        } else if (index!=AMEDIACODEC_INFO_TRY_AGAIN_LATER && index!=AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) goto done;
        idle=progress ? 0 : idle+1;
    }
    ok=output_eos && samples>0 && !cancelled(job);
    if (ok) fv_waveform_normalize(peaks);
done:
    if (codec) { AMediaCodec_stop(codec); AMediaCodec_delete(codec); }
    if (extractor) AMediaExtractor_delete(extractor);
    close(fd);return ok;
}
static void *analyze_waveform(void *arg) {
    const WaveformJob *job=arg;float peaks[FV_PLAYBACK_BINS]={0};
    JNIEnv *env=NULL;
    const bool attached=p.vm && (*p.vm)->AttachCurrentThread(p.vm,&env,NULL)==JNI_OK;
    const size_t n=strlen(job->path);
    const bool wav=n>=4 && strcmp(job->path+n-4,".wav")==0;
    const bool ok=wav ? fv_wav_waveform(job->path,peaks,&job->cancel) : compressed_waveform(job,peaks);
    if (attached && p.vm) (*p.vm)->DetachCurrentThread(p.vm);
    pthread_mutex_lock(&p.mutex);
    if (!cancelled(job) && job->generation==p.generation) {
        if (ok) {
            memcpy(p.state.waveform,peaks,sizeof(peaks));
            p.state.waveform_ready=true;
        } else {
            p.state.waveform_unavailable=true;
            LOGI("Waveform unavailable for local recording");
        }
    }
    pthread_mutex_unlock(&p.mutex);
    return NULL;
}
static void stop_analysis(void) {
    if (p.analysis.started) {
        atomic_store(&p.analysis.cancel,true);
        pthread_join(p.analysis.thread,NULL);p.analysis.started=false;
    }
}
static void video_image(AImageReader *reader) {
    AImage *image=NULL;
    if (!reader || AImageReader_acquireLatestImage(reader,&image)!=AMEDIA_OK || !image) return;
    pthread_mutex_lock(&p.mutex);
    if (p.pixels && fv_media_image_to_argb(image,p.pixels,p.pixel_w,p.pixel_h)) p.state.video_frames++;
    pthread_mutex_unlock(&p.mutex);
    AImage_delete(image);
}
static void release_media(JNIEnv *env,const PlayerMethods *m,jobject *player,AImageReader **reader) {
    stop_analysis();
    if (*player) {
        (*env)->CallVoidMethod(env,*player,m->release);fv_jni_check(env,"FieldVoxPlayer","release",true);
        (*env)->DeleteGlobalRef(env,*player);*player=NULL;
    }
    if (*reader) { AImageReader_delete(*reader);*reader=NULL; }
    pthread_mutex_lock(&p.mutex);
    free(p.pixels);p.pixels=NULL;p.pixel_w=p.pixel_h=0;
    pthread_mutex_unlock(&p.mutex);
}
static bool open_media(JNIEnv *env,const PlayerMethods *m,const char *path,uint64_t generation,jobject *player,AImageReader **reader) {
    struct stat st;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return false;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0) { close(fd);return false; }
    jobject descriptor=(*env)->CallStaticObjectMethod(env,m->pfd_cls,m->adopt_fd,fd);
    if (!fv_jni_check(env,"FieldVoxPlayer","adopt local file",true) || !descriptor) { close(fd);return false; }
    jobject local=(*env)->NewObject(env,m->cls,m->ctor);
    bool ok=fv_jni_check(env,"FieldVoxPlayer","create player",true) && local;
    if (ok) {
        *player=(*env)->NewGlobalRef(env,local);(*env)->DeleteLocalRef(env,local);
        ok=*player!=NULL && fv_jni_check(env,"FieldVoxPlayer","retain player",true);
    }
    jobject file_descriptor=ok ? (*env)->CallObjectMethod(env,descriptor,m->get_fd) : NULL;
    if (!fv_jni_check(env,"FieldVoxPlayer","get local descriptor",true) || !file_descriptor) ok=false;
    if (ok) { (*env)->CallVoidMethod(env,*player,m->set_source,file_descriptor,(jlong)0,(jlong)st.st_size);ok=fv_jni_check(env,"FieldVoxPlayer","load local file",true); }
    if (ok) { (*env)->CallVoidMethod(env,*player,m->prepare);ok=fv_jni_check(env,"FieldVoxPlayer","prepare local file",true); }
    (*env)->CallVoidMethod(env,descriptor,m->close_fd);if (!fv_jni_check(env,"FieldVoxPlayer","close local descriptor",true)) ok=false;
    if (file_descriptor) (*env)->DeleteLocalRef(env,file_descriptor);
    (*env)->DeleteLocalRef(env,descriptor);
    if (!ok) return false;
    const int duration=(*env)->CallIntMethod(env,*player,m->duration);
    const int width=(*env)->CallIntMethod(env,*player,m->width),height=(*env)->CallIntMethod(env,*player,m->height);
    if (!fv_jni_check(env,"FieldVoxPlayer","read local media format",true) || duration<=0 || width<0 || height<0 || width>8192 || height>8192) return false;
    if (width>0 && height>0) {
        if (AImageReader_new(width,height,AIMAGE_FORMAT_YUV_420_888,4,reader)!=AMEDIA_OK) return false;
        ANativeWindow *window=NULL;
        if (AImageReader_getWindow(*reader,&window)!=AMEDIA_OK || !window) return false;
        jobject surface=ANativeWindow_toSurface(env,window);
        if (!fv_jni_check(env,"FieldVoxPlayer","create playback surface",true) || !surface) return false;
        (*env)->CallVoidMethod(env,*player,m->surface,surface);ok=fv_jni_check(env,"FieldVoxPlayer","set playback surface",true);
        (*env)->DeleteLocalRef(env,surface);if (!ok) return false;
        const float scale=fminf(1.0f,1920.0f/(float)(width>height ? width : height));
        const int dw=(int)((float)width*scale),dh=(int)((float)height*scale);
        if (dw<=0 || dh<=0) return false;
        uint32_t *pixels=calloc((size_t)dw*(size_t)dh,sizeof(*pixels));if (!pixels) return false;
        pthread_mutex_lock(&p.mutex);p.pixels=pixels;p.pixel_w=dw;p.pixel_h=dh;pthread_mutex_unlock(&p.mutex);
    }
    FvTimeline timeline={0};
    const bool marks_ok=fv_timeline_load(path,&timeline);
    while (timeline.count && timeline.markers[timeline.count-1].ms>duration) timeline.count--;
    pthread_mutex_lock(&p.mutex);
    if (generation!=p.generation || p.pending_close) { pthread_mutex_unlock(&p.mutex);return false; }
    p.state.ready=true;p.state.duration_ms=duration;p.state.video=width>0 && height>0;
    p.state.timeline=timeline;
    p.timeline_readable=marks_ok;
    snprintf(p.state.status,sizeof(p.state.status),"%s",marks_ok ? "" : "Timeline could not be read");
    snprintf(p.analysis.path,sizeof(p.analysis.path),"%s",path);p.analysis.generation=p.generation;
    p.analysis.fallback_duration_us=(int64_t)duration*1000LL;
    pthread_mutex_unlock(&p.mutex);
    atomic_store(&p.analysis.cancel,false);
    p.analysis.started=pthread_create(&p.analysis.thread,NULL,analyze_waveform,&p.analysis)==0;
    if (!p.analysis.started) {
        pthread_mutex_lock(&p.mutex);
        if (generation==p.generation) p.state.waveform_unavailable=true;
        pthread_mutex_unlock(&p.mutex);
    }
    LOGI("Prepared local recording: %s duration=%d ms video=%dx%d",path,duration,width,height);
    return true;
}
static void player_error(const char *message) {
    pthread_mutex_lock(&p.mutex);
    p.state.error=true;p.state.ready=false;p.state.playing=false;p.desired_playing=false;
    snprintf(p.state.status,sizeof(p.state.status),"%s",message);pthread_mutex_unlock(&p.mutex);
}
static void *player_worker(void *arg) {
    (void)arg;JNIEnv *env=NULL;PlayerMethods m={0};jobject player=NULL;AImageReader *reader=NULL;
    if ((*p.vm)->AttachCurrentThread(p.vm,&env,NULL)!=JNI_OK || !env) {
        pthread_mutex_lock(&p.mutex);p.backend_failed=true;pthread_mutex_unlock(&p.mutex);
        player_error("Android media service unavailable");return NULL;
    }
    if (!methods(env,&m)) {
        pthread_mutex_lock(&p.mutex);p.backend_failed=true;pthread_mutex_unlock(&p.mutex);
        player_error("Android media APIs unavailable");goto done;
    }
    bool was_playing=false,was_muted=false;
    while (atomic_load(&p.running)) {
        pthread_mutex_lock(&p.mutex);
        const bool load=p.pending_open,close=p.pending_close;
        const uint64_t generation=p.generation;
        char path[512];snprintf(path,sizeof(path),"%s",p.path);
        p.pending_open=p.pending_close=false;
        pthread_mutex_unlock(&p.mutex);
        if (load || close) {
            release_media(env,&m,&player,&reader);was_playing=false;was_muted=false;
            if (close && !load) {
                pthread_mutex_lock(&p.mutex);p.state.visible=false;p.state.ready=false;p.state.playing=false;pthread_mutex_unlock(&p.mutex);
            } else if (!open_media(env,&m,path,generation,&player,&reader)) {
                release_media(env,&m,&player,&reader);
                pthread_mutex_lock(&p.mutex);const bool current=generation==p.generation && !p.pending_close;pthread_mutex_unlock(&p.mutex);
                if (current) player_error("Cannot load this recording • missing, damaged or unsupported file");
            }
        }
        if (player) {
            pthread_mutex_lock(&p.mutex);
            if (generation!=p.generation || p.pending_close) {
                pthread_mutex_unlock(&p.mutex);
                continue;
            }
            const bool desired=p.desired_playing,muted=p.desired_muted,mark=p.add_mark;
            const int64_t seek=p.pending_seek;p.pending_seek=-1;p.add_mark=false;
            const int64_t duration=p.state.duration_ms;
            pthread_mutex_unlock(&p.mutex);
            if (muted!=was_muted) { const jfloat volume=muted ? 0.0f : 1.0f;(*env)->CallVoidMethod(env,player,m.volume,volume,volume);was_muted=muted; }
            if (seek>=0) {
                (*env)->CallVoidMethod(env,player,m.seek,(jlong)fv_playback_clamp_seek(seek,duration),(jint)3);
                LOGI("Seek local recording: %lld ms",(long long)seek);
            }
            if (desired!=was_playing) {
                (*env)->CallVoidMethod(env,player,desired ? m.start : m.pause);was_playing=desired;
                LOGI("Local playback %s",desired ? "playing" : "paused");
            }
            const int position=(*env)->CallIntMethod(env,player,m.position);
            const bool playing=(*env)->CallBooleanMethod(env,player,m.playing)==JNI_TRUE;
            if (!fv_jni_check(env,"FieldVoxPlayer","playback controls",true)) {
                release_media(env,&m,&player,&reader);player_error("Playback failed • reopen the recording");
            } else {
                FvTimeline revised;
                bool save=false;
                pthread_mutex_lock(&p.mutex);
                if (generation!=p.generation || p.pending_close) {
                    pthread_mutex_unlock(&p.mutex);
                    continue;
                }
                p.state.position_ms=fv_playback_clamp_seek(position,duration);
                p.state.playing=playing;p.state.muted=muted;
                if (playing && seek<0 && p.pending_seek<0 && p.marker_target_ms>=0 &&
                    p.state.position_ms>p.marker_target_ms+250)
                    p.marker_target_ms=-1;
                if (!playing && desired && seek<0 && p.pending_seek<0 &&
                    duration-p.state.position_ms<=100) {
                    p.desired_playing=false;
                    was_playing=false;
                    queue_seek_locked(-1,false);
                    LOGI("Local playback completed");
                }
                if (mark) {
                    revised=p.state.timeline;
                    save=fv_timeline_add(&revised,p.state.position_ms,FV_MARK_BOOKMARK);
                    if (!save) snprintf(p.state.status,sizeof(p.state.status),"Timeline is full");
                }
                pthread_mutex_unlock(&p.mutex);
                if (save) {
                    const bool saved=fv_timeline_save(path,&revised);
                    pthread_mutex_lock(&p.mutex);
                    if (generation==p.generation && !p.pending_close) {
                        if (saved) {
                            p.state.timeline=revised;
                            p.state.status[0]='\0';
                        } else {
                            snprintf(p.state.status,sizeof(p.state.status),"Bookmark save failed • check free storage");
                        }
                    }
                    pthread_mutex_unlock(&p.mutex);
                    if (saved) LOGI("Local bookmark saved: %d ms",position);
                }
                video_image(reader);
            }
        }
        const struct timespec delay={.tv_nsec=16000000};nanosleep(&delay,NULL);
    }
    release_media(env,&m,&player,&reader);
done:
    if (m.cls) (*env)->DeleteLocalRef(env,m.cls);
    if (m.pfd_cls) (*env)->DeleteLocalRef(env,m.pfd_cls);
    (*p.vm)->DetachCurrentThread(p.vm);return NULL;
}
bool fv_local_player_init(JavaVM *vm) {
    if (p.started) return true;
    if (!vm) return false;
    p.vm=vm;p.backend_failed=false;atomic_store(&p.running,true);
    p.started=pthread_create(&p.thread,NULL,player_worker,NULL)==0;
    return p.started;
}
void fv_local_player_shutdown(void) {
    if (p.started) { atomic_store(&p.running,false);pthread_join(p.thread,NULL);p.started=false; }
    pthread_mutex_lock(&p.mutex);p.state=(FvLocalPlayerSnapshot){0};p.pending_open=p.pending_close=false;queue_seek_locked(-1,false);pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_open(const char *path,bool muted) {
    if (!path || path[0]!='/' || strlen(path)>=sizeof(p.path)) return;
    pthread_mutex_lock(&p.mutex);
    p.generation++;p.state=(FvLocalPlayerSnapshot){.visible=true,.muted=muted};
    snprintf(p.path,sizeof(p.path),"%s",path);
    const char *name=strrchr(path,'/');snprintf(p.state.name,sizeof(p.state.name),"%s",name ? name+1 : path);
    snprintf(p.state.status,sizeof(p.state.status),"Loading local recording…");
    p.pending_open=true;p.pending_close=false;queue_seek_locked(-1,false);p.add_mark=false;
    p.desired_playing=true;p.desired_muted=muted;
    if (!p.started || p.backend_failed) { p.state.error=true;p.pending_open=false;snprintf(p.state.status,sizeof(p.state.status),"Local player could not start"); }
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_close(void) {
    pthread_mutex_lock(&p.mutex);p.pending_close=true;p.pending_open=false;p.desired_playing=false;pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_pause(void) {
    pthread_mutex_lock(&p.mutex);p.desired_playing=false;p.scrub_resume=false;pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_toggle(void) {
    pthread_mutex_lock(&p.mutex);
    if (p.state.ready) {
        if (!p.desired_playing && p.state.position_ms>=p.state.duration_ms-100)
            queue_seek_locked(0,false);
        p.desired_playing=!p.desired_playing;
    }
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_mute(void) { pthread_mutex_lock(&p.mutex);p.desired_muted=!p.desired_muted;pthread_mutex_unlock(&p.mutex); }
void fv_local_player_seek(int64_t ms) {
    pthread_mutex_lock(&p.mutex);
    if (p.state.ready) {
        queue_seek_locked(fv_playback_clamp_seek(ms,p.state.duration_ms),false);
    }
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_seek_delta(int64_t delta_ms) {
    pthread_mutex_lock(&p.mutex);
    if (p.state.ready) {
        const int64_t base = p.pending_seek >= 0 ? p.pending_seek : p.state.position_ms;
        queue_seek_locked(fv_playback_clamp_seek(base + delta_ms,p.state.duration_ms),false);
    }
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_scrub_begin(void) {
    pthread_mutex_lock(&p.mutex);p.scrub_resume=p.desired_playing;p.desired_playing=false;pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_scrub_end(void) {
    pthread_mutex_lock(&p.mutex);p.desired_playing=p.scrub_resume;p.scrub_resume=false;pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_bookmark(void) {
    pthread_mutex_lock(&p.mutex);
    p.add_mark=p.state.ready && p.timeline_readable;
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_jump_mark(bool forward) {
    pthread_mutex_lock(&p.mutex);
    if (p.state.ready) {
        bool selected;
        const int64_t base=marker_position_locked(&selected);
        const FvTimelineMarker *marker=fv_timeline_step(&p.state.timeline,base,forward);
        if (marker) {
            queue_seek_locked(marker->ms,true);
            p.state.status[0]='\0';
        }
    }
    pthread_mutex_unlock(&p.mutex);
}
bool fv_local_player_active(void) { pthread_mutex_lock(&p.mutex);const bool active=p.state.visible;pthread_mutex_unlock(&p.mutex);return active; }
void fv_local_player_snapshot(FvLocalPlayerSnapshot *out) {
    if (!out) return;
    pthread_mutex_lock(&p.mutex);
    *out=p.state;
    if (out->timeline.count) {
        bool selected;
        const int64_t position=marker_position_locked(&selected);
        const FvTimelineMarker *last=fv_timeline_step(&out->timeline,position+1,false);
        out->cue=last ? *last : out->timeline.markers[0];
        out->cue_type=selected ? FV_CUE_SELECTED : (last ? FV_CUE_LAST : FV_CUE_NEXT);
    }
    pthread_mutex_unlock(&p.mutex);
}
void fv_local_player_draw_video(FvCanvas *c) {
    if (!c) return;
    pthread_mutex_lock(&p.mutex);
    if (p.pixels && p.state.video_frames && p.pixel_w>0 && p.pixel_h>0) {
        fvg_blit_cover(c,p.pixels,p.pixel_w,p.pixel_h);
    }
    pthread_mutex_unlock(&p.mutex);
}
