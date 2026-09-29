#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * FieldVox Stage 1P-B Native Source Provenance + Timeline + Sweep Schedule Audit
 * GPL-3.0-or-later project code.
 *
 * Native C responsibilities:
 * - maintain a ring buffer of recent stream/sensor/gate/hit timeline events
 * - generate deterministic evidence-grade sweep scheduler records
 * - serialize per-hit source audit JSON with timeline_snapshot and sweep_engine
 * - write per-hit JSON, JSONL ledgers, slot catalog, scheduler manifest, and README
 *
 * Stage 1P-B does not alter audio playback yet. Java/Android still owns playback.
 * Native C records the deterministic schedule evidence layer first.
 *
 * Native C does not:
 * - generate words
 * - use a word bank
 * - use TTS
 * - open network connections
 * - use broad storage
 */

static const unsigned char FVSA_MAGIC_BLOB[] = {
    0x46, 0x56, 0x53, 0x41, 0x01, 0x1a, 0x43, 0x4a,
    0x4e, 0x49, 0x00, 0x1f, 0x0a, 0x7e, 0x5a, 0xc3
};

#define FVSA_TIMELINE_CAP 256
#define FVSA_TIMELINE_KIND_CAP 64
#define FVSA_TIMELINE_DETAIL_CAP 1024
#define FVSA_SWEEP_SLOT_COUNT 64

typedef struct FvsaTimelineEvent {
    long long unix_ms;
    unsigned long long seq;
    char kind[FVSA_TIMELINE_KIND_CAP];
    char detail[FVSA_TIMELINE_DETAIL_CAP];
} FvsaTimelineEvent;

typedef struct FvsaSweepSlot {
    int slot;
    const char *station_uuid;
    const char *station_name;
    const char *country_code;
    const char *language;
    const char *stream_url_hash;
    const char *source_label;
} FvsaSweepSlot;

typedef struct FvsaSweepEvent {
    unsigned long long schedule_seq;
    unsigned long long seed;
    long long unix_ms;
    int slot;
    int dwell_ms;
    int jitter_ms;
    const FvsaSweepSlot *slot_info;
} FvsaSweepEvent;

static pthread_mutex_t fvsa_timeline_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fvsa_sweep_mutex = PTHREAD_MUTEX_INITIALIZER;
static FvsaTimelineEvent fvsa_timeline[FVSA_TIMELINE_CAP];
static unsigned long long fvsa_timeline_seq = 0ULL;
static unsigned long long fvsa_sweep_seq = 0ULL;
static unsigned long long fvsa_sweep_state = 0x4656534153503141ULL; /* "FVSASP1A" */

