#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * FieldVox Stage 1O-A Native Source Provenance Audit
 * GPL-3.0-or-later project code.
 *
 * This module intentionally keeps provenance serialization in native C:
 * - no word bank
 * - no TTS
 * - no generated phrase synthesis
 * - no network access
 * - no broad storage access
 *
 * Java passes the already-created hit JSON object. C wraps it in a native
 * audit record, adds binary magic/versioning/hash metadata, writes a per-hit
 * JSON file, and appends a JSONL ledger beside the hit bundle.
 */

/* Small binary blob marker for native audit records: "FVSA\1\0CJNI\0" */
static const unsigned char FVSA_MAGIC_BLOB[] = {
    0x46, 0x56, 0x53, 0x41, 0x01, 0x00, 0x43, 0x4a,
    0x4e, 0x49, 0x00, 0x1f, 0x0a, 0x7e, 0x5a, 0xc3
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
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static char *fvsa_jstring(JNIEnv *env, jstring s) {
    if (s == NULL) {
        char *z = (char *)calloc(1, 1);
        return z;
    }
    const char *tmp = (*env)->GetStringUTFChars(env, s, NULL);
    if (tmp == NULL) {
        char *z = (char *)calloc(1, 1);
        return z;
    }
    size_t n = strlen(tmp);
    char *out = (char *)malloc(n + 1);
    if (out != NULL) {
        memcpy(out, tmp, n + 1);
    }
    (*env)->ReleaseStringUTFChars(env, s, tmp);
    return out;
}

static char *fvsa_json_escape(const char *s) {
    if (s == NULL) s = "";
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
                if (*p < 0x20) extra += 5;
                break;
        }
    }

    size_t n = strlen(s);
    char *out = (char *)malloc(n + extra + 1);
    if (!out) return NULL;

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
                    sprintf(q, "\\u%04x", (unsigned int)*p);
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

static int fvsa_write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int fvsa_write_file(const char *path, const char *buf, int append) {
    int flags = O_WRONLY | O_CREAT;
    flags |= append ? O_APPEND : O_TRUNC;
    int fd = open(path, flags, 0600);
    if (fd < 0) return -1;
    int rc = fvsa_write_all(fd, buf, strlen(buf));
    if (fsync(fd) != 0 && rc == 0) rc = -1;
    if (close(fd) != 0 && rc == 0) rc = -1;
    return rc;
}

static int fvsa_ensure_dir(const char *path) {
    if (path == NULL || path[0] == 0) return -1;
    if (mkdir(path, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static char *fvsa_basename_safe(const char *s) {
    char *esc = fvsa_json_escape(s ? s : "unknown");
    if (!esc) return NULL;
    for (char *p = esc; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == ' ' || *p == '"' || *p == '\'') {
            *p = '_';
        }
    }
    return esc;
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

    if (!dir || !hit || !hit_json) {
        free(dir); free(hit); free(hit_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc");
    }

    if (fvsa_ensure_dir(dir) != 0) {
        free(dir); free(hit); free(hit_json);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED mkdir");
    }

    char magic_hex[(sizeof(FVSA_MAGIC_BLOB) * 2) + 1];
    for (size_t i = 0; i < sizeof(FVSA_MAGIC_BLOB); i++) {
        sprintf(magic_hex + (i * 2), "%02x", FVSA_MAGIC_BLOB[i]);
    }
    magic_hex[sizeof(FVSA_MAGIC_BLOB) * 2] = 0;

    uint64_t hit_hash = fvsa_fnv1a64((const unsigned char *)hit_json, strlen(hit_json));
    char hash_hex[17];
    sprintf(hash_hex, "%016llx", (unsigned long long)hit_hash);

    char *hit_esc = fvsa_json_escape(hit);
    char *hit_file_safe = fvsa_basename_safe(hit);
    if (!hit_esc || !hit_file_safe) {
        free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe);
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
        if (!hit_json_escaped) {
            free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe);
            return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED escape_hit_json");
        }
    }

    long long now_ms = fvsa_now_ms();

    size_t cap = strlen(hit_json) + strlen(dir) + strlen(hit_esc) + strlen(magic_hex) + 4096;
    char *audit = (char *)malloc(cap);
    if (!audit) {
        free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe); free(hit_json_escaped);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc_audit");
    }

    int n = snprintf(
        audit,
        cap,
        "{"
        "\"event\":\"source_audit\","
        "\"event_type\":\"source_audit\","
        "\"stage\":\"1O-A\","
        "\"audit_format_version\":1,"
        "\"native_writer\":\"c_jni\","
        "\"implementation_language\":\"C\","
        "\"binary_magic_hex\":\"%s\","
        "\"audit_unix_ms\":%lld,"
        "\"hit_id\":\"%s\","
        "\"hit_json_fnv1a64\":\"%s\","
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
        raw_is_object ? "" : "\"",
        raw_is_object ? raw_object : hit_json_escaped,
        raw_is_object ? "" : "\""
    );

    if (n < 0 || (size_t)n >= cap) {
        free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe); free(hit_json_escaped); free(audit);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED snprintf");
    }

    size_t path_cap = strlen(dir) + strlen(hit_file_safe) + 128;
    char *per_hit_path = (char *)malloc(path_cap);
    char *ledger_path = (char *)malloc(path_cap);
    char *readme_path = (char *)malloc(path_cap);
    if (!per_hit_path || !ledger_path || !readme_path) {
        free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe); free(hit_json_escaped); free(audit);
        free(per_hit_path); free(ledger_path); free(readme_path);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED alloc_path");
    }

    snprintf(per_hit_path, path_cap, "%s/source_audit_%s.json", dir, hit_file_safe);
    snprintf(ledger_path, path_cap, "%s/fieldvox-source-audit.jsonl", dir);
    snprintf(readme_path, path_cap, "%s/SOURCE_AUDIT_README.txt", dir);

    const char *readme =
        "FieldVox Native Source Audit\n"
        "\n"
        "These files are generated by native C/JNI code.\n"
        "They wrap each marked-hit JSON object with provenance metadata, a binary magic marker,\n"
        "and an FNV-1a hash of the hit JSON text.\n"
        "\n"
        "The native writer does not generate words, does not use a word bank, does not use TTS,\n"
        "and does not open network connections.\n"
        "Reviewers should listen to blind clips before viewing labels or truth TSV files.\n";

    if (fvsa_write_file(per_hit_path, audit, 0) != 0 ||
        fvsa_write_file(ledger_path, audit, 1) != 0 ||
        fvsa_write_file(readme_path, readme, 0) != 0) {
        free(dir); free(hit); free(hit_json); free(hit_esc); free(hit_file_safe); free(hit_json_escaped);
        free(audit); free(per_hit_path); free(ledger_path); free(readme_path);
        return (*env)->NewStringUTF(env, "NATIVE_SOURCE_AUDIT_FAILED write");
    }

    char result[512];
    snprintf(result, sizeof(result),
             "NATIVE_SOURCE_AUDIT_OK hit_id=%s hash=%s writer=c_jni files=source_audit_json,fieldvox-source-audit.jsonl",
             hit, hash_hex);

    free(dir);
    free(hit);
    free(hit_json);
    free(hit_esc);
    free(hit_file_safe);
    free(hit_json_escaped);
    free(audit);
    free(per_hit_path);
    free(ledger_path);
    free(readme_path);

    return (*env)->NewStringUTF(env, result);
}
