/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fv_native_radio.h"
#include "fv_fragment_engine.h"
#include "fv_station_policy.h"
#include "fv_station_blob_data.h"
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"FieldVoxRadio",__VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,"FieldVoxRadio",__VA_ARGS__)
#define FETCH_BYTES 50000u
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    JavaVM *vm;
    bool started,enabled,queued,busy,ready;
    unsigned generation,job_generation,group[FV_STATION_GROUP_SIZE];
    size_t group_count;
    int station,result_station,result_fd;
    FvVoxDecimalTrigger trigger,result_trigger;
    int16_t *pcm;
    size_t frames;
    char directory[512];
} radio={.mutex=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.result_fd=-1};
static long now_ms(void) {
    struct timespec ts; if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) return 0;
    return ts.tv_sec*1000L+ts.tv_nsec/1000000L;
}
static bool current(unsigned generation) {
    pthread_mutex_lock(&radio.mutex);
    const bool ok=radio.enabled && radio.generation==generation;
    pthread_mutex_unlock(&radio.mutex); return ok;
}
static const FvBlobStationEntry *entry_at(int station) {
    if(station<0 || (unsigned)station>=FV_STATION_BLOB_COUNT) return NULL;
    return (const FvBlobStationEntry *)(FV_STATION_PLAN_BINARY_DATA+16u+(size_t)station*sizeof(FvBlobStationEntry));
}
/* Calls platform HTTP classes from C23. All local references and exceptions are
 * confined to this request; connect/read deadlines also bound lifecycle cancellation. */