static const FvsaSweepSlot fvsa_slots[FVSA_SWEEP_SLOT_COUNT] = {
    {0, "stage1p-a-slot-kiro", "KIRO Newsradio Seattle 97.3", "US", "en", "hash_kiro_2643_48_aac", "KIRO Newsradio Seattle 97.3"},
    {1, "stage1p-a-slot-nwnews", "Northwest Newsradio KOMO/KNWN", "US", "en", "hash_northwest_newsradio", "Northwest Newsradio KOMO/KNWN"},
    {2, "stage1p-a-slot-kuow", "KUOW Seattle 94.9", "US", "en", "hash_kuow", "KUOW Seattle 94.9"},
    {3, "stage1p-a-slot-bbc", "BBC World Service", "GB", "en", "hash_bbc_world_service", "BBC World Service"},
    {4, "stage1p-a-slot-wnyc", "WNYC AM 820 New York", "US", "en", "hash_wnyc_am", "WNYC AM 820 New York"},
    {5, "stage1p-a-slot-wbur", "WBUR 90.9 Boston", "US", "en", "hash_wbur", "WBUR 90.9 Boston"},
    {6, "stage1p-a-slot-wamu", "WAMU 88.5 Washington DC", "US", "en", "hash_wamu", "WAMU 88.5 Washington DC"},
    {7, "stage1p-a-slot-mpr", "Minnesota Public Radio News", "US", "en", "hash_mpr_news", "Minnesota Public Radio News"},
    {8, "stage1p-a-slot-vpr", "Vermont Public Radio", "US", "en", "hash_vpr", "Vermont Public Radio"},
    {9, "stage1p-a-slot-wbez", "WBEZ 91.5 Chicago", "US", "en", "hash_wbez", "WBEZ 91.5 Chicago"},
    {10, "stage1p-a-slot-opb", "Oregon Public Broadcasting OPB", "US", "en", "hash_opb", "Oregon Public Broadcasting OPB"},
    {11, "stage1p-a-slot-kcrw", "KCRW 89.9 Santa Monica", "US", "en", "hash_kcrw", "KCRW 89.9 Santa Monica"},
    {12, "stage1p-a-slot-wunc", "WUNC 91.5 North Carolina", "US", "en", "hash_wunc", "WUNC 91.5 North Carolina"},
    {13, "stage1p-a-slot-whyy", "WHYY 90.9 Philadelphia", "US", "en", "hash_whyy", "WHYY 90.9 Philadelphia"},
    {14, "stage1p-a-slot-kera", "KERA 90.1 North Texas", "US", "en", "hash_kera", "KERA 90.1 North Texas"},
    {15, "stage1p-a-slot-wlrn", "WLRN 91.3 South Florida", "US", "en", "hash_wlrn", "WLRN 91.3 South Florida"},
    {16, "stage1p-a-slot-kqed", "KQED 88.5 San Francisco", "US", "en", "hash_kqed", "KQED 88.5 San Francisco"},
    {17, "stage1p-a-slot-wpr", "Wisconsin Public Radio News", "US", "en", "hash_wpr", "Wisconsin Public Radio News"},
    {18, "stage1p-a-slot-ipr", "Iowa Public Radio News", "US", "en", "hash_ipr", "Iowa Public Radio News"},
    {19, "stage1p-a-slot-noaa", "NOAA Puget Sound Marine Weather", "US", "en", "hash_noaa", "NOAA Puget Sound Marine Weather"},
    {20, "stage1p-a-slot-cspan", "C-SPAN Radio Washington", "US", "en", "hash_cspan", "C-SPAN Radio Washington"},
    {21, "stage1p-a-slot-cbc", "CBC Radio One Toronto", "CA", "en", "hash_cbc", "CBC Radio One Toronto"},
    {22, "stage1p-a-slot-rte", "RTE Radio 1 Dublin", "IE", "en", "hash_rte", "RTE Radio 1 Dublin"},
    {23, "stage1p-a-slot-abc-au", "ABC NewsRadio Sydney", "AU", "en", "hash_abc_au", "ABC NewsRadio Sydney"},
    {24, "stage1p-a-slot-bbc4", "BBC Radio 4 London", "GB", "en", "hash_bbc4", "BBC Radio 4 London"},
    {25, "stage1p-a-slot-finfo", "France Info Paris", "FR", "fr", "hash_finfo", "France Info Paris"},
    {26, "stage1p-a-slot-dw", "Deutsche Welle English", "DE", "en", "hash_dw", "Deutsche Welle English"},
    {27, "stage1p-a-slot-nhk", "NHK World Radio Tokyo", "JP", "en", "hash_nhk", "NHK World Radio Tokyo"},
    {28, "stage1p-a-slot-rnz", "Radio New Zealand National", "NZ", "en", "hash_rnz", "Radio New Zealand National"},
    {29, "stage1p-a-slot-cpr", "Colorado Public Radio CPR", "US", "en", "hash_cpr", "Colorado Public Radio CPR"},
    {30, "stage1p-a-slot-tpr", "Texas Public Radio KSTX", "US", "en", "hash_tpr", "Texas Public Radio KSTX"},
    {31, "stage1p-a-slot-gpb", "Georgia Public Broadcasting GPB", "US", "en", "hash_gpb", "Georgia Public Broadcasting GPB"},
    {32, "stage1p-a-slot-wuom", "Michigan Radio WUOM", "US", "en", "hash_wuom", "Michigan Radio WUOM"},
    {33, "stage1p-a-slot-kwmu", "St. Louis Public Radio KWMU", "US", "en", "hash_kwmu", "St. Louis Public Radio KWMU"},
    {34, "stage1p-a-slot-kut", "KUT 90.5 Austin", "US", "en", "hash_kut", "KUT 90.5 Austin"},
    {35, "stage1p-a-slot-nhpr", "New Hampshire Public Radio NHPR", "US", "en", "hash_nhpr", "New Hampshire Public Radio NHPR"},
    {36, "stage1p-a-slot-maine", "Maine Public Radio", "US", "en", "hash_maine", "Maine Public Radio"},
    {37, "stage1p-a-slot-boise", "Boise State Public Radio", "US", "en", "hash_boise", "Boise State Public Radio"},
    {38, "stage1p-a-slot-wpr-wy", "Wyoming Public Radio", "US", "en", "hash_wpr_wy", "Wyoming Public Radio"},
    {39, "stage1p-a-slot-kufm", "Montana Public Radio KUFM", "US", "en", "hash_kufm", "Montana Public Radio KUFM"},
    {40, "stage1p-a-slot-kska", "Alaska Public Media KSKA", "US", "en", "hash_kska", "Alaska Public Media KSKA"},
    {41, "stage1p-a-slot-hpr", "Hawaii Public Radio HPR", "US", "en", "hash_hpr", "Hawaii Public Radio HPR"},
    {42, "stage1p-a-slot-kpcc", "KPCC 89.3 Pasadena", "US", "en", "hash_kpcc", "KPCC 89.3 Pasadena"},
    {43, "stage1p-a-slot-kalw", "KALW 91.7 San Francisco", "US", "en", "hash_kalw", "KALW 91.7 San Francisco"},
    {44, "stage1p-a-slot-knpr", "Nevada Public Radio KNPR", "US", "en", "hash_knpr", "Nevada Public Radio KNPR"},
    {45, "stage1p-a-slot-kuaz", "Arizona Public Media KUAZ", "US", "en", "hash_kuaz", "Arizona Public Media KUAZ"},
    {46, "stage1p-a-slot-kunm", "New Mexico Public Radio KUNM", "US", "en", "hash_kunm", "New Mexico Public Radio KUNM"},
    {47, "stage1p-a-slot-upr", "Utah Public Radio UPR", "US", "en", "hash_upr", "Utah Public Radio UPR"},
    {48, "stage1p-a-slot-npm", "Nebraska Public Media", "US", "en", "hash_npm", "Nebraska Public Media"},
    {49, "stage1p-a-slot-sdpb", "South Dakota Public Broadcasting", "US", "en", "hash_sdpb", "South Dakota Public Broadcasting"},
    {50, "stage1p-a-slot-ndpr", "North Dakota Public Radio Prairie", "US", "en", "hash_ndpr", "North Dakota Public Radio Prairie"},
    {51, "stage1p-a-slot-kpr", "Kansas Public Radio KPR", "US", "en", "hash_kpr", "Kansas Public Radio KPR"},
    {52, "stage1p-a-slot-kgou", "Oklahoma Public Media KGOU", "US", "en", "hash_kgou", "Oklahoma Public Media KGOU"},
    {53, "stage1p-a-slot-kuar", "Arkansas Public Media KUAR", "US", "en", "hash_kuar", "Arkansas Public Media KUAR"},
    {54, "stage1p-a-slot-wrkf", "Louisiana Public Broadcasting WRKF", "US", "en", "hash_wrkf", "Louisiana Public Broadcasting WRKF"},
    {55, "stage1p-a-slot-mpb", "Mississippi Public Broadcasting MPB", "US", "en", "hash_mpb", "Mississippi Public Broadcasting MPB"},
    {56, "stage1p-a-slot-apr", "Alabama Public Radio APR", "US", "en", "hash_apr", "Alabama Public Radio APR"},
    {57, "stage1p-a-slot-wpln", "Tennessee Public Radio WPLN", "US", "en", "hash_wpln", "Tennessee Public Radio WPLN"},
    {58, "stage1p-a-slot-wfpl", "Kentucky Public Radio WFPL", "US", "en", "hash_wfpl", "Kentucky Public Radio WFPL"},
    {59, "stage1p-a-slot-wfiu", "Indiana Public Media WFIU", "US", "en", "hash_wfiu", "Indiana Public Media WFIU"},
    {60, "stage1p-a-slot-wosu", "Ohio Public Radio WOSU", "US", "en", "hash_wosu", "Ohio Public Radio WOSU"},
    {61, "stage1p-a-slot-wvpb", "West Virginia Public Broadcasting", "US", "en", "hash_wvpb", "West Virginia Public Broadcasting"},
    {62, "stage1p-a-slot-wnpr", "Connecticut Public Radio WNPR", "US", "en", "hash_wnpr", "Connecticut Public Radio WNPR"},
    {63, "stage1p-a-slot-ripbs", "Rhode Island PBS Radio", "US", "en", "hash_ripbs", "Rhode Island PBS Radio"}
};

static long long fvsa_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return ((long long)ts.tv_sec * 1000LL) + ((long long)ts.tv_nsec / 1000000LL);
}

static uint64_t fvsa_fnv1a64(const unsigned char *data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned long long fvsa_xorshift64star(unsigned long long x) {
    if (x == 0ULL) {
        x = 0x9e3779b97f4a7c15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 2685821657736338717ULL;
}

static char *fvsa_strdup_local(const char *s) {
    if (s == NULL) {
        s = "";
    }
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (out != NULL) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static char *fvsa_jstring(JNIEnv *env, jstring s) {
    if (s == NULL) {
        return fvsa_strdup_local("");
    }

    const char *tmp = (*env)->GetStringUTFChars(env, s, NULL);
    if (tmp == NULL) {
        return fvsa_strdup_local("");
    }

    char *out = fvsa_strdup_local(tmp);
    (*env)->ReleaseStringUTFChars(env, s, tmp);
    return out;
}

static char *fvsa_json_escape(const char *s) {
    if (s == NULL) {
        s = "";
    }

    size_t extra = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                extra++;
                break;
            default:
                if (*p < 0x20) {
                    extra += 5;
                }
                break;
        }
    }

    size_t n = strlen(s);
    char *out = (char *)malloc(n + extra + 1);
    if (out == NULL) {
        return NULL;
    }

    char *q = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\\': *q++='\\'; *q++='\\'; break;
            case '"': *q++='\\'; *q++='"'; break;
            case '\n': *q++='\\'; *q++='n'; break;
            case '\r': *q++='\\'; *q++='r'; break;
            case '\t': *q++='\\'; *q++='t'; break;
            default:
                if (*p < 0x20) {
                    snprintf(q, 7, "\\u%04x", (unsigned int)*p);
                    q += 6;
                } else {
                    *q++ = (char)*p;
                }
                break;
        }
    }
    *q = 0;
    return out;
}