static bool download(JNIEnv *env,const char *url,FILE *file,unsigned generation) {
    if((*env)->PushLocalFrame(env,24)!=JNI_OK) {
        if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return false;
    }
    bool ok=false; jobject connection=NULL,input=NULL;
    jclass url_class=(*env)->FindClass(env,"java/net/URL");
    jclass http_class=(*env)->FindClass(env,"java/net/HttpURLConnection");
    jclass input_class=(*env)->FindClass(env,"java/io/InputStream");
    if(!url_class || !http_class || !input_class) goto done;
    jmethodID constructor=(*env)->GetMethodID(env,url_class,"<init>","(Ljava/lang/String;)V");
    jmethodID open=(*env)->GetMethodID(env,url_class,"openConnection","()Ljava/net/URLConnection;");
    jmethodID connect_timeout=(*env)->GetMethodID(env,http_class,"setConnectTimeout","(I)V");
    jmethodID read_timeout=(*env)->GetMethodID(env,http_class,"setReadTimeout","(I)V");
    jmethodID header=(*env)->GetMethodID(env,http_class,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID response=(*env)->GetMethodID(env,http_class,"getResponseCode","()I");
    jmethodID get_input=(*env)->GetMethodID(env,http_class,"getInputStream","()Ljava/io/InputStream;");
    jmethodID read=(*env)->GetMethodID(env,input_class,"read","([BII)I");
    if((*env)->ExceptionCheck(env)) goto done;
    jstring address=(*env)->NewStringUTF(env,url);
    if(!address) goto done;
    jobject object=(*env)->NewObject(env,url_class,constructor,address);
    if(!object || (*env)->ExceptionCheck(env)) goto done;
    connection=(*env)->CallObjectMethod(env,object,open);
    if(!connection || (*env)->ExceptionCheck(env) || !(*env)->IsInstanceOf(env,connection,http_class)) goto done;
    (*env)->CallVoidMethod(env,connection,connect_timeout,5000);
    (*env)->CallVoidMethod(env,connection,read_timeout,5000);
    jstring key=(*env)->NewStringUTF(env,"User-Agent"),value=(*env)->NewStringUTF(env,"FieldVoxLab/0.5.2");
    if(!key || !value || (*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env,connection,header,key,value);
    if((*env)->ExceptionCheck(env)) goto done;
    const jint code=(*env)->CallIntMethod(env,connection,response);
    if((*env)->ExceptionCheck(env) || code<200 || code>=300 || !current(generation)) goto done;
    input=(*env)->CallObjectMethod(env,connection,get_input);
    if(!input || (*env)->ExceptionCheck(env)) goto done;
    jbyteArray bytes=(*env)->NewByteArray(env,4096); if(!bytes) goto done;
    size_t total=0; const long started=now_ms();
    while(total<FETCH_BYTES && current(generation) && now_ms()-started<5000L) {
        const jint wanted=(jint)(FETCH_BYTES-total>4096u ? 4096u : FETCH_BYTES-total);
        const jint n=(*env)->CallIntMethod(env,input,read,bytes,0,wanted);
        if((*env)->ExceptionCheck(env) || n<0) break;
        if(!n) continue;
        if(n>wanted) break;
        jbyte buffer[4096]; (*env)->GetByteArrayRegion(env,bytes,0,n,buffer);
        if((*env)->ExceptionCheck(env) || fwrite(buffer,1,(size_t)n,file)!=(size_t)n) goto done;
        total+=(size_t)n;
    }
    ok=total>0u && current(generation) && fflush(file)==0;
    LOGI("clip fetch: %zu bytes in %ld ms",total,now_ms()-started);
done:
    if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if(input && input_class) {
        jmethodID close=(*env)->GetMethodID(env,input_class,"close","()V");
        if(close) (*env)->CallVoidMethod(env,input,close);
        if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if(connection && http_class) {
        jmethodID disconnect=(*env)->GetMethodID(env,http_class,"disconnect","()V");
        if(disconnect) (*env)->CallVoidMethod(env,connection,disconnect);
        if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env,NULL); return ok;
}
static size_t append_pcm(int16_t *pcm,size_t count,const uint8_t *data,size_t bytes,
                         int rate,int channels,int encoding,double *phase,float *previous) {
    if(rate<4000 || rate>192000 || channels<1 || channels>32 || (encoding!=2 && encoding!=4)) return count;
    const size_t width=encoding==4 ? sizeof(float) : sizeof(int16_t);
    const size_t frames=bytes/(width*(size_t)channels);
    const double step=(double)rate/(double)FV_FRAGMENT_RATE;
    for(size_t i=0;i<frames && count<FV_FRAGMENT_CLIP_FRAMES;i++) {
        float mono=0.0f;
        for(int c=0;c<channels;c++) {
            const uint8_t *at=data+(i*(size_t)channels+(size_t)c)*width;
            if(encoding==4) { float value;memcpy(&value,at,width);mono+=isfinite(value)?value:0.0f; }
            else { int16_t value;memcpy(&value,at,width);mono+=(float)value/32768.0f; }
        }
        mono/=(float)channels;
        while(*phase<=1.0 && count<FV_FRAGMENT_CLIP_FRAMES) {
            const float sample=*previous+(mono-*previous)*(float)*phase;
            pcm[count++]=(int16_t)(fmaxf(-1.0f,fminf(1.0f,sample))*32767.0f);
            *phase+=step;
        }
        *phase-=1.0;*previous=mono;
    }
    return count;
}
static int16_t *decode(const char *path,unsigned generation,size_t *frames) {
    AMediaExtractor *extractor=AMediaExtractor_new(); AMediaCodec *codec=NULL;
    AMediaFormat *format=NULL; int16_t *pcm=NULL; *frames=0;
    if(!extractor) return NULL;
    const int fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0) goto done;
    const off_t bytes=lseek(fd,0,SEEK_END);
    const media_status_t status=AMediaExtractor_setDataSourceFd(extractor,fd,0,bytes); close(fd);
    if(status!=AMEDIA_OK) goto done;
    const size_t tracks=AMediaExtractor_getTrackCount(extractor); int track=-1;char mime[80]={0};
    for(size_t i=0;i<tracks;i++) {
        AMediaFormat *candidate=AMediaExtractor_getTrackFormat(extractor,i); const char *type=NULL;
        if(candidate && AMediaFormat_getString(candidate,AMEDIAFORMAT_KEY_MIME,&type) && type && strncmp(type,"audio/",6)==0) {
            format=candidate;track=(int)i;snprintf(mime,sizeof(mime),"%s",type);break;
        }
        if(candidate) AMediaFormat_delete(candidate);
    }
    if(track<0) goto done;
    codec=AMediaCodec_createDecoderByType(mime);
    if(!codec || AMediaCodec_configure(codec,format,NULL,NULL,0)!=AMEDIA_OK || AMediaCodec_start(codec)!=AMEDIA_OK) goto done;
    pcm=malloc(FV_FRAGMENT_CLIP_FRAMES*sizeof(*pcm)); if(!pcm) goto done;
    AMediaExtractor_selectTrack(extractor,(size_t)track);
    int32_t rate=48000,channels=1,encoding=2; double phase=0.0;float previous=0.0f;
    AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_SAMPLE_RATE,&rate);
    AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_CHANNEL_COUNT,&channels);
    bool input_eos=false; const long started=now_ms();
    while(current(generation) && now_ms()-started<5000L && *frames<FV_FRAGMENT_CLIP_FRAMES) {
        if(!input_eos) {
            const ssize_t index=AMediaCodec_dequeueInputBuffer(codec,10000);
            if(index>=0) {
                size_t capacity=0;uint8_t *buffer=AMediaCodec_getInputBuffer(codec,(size_t)index,&capacity);
                const ssize_t n=buffer ? AMediaExtractor_readSampleData(extractor,buffer,capacity) : -1;
                input_eos=n<0;
                AMediaCodec_queueInputBuffer(codec,(size_t)index,0,n<0 ? 0u : (size_t)n,
                    n<0 ? 0u : (uint64_t)AMediaExtractor_getSampleTime(extractor),
                    n<0 ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0u);
                if(n>=0) AMediaExtractor_advance(extractor);
            }
        }
        AMediaCodecBufferInfo info;const ssize_t index=AMediaCodec_dequeueOutputBuffer(codec,&info,10000);
        if(index>=0) {
            size_t capacity=0;const uint8_t *buffer=AMediaCodec_getOutputBuffer(codec,(size_t)index,&capacity);
            if(buffer && info.offset>=0 && info.size>0 && (size_t)info.offset<=capacity && (size_t)info.size<=capacity-(size_t)info.offset)
                *frames=append_pcm(pcm,*frames,buffer+info.offset,(size_t)info.size,rate,channels,encoding,&phase,&previous);
            AMediaCodec_releaseOutputBuffer(codec,(size_t)index,false);
            if(info.flags&AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else if(index==AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *output=AMediaCodec_getOutputFormat(codec);
            if(output) {
                AMediaFormat_getInt32(output,AMEDIAFORMAT_KEY_SAMPLE_RATE,&rate);
                AMediaFormat_getInt32(output,AMEDIAFORMAT_KEY_CHANNEL_COUNT,&channels);
                AMediaFormat_getInt32(output,AMEDIAFORMAT_KEY_PCM_ENCODING,&encoding);AMediaFormat_delete(output);
            }
        } else if(index!=AMEDIACODEC_INFO_TRY_AGAIN_LATER && index!=AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) break;
    }
done:
    if(codec) { AMediaCodec_stop(codec);AMediaCodec_delete(codec); }
    if(format) AMediaFormat_delete(format);
    AMediaExtractor_delete(extractor);
    if(!*frames || !current(generation)) { free(pcm);pcm=NULL;*frames=0; }
    return pcm;
}
static void *worker(void *unused) {
    (void)unused;JNIEnv *env=NULL;
    if((*radio.vm)->AttachCurrentThread(radio.vm,&env,NULL)!=JNI_OK) {
        pthread_mutex_lock(&radio.mutex);
        radio.started=false;radio.busy=false;
        if(radio.queued && radio.enabled && radio.job_generation==radio.generation) {
            radio.result_station=radio.station;radio.result_trigger=radio.trigger;radio.ready=true;
        }
        radio.queued=false;
        pthread_mutex_unlock(&radio.mutex);return NULL;
    }
    for(;;) {
        pthread_mutex_lock(&radio.mutex);
        while(!radio.queued) pthread_cond_wait(&radio.changed,&radio.mutex);
        const int station=radio.station;const unsigned generation=radio.job_generation;
        const FvVoxDecimalTrigger trigger=radio.trigger;radio.queued=false;
        char path[560];snprintf(path,sizeof(path),"%s/radio-slice-XXXXXX",radio.directory);
        pthread_mutex_unlock(&radio.mutex);
        int16_t *pcm=NULL;size_t frames=0;
        const FvBlobStationEntry *entry=entry_at(station);
        const int fd=mkstemp(path);FILE *file=fd>=0 ? fdopen(fd,"wb") : NULL;
        if(fd>=0 && !file) close(fd);
        if(file && entry) {
            char url[161];memcpy(url,entry->url,160);url[160]=0;
            const bool fetched=download(env,url,file,generation);
            const bool closed=fclose(file)==0;file=NULL;
            if(fetched && closed) pcm=decode(path,generation,&frames);
        }
        if(file) fclose(file);
        const int source_fd=pcm ? open(path,O_RDONLY|O_CLOEXEC) : -1;
        if(fd>=0) unlink(path);
        LOGI("clip result: station=%d frames=%zu duration=%.3fs",station,frames,(double)frames/48000.0);
        pthread_mutex_lock(&radio.mutex);
        radio.busy=false;
        if(radio.enabled && radio.generation==generation) {
            radio.pcm=pcm;radio.result_fd=source_fd;radio.frames=frames;radio.result_station=station;radio.result_trigger=trigger;radio.ready=true;
        } else { free(pcm); if(source_fd>=0) close(source_fd); }
        pthread_mutex_unlock(&radio.mutex);
    }
    return NULL;
}
bool fv_radio_start(JavaVM *vm,const char *app_directory,const char *language,int station_count) {
    if(!vm || !app_directory || !language || station_count<1 || station_count>(int)FV_STATION_BLOB_COUNT) return false;
    pthread_mutex_lock(&radio.mutex);
    if(radio.started) { pthread_mutex_unlock(&radio.mutex);return true; }
    const int n=snprintf(radio.directory,sizeof(radio.directory),"%s",app_directory);
    if(n<0 || (size_t)n>=sizeof(radio.directory)) { pthread_mutex_unlock(&radio.mutex);return false; }
    char languages[FV_STATION_BLOB_COUNT][8];
    for(int i=0;i<station_count;i++) memcpy(languages[i],entry_at(i)->lang,8);
    radio.group_count=fv_station_group(languages,(size_t)station_count,language,(uint32_t)now_ms(),radio.group);
    radio.vm=vm;pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    radio.started=pthread_create(&thread,&attr,worker,NULL)==0;pthread_attr_destroy(&attr);
    const bool started=radio.started;
    LOGI("on-demand radio: language=%s group=%zu",language,radio.group_count);
    for(size_t i=0;i<radio.group_count;i++) LOGI("source %zu: station=%u %s",i,radio.group[i],entry_at((int)radio.group[i])->callsign);
    pthread_mutex_unlock(&radio.mutex);return started;
}
void fv_radio_set_enabled(bool enabled) {
    pthread_mutex_lock(&radio.mutex);
    if(radio.enabled!=enabled) {
        radio.enabled=enabled;radio.generation++;
        free(radio.pcm);radio.pcm=NULL;radio.frames=0;radio.ready=false;
        if(radio.result_fd>=0) close(radio.result_fd);radio.result_fd=-1;
    }
    pthread_mutex_unlock(&radio.mutex);
}
int fv_radio_selected_station(int channel) {
    pthread_mutex_lock(&radio.mutex);
    const int station=channel>=0 && radio.group_count ? (int)radio.group[(size_t)(unsigned)channel%radio.group_count] : -1;
    pthread_mutex_unlock(&radio.mutex);return station;
}
bool fv_radio_request(int channel,const FvVoxDecimalTrigger *trigger) {
    if(!trigger || channel<0) return false;
    pthread_mutex_lock(&radio.mutex);
    const bool ok=radio.started && radio.enabled && !radio.busy && !radio.ready && radio.group_count;
    if(ok) {
        radio.station=(int)radio.group[(size_t)(unsigned)channel%radio.group_count];radio.trigger=*trigger;
        radio.job_generation=radio.generation;radio.queued=radio.busy=true;pthread_cond_signal(&radio.changed);
    }
    pthread_mutex_unlock(&radio.mutex);return ok;
}
bool fv_radio_take_result(int16_t **pcm,size_t *frames,int *station,FvVoxDecimalTrigger *trigger,int *source_fd) {
    if(!pcm || !frames || !station || !trigger || !source_fd) return false;
    pthread_mutex_lock(&radio.mutex);const bool ready=radio.ready;
    if(ready) {
        *pcm=radio.pcm;*source_fd=radio.result_fd;radio.result_fd=-1;*frames=radio.frames;*station=radio.result_station;*trigger=radio.result_trigger;
        radio.pcm=NULL;radio.frames=0;radio.ready=false;
    }
    pthread_mutex_unlock(&radio.mutex);return ready;
}
bool fv_radio_busy(void) {
    pthread_mutex_lock(&radio.mutex);const bool busy=radio.busy;pthread_mutex_unlock(&radio.mutex);return busy;
}