static void fvsa_copy_trunc(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }

    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void fvsa_timeline_record_c(const char *kind, const char *detail) {
    if (kind == NULL || kind[0] == 0) {
        kind = "unknown";
    }
    if (detail == NULL) {
        detail = "";
    }

    pthread_mutex_lock(&fvsa_timeline_mutex);

    unsigned long long seq = fvsa_timeline_seq++;
    FvsaTimelineEvent *ev = &fvsa_timeline[seq % FVSA_TIMELINE_CAP];
    ev->seq = seq;
    ev->unix_ms = fvsa_now_ms();
    fvsa_copy_trunc(ev->kind, sizeof(ev->kind), kind);
    fvsa_copy_trunc(ev->detail, sizeof(ev->detail), detail);

    pthread_mutex_unlock(&fvsa_timeline_mutex);
}

static char *fvsa_timeline_snapshot_json(long long window_ms) {
    long long now = fvsa_now_ms();
    size_t cap = 1024 + ((size_t)FVSA_TIMELINE_CAP * 2600);
    char *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    size_t len = 0;
    int first = 1;
    int wrote = snprintf(out + len, cap - len, "[");
    if (wrote < 0 || (size_t)wrote >= cap - len) {
        free(out);
        return NULL;
    }
    len += (size_t)wrote;

    pthread_mutex_lock(&fvsa_timeline_mutex);

    unsigned long long end = fvsa_timeline_seq;
    unsigned long long start = (end > FVSA_TIMELINE_CAP) ? (end - FVSA_TIMELINE_CAP) : 0ULL;

    for (unsigned long long seq = start; seq < end; seq++) {
        FvsaTimelineEvent ev = fvsa_timeline[seq % FVSA_TIMELINE_CAP];
        if (ev.seq != seq) {
            continue;
        }
        if (window_ms > 0 && ev.unix_ms > 0 && now > ev.unix_ms && (now - ev.unix_ms) > window_ms) {
            continue;
        }

        char *kind_esc = fvsa_json_escape(ev.kind);
        char *detail_esc = fvsa_json_escape(ev.detail);
        if (kind_esc == NULL || detail_esc == NULL) {
            free(kind_esc);
            free(detail_esc);
            continue;
        }

        wrote = snprintf(
            out + len,
            cap - len,
            "%s{\"seq\":%llu,\"unix_ms\":%lld,\"age_ms\":%lld,\"event_kind\":\"%s\",\"detail\":\"%s\"}",
            first ? "" : ",",
            (unsigned long long)ev.seq,
            ev.unix_ms,
            (ev.unix_ms > 0 && now >= ev.unix_ms) ? (now - ev.unix_ms) : 0LL,
            kind_esc,
            detail_esc
        );

        free(kind_esc);
        free(detail_esc);

        if (wrote < 0 || (size_t)wrote >= cap - len) {
            break;
        }

        len += (size_t)wrote;
        first = 0;
    }

    pthread_mutex_unlock(&fvsa_timeline_mutex);

    if (len + 2 >= cap) {
        out[cap - 2] = ']';
        out[cap - 1] = 0;
    } else {
        snprintf(out + len, cap - len, "]");
    }

    return out;
}

static FvsaSweepEvent fvsa_sweep_next_c(const char *hit_id, const char *hit_json) {
    uint64_t hit_mix = fvsa_fnv1a64((const unsigned char *)(hit_id ? hit_id : ""), strlen(hit_id ? hit_id : ""));
    hit_mix ^= fvsa_fnv1a64((const unsigned char *)(hit_json ? hit_json : ""), strlen(hit_json ? hit_json : ""));

    pthread_mutex_lock(&fvsa_sweep_mutex);

    fvsa_sweep_state ^= hit_mix;
    fvsa_sweep_state = fvsa_xorshift64star(fvsa_sweep_state + fvsa_sweep_seq + 0x9e3779b97f4a7c15ULL);

    FvsaSweepEvent ev;
    ev.schedule_seq = fvsa_sweep_seq++;
    ev.seed = fvsa_sweep_state;
    ev.unix_ms = fvsa_now_ms();
    ev.slot = (int)(ev.seed % FVSA_SWEEP_SLOT_COUNT);
    ev.dwell_ms = 160 + (int)((ev.seed >> 8) % 121ULL);
    ev.jitter_ms = (int)((ev.seed >> 17) % 61ULL);
    ev.slot_info = &fvsa_slots[ev.slot];

    pthread_mutex_unlock(&fvsa_sweep_mutex);

    return ev;
}

static char *fvsa_sweep_engine_json(const FvsaSweepEvent *ev) {
    if (ev == NULL || ev->slot_info == NULL) {
        return fvsa_strdup_local("{}");
    }

    size_t cap = 2048;
    char *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    snprintf(
        out,
        cap,
        "{"
        "\"stage\":\"1P-B\","
        "\"scheduler\":\"native_c_deterministic\",\"catalog_provider\":\"manual_seed_v1_radio_browser_compatible\",\"radio_browser_compatible\":true,\"uuid_policy\":\"stationuuid_required\",\"countrycode_policy\":\"countrycode_required\",\"server_discovery_policy\":\"future_dns_all_api_radio_browser_info_randomize_retry\",\"http_user_agent_policy\":\"speaking_agent_required\","
        "\"seed_hex\":\"%016llx\","
        "\"slot_count\":%d,"
        "\"active_slot\":%d,"
        "\"dwell_ms\":%d,"
        "\"jitter_ms\":%d,"
        "\"overlap_policy\":\"single_open_slot\","
        "\"schedule_seq\":%llu,"
        "\"stationuuid\":\"%s\","
        "\"station_name\":\"%s\","
        "\"countrycode\":\"%s\","
        "\"language\":\"%s\","
        "\"stream_url_hash\":\"%s\""
        "}",
        (unsigned long long)ev->seed,
        FVSA_SWEEP_SLOT_COUNT,
        ev->slot,
        ev->dwell_ms,
        ev->jitter_ms,
        (unsigned long long)ev->schedule_seq,
        ev->slot_info->station_uuid,
        ev->slot_info->station_name,
        ev->slot_info->country_code,
        ev->slot_info->language,
        ev->slot_info->stream_url_hash
    );

    return out;
}

static char *fvsa_sweep_event_json(const FvsaSweepEvent *ev) {
    if (ev == NULL || ev->slot_info == NULL) {
        return fvsa_strdup_local("{}\n");
    }

    size_t cap = 2048;
    char *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    snprintf(
        out,
        cap,
        "{"
        "\"event\":\"sweep_slot\","
        "\"event_type\":\"sweep_slot\","
        "\"stage\":\"1P-B\","
        "\"seq\":%llu,"
        "\"unix_ms\":%lld,"
        "\"slot\":%d,"
        "\"stationuuid\":\"%s\","
        "\"station_name\":\"%s\","
        "\"countrycode\":\"%s\","
        "\"language\":\"%s\","
        "\"stream_url_hash\":\"%s\","
        "\"dwell_ms\":%d,"
        "\"jitter_ms\":%d,"
        "\"seed_hex\":\"%016llx\","
        "\"scheduler\":\"native_c_deterministic\",\"catalog_provider\":\"manual_seed_v1_radio_browser_compatible\",\"radio_browser_compatible\":true,\"uuid_policy\":\"stationuuid_required\",\"countrycode_policy\":\"countrycode_required\",\"server_discovery_policy\":\"future_dns_all_api_radio_browser_info_randomize_retry\",\"http_user_agent_policy\":\"speaking_agent_required\","
        "\"overlap_policy\":\"single_open_slot\","
        "\"generated_words\":false,"
        "\"word_bank\":false,"
        "\"tts\":false"
        "}\n",
        (unsigned long long)ev->schedule_seq,
        ev->unix_ms,
        ev->slot,
        ev->slot_info->station_uuid,
        ev->slot_info->station_name,
        ev->slot_info->country_code,
        ev->slot_info->language,
        ev->slot_info->stream_url_hash,
        ev->dwell_ms,
        ev->jitter_ms,
        (unsigned long long)ev->seed
    );

    return out;
}

static char *fvsa_source_slots_json(void) {
    size_t cap = 65536;
    char *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    size_t len = 0;
    int wrote = snprintf(out + len, cap - len,
        "{\"event_type\":\"source_slots\",\"stage\":\"1P-B\",\"catalog_provider\":\"manual_seed_v1_radio_browser_compatible\",\"radio_browser_compatible\":true,\"uuid_policy\":\"stationuuid_required\",\"countrycode_policy\":\"countrycode_required\",\"server_discovery_policy\":\"future_dns_all_api_radio_browser_info_randomize_retry\",\"http_user_agent_policy\":\"speaking_agent_required\",\"raw_stream_urls_exported\":false,\"slot_count\":%d,\"slots\":[",
        FVSA_SWEEP_SLOT_COUNT);
    if (wrote < 0 || (size_t)wrote >= cap - len) {
        free(out);
        return NULL;
    }
    len += (size_t)wrote;

    for (int i = 0; i < FVSA_SWEEP_SLOT_COUNT; i++) {
        const FvsaSweepSlot *s = &fvsa_slots[i];
        wrote = snprintf(
            out + len,
            cap - len,
            "%s{\"slot\":%d,\"stationuuid\":\"%s\",\"station_name\":\"%s\",\"countrycode\":\"%s\",\"language\":\"%s\",\"stream_url_hash\":\"%s\",\"source_label\":\"%s\",\"stationuuid_source\":\"manual_seed_pending_live_radio_browser_lookup\",\"raw_stream_url_exported\":false,\"url_hash_policy\":\"hash_only_until_live_resolution\"}",
            i == 0 ? "" : ",",
            s->slot,
            s->station_uuid,
            s->station_name,
            s->country_code,
            s->language,
            s->stream_url_hash,
            s->source_label
        );
        if (wrote < 0 || (size_t)wrote >= cap - len) {
            break;
        }
        len += (size_t)wrote;
    }

    snprintf(out + len, cap - len,
        "],\"generated_words\":false,\"word_bank\":false,\"tts\":false}\n");
    return out;
}

static char *fvsa_sweep_manifest_json(void) {
    size_t cap = 2048;
    char *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    snprintf(
        out,
        cap,
        "{"
        "\"event_type\":\"sweep_manifest\","
        "\"stage\":\"1P-B\","
        "\"scheduler\":\"native_c_deterministic\",\"catalog_provider\":\"manual_seed_v1_radio_browser_compatible\",\"radio_browser_compatible\":true,\"uuid_policy\":\"stationuuid_required\",\"countrycode_policy\":\"countrycode_required\",\"server_discovery_policy\":\"future_dns_all_api_radio_browser_info_randomize_retry\",\"http_user_agent_policy\":\"speaking_agent_required\","
        "\"slot_count\":%d,"
        "\"overlap_policy\":\"single_open_slot\","
        "\"dwell_ms_min\":160,"
        "\"dwell_ms_max\":280,"
        "\"jitter_ms_min\":0,"
        "\"jitter_ms_max\":60,"
        "\"audio_engine_changed\":false,"
        "\"java_playback_owner\":true,"
        "\"native_scheduler_evidence_only\":true,"
        "\"generated_words\":false,"
        "\"word_bank\":false,"
        "\"tts\":false"
        "}\n",
        FVSA_SWEEP_SLOT_COUNT
    );

    return out;
}

static int fvsa_write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int fvsa_write_file(const char *path, const char *buf, int append) {
    int flags = O_WRONLY | O_CREAT;
    flags |= append ? O_APPEND : O_TRUNC;

    int fd = open(path, flags, 0600);
    if (fd < 0) {
        return -1;
    }

    int rc = fvsa_write_all(fd, buf, strlen(buf));
    if (fsync(fd) != 0 && rc == 0) {
        rc = -1;
    }
    if (close(fd) != 0 && rc == 0) {
        rc = -1;
    }
    return rc;
}

static int fvsa_ensure_dir(const char *path) {
    if (path == NULL || path[0] == 0) {
        return -1;
    }
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static char *fvsa_basename_safe(const char *s) {
    char *esc = fvsa_json_escape(s ? s : "unknown");
    if (esc == NULL) {
        return NULL;
    }

    for (char *p = esc; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == ' ' || *p == '"' || *p == '\'') {
            *p = '_';
        }
    }
    return esc;
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeTimelineRecord(
        JNIEnv *env,
        jobject self,
        jstring eventKind,
        jstring detail) {
    (void)self;

    char *kind = fvsa_jstring(env, eventKind);
    char *det = fvsa_jstring(env, detail);

    if (kind == NULL || det == NULL) {
        free(kind);
        free(det);
        return (*env)->NewStringUTF(env, "NATIVE_TIMELINE_RECORD_FAILED alloc");
    }

    fvsa_timeline_record_c(kind, det);

    char result[192];
    snprintf(result, sizeof(result), "NATIVE_TIMELINE_RECORD_OK kind=%s", kind[0] ? kind : "empty");

    free(kind);
    free(det);

    return (*env)->NewStringUTF(env, result);
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeSourceAuditHit(
        JNIEnv *env,
        jobject self,
        jstring hitDirPath,
        jstring hitId,
        jstring hitJsonObject) {
    (void)self;

    char *dir = fvsa_jstring(env, hitDirPath);
    char *hit = fvsa_jstring(env, hitId);
    char *hit_json = fvsa_jstring(env, hitJsonObject);

    if (dir == NULL || hit == NULL || hit_json == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc");
    }

    if (fvsa_ensure_dir(dir) != 0) {
        free(dir);
        free(hit);
        free(hit_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED mkdir");
    }

    char magic_hex[(sizeof(FVSA_MAGIC_BLOB) * 2) + 1];
    for (size_t i = 0; i < sizeof(FVSA_MAGIC_BLOB); i++) {
        snprintf(magic_hex + (i * 2), 3, "%02x", FVSA_MAGIC_BLOB[i]);
    }
    magic_hex[sizeof(FVSA_MAGIC_BLOB) * 2] = 0;

    uint64_t hit_hash = fvsa_fnv1a64((const unsigned char *)hit_json, strlen(hit_json));
    char hash_hex[17];
    snprintf(hash_hex, sizeof(hash_hex), "%016llx", (unsigned long long)hit_hash);

    char *hit_esc = fvsa_json_escape(hit);
    char *hit_file_safe = fvsa_basename_safe(hit);
    if (hit_esc == NULL || hit_file_safe == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED escape");
    }

    const char *raw_object = hit_json;
    while (*raw_object == ' ' || *raw_object == '\n' || *raw_object == '\r' || *raw_object == '\t') {
        raw_object++;
    }
    int raw_is_object = (*raw_object == '{');

    char *hit_json_escaped = NULL;
    if (!raw_is_object) {
        hit_json_escaped = fvsa_json_escape(hit_json);
        if (hit_json_escaped == NULL) {
            free(dir);
            free(hit);
            free(hit_json);
            free(hit_esc);
            free(hit_file_safe);
            return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED escape_hit_json");
        }
    }

    FvsaSweepEvent sweep_ev = fvsa_sweep_next_c(hit, hit_json);
    char *sweep_engine_json = fvsa_sweep_engine_json(&sweep_ev);
    char *sweep_event_json = fvsa_sweep_event_json(&sweep_ev);
    char *slots_json = fvsa_source_slots_json();
    char *manifest_json = fvsa_sweep_manifest_json();

    if (sweep_engine_json == NULL || sweep_event_json == NULL || slots_json == NULL || manifest_json == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED sweep_alloc");
    }

    fvsa_timeline_record_c("sweep_slot", sweep_event_json);

    char *timeline_json = fvsa_timeline_snapshot_json(60000LL);
    if (timeline_json == NULL) {
        timeline_json = fvsa_strdup_local("[]");
    }
    if (timeline_json == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED timeline_alloc");
    }

    long long now_ms = fvsa_now_ms();
    size_t cap = strlen(hit_json) + strlen(timeline_json) + strlen(sweep_engine_json) + strlen(dir) + strlen(hit_esc) + strlen(magic_hex) + 12000;
    char *audit = (char *)malloc(cap);
    if (audit == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(timeline_json);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc_audit");
    }

    int n = snprintf(
        audit,
        cap,
        "{"
        "\"event\":\"source_audit\","
        "\"event_type\":\"source_audit\","
        "\"stage\":\"1P-B\","
        "\"audit_format_version\":4,"
        "\"native_writer\":\"c_jni\","
        "\"implementation_language\":\"C\","
        "\"binary_magic_hex\":\"%s\","
        "\"audit_unix_ms\":%lld,"
        "\"hit_id\":\"%s\","
        "\"hit_json_fnv1a64\":\"%s\","
        "\"sweep_engine\":%s,\"source_catalog_policy\":{\"provider\":\"manual_seed_v1_radio_browser_compatible\",\"radio_browser_compatible\":true,\"uuid_policy\":\"stationuuid_required\",\"countrycode_policy\":\"countrycode_required\",\"server_discovery_policy\":\"future_dns_all_api_radio_browser_info_randomize_retry\",\"http_user_agent_policy\":\"speaking_agent_required\",\"raw_stream_urls_exported\":false},"
        "\"timeline_window_ms\":60000,"
        "\"timeline_snapshot\":%s,"
        "\"source_hit_json\":%s%s%s,"
        "\"generated_words\":false,"
        "\"word_bank\":false,"
        "\"tts\":false,"
        "\"network_access_from_native\":false,"
        "\"storage_scope\":\"app_private_hit_bundle_then_existing_android_export\","
        "\"review_rule\":\"Do not show expected phrase before blind review.\""
        "}\n",
        magic_hex,
        now_ms,
        hit_esc,
        hash_hex,
        sweep_engine_json,
        timeline_json,
        raw_is_object ? "" : "\"",
        raw_is_object ? raw_object : hit_json_escaped,
        raw_is_object ? "" : "\""
    );

    if (n < 0 || (size_t)n >= cap) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(timeline_json);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        free(audit);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED snprintf");
    }

    size_t path_cap = strlen(dir) + strlen(hit_file_safe) + 160;
    char *per_hit_path = (char *)malloc(path_cap);
    char *ledger_path = (char *)malloc(path_cap);
    char *readme_path = (char *)malloc(path_cap);
    char *schedule_path = (char *)malloc(path_cap);
    char *slots_path = (char *)malloc(path_cap);
    char *manifest_path = (char *)malloc(path_cap);
    if (per_hit_path == NULL || ledger_path == NULL || readme_path == NULL ||
        schedule_path == NULL || slots_path == NULL || manifest_path == NULL) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(timeline_json);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        free(audit);
        free(per_hit_path);
        free(ledger_path);
        free(readme_path);
        free(schedule_path);
        free(slots_path);
        free(manifest_path);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc_path");
    }

    snprintf(per_hit_path, path_cap, "%s/source_audit_%s.json", dir, hit_file_safe);
    snprintf(ledger_path, path_cap, "%s/fieldvox-source-audit.jsonl", dir);
    snprintf(readme_path, path_cap, "%s/SOURCE_AUDIT_README.txt", dir);
    snprintf(schedule_path, path_cap, "%s/sweep_schedule.jsonl", dir);
    snprintf(slots_path, path_cap, "%s/source_slots.json", dir);
    snprintf(manifest_path, path_cap, "%s/sweep_manifest.json", dir);

    const char *readme =
        "FieldVox Native Source Audit\n"
        "\n"
        "These files are generated by native C/JNI code.\n"
        "Stage 1P-B adds Radio Browser-compatible native C source-slot catalog policy.\n"
        "This stage records scheduler and source-catalog metadata but does not change playback.\n"
        "\n"
        "sweep_manifest.json describes scheduler policy.\n"
        "source_slots.json describes source slot identities and Radio Browser-compatible UUID/countrycode policy.\n"
        "sweep_schedule.jsonl appends one deterministic sweep_slot event per marked hit.\n"
        "source_audit_*.json embeds sweep_engine, timeline_snapshot, and source_hit_json.\nStage 1P-B canonical exports avoid duplicate JSON object names for parser interoperability.\n"
        "\n"
        "The native writer does not generate words, does not use a word bank, does not use TTS,\n"
        "and does not open network connections.\n"
        "Reviewers should listen to blind clips before viewing labels or truth TSV files.\n";

    if (fvsa_write_file(per_hit_path, audit, 0) != 0 ||
        fvsa_write_file(ledger_path, audit, 1) != 0 ||
        fvsa_write_file(readme_path, readme, 0) != 0 ||
        fvsa_write_file(schedule_path, sweep_event_json, 1) != 0 ||
        fvsa_write_file(slots_path, slots_json, 0) != 0 ||
        fvsa_write_file(manifest_path, manifest_json, 0) != 0) {
        free(dir);
        free(hit);
        free(hit_json);
        free(hit_esc);
        free(hit_file_safe);
        free(hit_json_escaped);
        free(timeline_json);
        free(sweep_engine_json);
        free(sweep_event_json);
        free(slots_json);
        free(manifest_json);
        free(audit);
        free(per_hit_path);
        free(ledger_path);
        free(readme_path);
        free(schedule_path);
        free(slots_path);
        free(manifest_path);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED write");
    }

    char result[640];
    snprintf(
        result,
        sizeof(result),
        "NATIVE_SOURCE_AUDIT_OK hit_id=%s hash=%s writer=c_jni timeline=ring_buffer sweep=slot_%d seq=%llu",
        hit,
        hash_hex,
        sweep_ev.slot,
        (unsigned long long)sweep_ev.schedule_seq
    );

    free(dir);
    free(hit);
    free(hit_json);
    free(hit_esc);
    free(hit_file_safe);
    free(hit_json_escaped);
    free(timeline_json);
    free(sweep_engine_json);
    free(sweep_event_json);
    free(slots_json);
    free(manifest_json);
    free(audit);
    free(per_hit_path);
    free(ledger_path);
    free(readme_path);
    free(schedule_path);
    free(slots_path);
    free(manifest_path);

    return (*env)->NewStringUTF(env, result);
}
