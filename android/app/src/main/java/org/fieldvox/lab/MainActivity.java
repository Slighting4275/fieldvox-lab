package org.fieldvox.lab;

import android.app.Activity;
import android.app.AlertDialog;
import android.util.Log;
import android.hardware.SensorManager;
import android.hardware.SensorEventListener;
import android.hardware.SensorEvent;
import android.hardware.Sensor;
import android.media.MediaPlayer;
import android.media.AudioAttributes;
import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.net.Uri;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.SeekBar;
import android.widget.EditText;

import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.util.ArrayList;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("fieldvox");
    }

    private final Handler handler = new Handler(Looper.getMainLooper());

    private TextView status;
    private TextView sessions;
    private TextView exportStatus;
    private TextView thresholdLabel;
    private TextView cooldownLabel;
    private Button startButton;
    private Button sessionsButton;
    private Button exportButton;
    private Button shareButton;
    private Button controlsButton;
    private WavePanelView wavePanel;
    private LinearLayout controlPanel;
    private boolean running = false;
    private boolean controlsVisible = false;
    private Button liveButton;
    private volatile boolean liveMonitorRunning = false;
    private volatile boolean speakerMonitorEnabled = false;
    private Thread liveMonitorThread;
    private Button speakerMonitorButton;
    private Button markHitButton;
    private Button exportHitsButton;
    private final Object hitMarkerLock = new Object();
    private final ArrayList<String> pendingHitJsonLines = new ArrayList<>();
    private File currentLiveJsonlFile;
    private File currentLiveWavFile;
    private String currentLiveSessionStem = "";
    private long currentLiveSessionStartMs = 0L;
    private int hitMarkerCount = 0;
    private String onlineCarrierLastGateReason = "none";
    private float onlineCarrierLastGateDelta = 0.0f;
    private int onlineCarrierLastPacketToken = 0;
    private int onlineCarrierLastGrainCount = 0;
    private int onlineCarrierLastPacketMs = 0;
    private int onlineCarrierLastForcedSilenceMs = 0;
    private long onlineCarrierLastPacketUnixMs = 0L;
    private double liveNoiseFloor = 300.0;
    private long liveLastTriggerMs = 0L;
    private float liveOutputGain = 3.0f;

    private MediaPlayer onlineCarrierPlayer;
    private volatile boolean onlineCarrierActive = false;
    private volatile boolean onlineCarrierPrepared = false;
    private int onlineCarrierIndex = 0;
    private long onlineCarrierLastGateMs = 0L;
    private long onlineCarrierGateOpenUntilMs = 0L;
    private long onlineCarrierNextAllowedGateMs = 0L;
    private boolean onlineCarrierGateOpen = false;
    private int onlineCarrierGateToken = 0;
    private int onlineCarrierLastSweptToken = 0;
    private boolean onlineCarrierStartupTestDone = false;
    private int onlineCarrierSweepCount = 0;
    private int onlineCarrierPrepareToken = 0;
    private int onlineCarrierGranularPacketToken = 0;
    private Runnable onlineCarrierAutoGateRunnable;
    private float onlineCarrierMagBaseline = -1.0f;
    private float onlineCarrierGyroBaseline = -1.0f;
    private float onlineCarrierProximityLast = -1.0f;
    private boolean onlineCarrierProximityNear = false;
    private long onlineCarrierLastProximityLogMs = 0L;
    private boolean onlineCarrierPendingProximityGate = false;
    private String onlineCarrierPendingProximityReason = "";
    private float onlineCarrierPendingProximityDelta = 0.0f;
    private int onlineCarrierRegisteredSensorCount = 0;
    private SensorManager onlineCarrierSensorManager;
    private SensorEventListener onlineCarrierSensorListener;

    private static final String[] ONLINE_CARRIER_URLS = new String[] {
            "https://bonneville.cdnstream1.com/2643_48.aac",
            "https://playerservices.streamtheworld.com/api/livestream-redirect/KNWNAM.mp3",
            "https://playerservices.streamtheworld.com/api/livestream-redirect/KUOWFM_HIGH_MP3.mp3",
            "https://playerservices.streamtheworld.com/api/livestream-redirect/KUOWFM_LOW_MP3.mp3",
            "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
            "https://news-stream.iowapublicradio.org/NewsLow.mp3",
            "https://news-stream.iowapublicradio.org/News.mp3",
            "https://wpr-ice.streamguys1.com/wpr-ideas-mp3-64",
            "https://streams.kqed.org/kqedradio",
            "https://vprbbc.streamguys1.com/vprbbc24.mp3",
            "https://playerservices.streamtheworld.com/api/livestream-redirect/CSPANRADIO.mp3",
            "https://vpr.streamguys1.com/vpr64.mp3",
            "https://vpr.streamguys1.com/vpr24.mp3",
            "http://161.55.224.79:88"
    };

    private static final String[] ONLINE_CARRIER_LABELS = new String[] {
            "KIRO Newsradio Seattle 97.3",
            "Northwest Newsradio KOMO/KNWN",
            "KUOW Seattle News High",
            "KUOW Seattle News Low",
            "BBC World Service",
            "Iowa Public Radio News Low",
            "Iowa Public Radio News",
            "Wisconsin Public Radio News",
            "KQED Radio News/Talk",
            "Vermont Public BBC Relay",
            "C-SPAN Radio Public Affairs",
            "Vermont Public News/Talk 64",
            "Vermont Public News/Talk 24",
            "NOAA Puget Sound Marine Weather"
    };

    private static final int LIVE_SAMPLE_RATE = 48000;
    private static final int LIVE_PERMISSION_REQUEST = 4101;


    private native String nativeSourceAuditHit(String hitDirPath, String hitId, String hitJsonObject);
    private native void nativeInit(String packageName, String filesDir);
    private native String nativeStatus();
    private native String nativeSessionList();
    private native boolean nativeStart();
    private native void nativeStop();
    private native void nativeSetThreshold(float threshold);
    private native void nativeSetCooldownMs(int cooldownMs);

    private float threshold = 1.65f;
    private int cooldownMs = 450;

    private final Runnable tick = new Runnable() {
        public void run() {
            if (liveMonitorRunning) {
                status.setText("KIRO/KOMO local news VOX running\nSeattle-local news/talk streams are first in the bank. No word bank. No TTS.");
            } else {
                status.setText(nativeStatus());
            }
            if (wavePanel != null) {
                wavePanel.setRunning(running);
                wavePanel.invalidate();
            }
            handler.postDelayed(this, 250);
        }
    };

    private static int dp(Activity a, int v) {
        float d = a.getResources().getDisplayMetrics().density;
        return Math.round(v * d);
    }

    private TextView makeLabel(String text, int color, int sizeSp) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(color);
        t.setTextSize(sizeSp);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private Button makeCircleButton(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setTextSize(10);
        b.setAllCaps(false);
        b.setSingleLine(false);
        b.setGravity(Gravity.CENTER);
        b.setIncludeFontPadding(false);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setBackgroundColor(Color.rgb(28, 28, 32));
        b.setPadding(0, 0, 0, 0);
        return b;
    }

    private Button makeWideButton(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setTextSize(15);
        b.setAllCaps(false);
        b.setBackgroundColor(Color.rgb(28, 28, 32));
        return b;
    }

    private void updateLabels() {
        thresholdLabel.setText(String.format("Sensitivity: %.2f uT delta", threshold));
        cooldownLabel.setText(String.format("Cooldown: %d ms", cooldownMs));
    }

    private void refreshSessions() {
        sessions.setText(nativeSessionList());
    }

    private static final class SessionPair {
        final File jsonl;
        final File wav;

        SessionPair(File jsonl, File wav) {
            this.jsonl = jsonl;
            this.wav = wav;
        }
    }

    private static final class ExportResult {
        final Uri uri;
        final long bytes;

        ExportResult(Uri uri, long bytes) {
            this.uri = uri;
            this.bytes = bytes;
        }
    }

    private SessionPair findLatestClosedPair() {
        File dir = new File(getFilesDir(), "fieldvox-sessions");
        File[] files = dir.listFiles();
        if (files == null) {
            return null;
        }

        SessionPair best = null;
        long bestTime = Long.MIN_VALUE;

        for (File jsonl : files) {
            String name = jsonl.getName();
            if (!jsonl.isFile() || !name.startsWith("session-") || !name.endsWith(".jsonl")) {
                continue;
            }

            String stem = name.substring(0, name.length() - ".jsonl".length());
            File wav = new File(dir, stem + ".wav");
            if (!wav.isFile()) {
                continue;
            }

            long pairTime = Math.max(jsonl.lastModified(), wav.lastModified());
            if (pairTime > bestTime) {
                bestTime = pairTime;
                best = new SessionPair(jsonl, wav);
            }
        }

        return best;
    }

    private void deleteExistingDownloadByName(String displayName) {
        ContentResolver resolver = getContentResolver();
        String rel = Environment.DIRECTORY_DOWNLOADS + "/FieldVoxLab";

        String selection = MediaStore.MediaColumns.DISPLAY_NAME + " = ? AND "
                + MediaStore.MediaColumns.RELATIVE_PATH + " LIKE ?";
        String[] args = new String[] {
                displayName,
                rel + "%"
        };

        try {
            resolver.delete(MediaStore.Downloads.EXTERNAL_CONTENT_URI, selection, args);
        } catch (Exception ignored) {
            // If Android refuses deletion for an old row, the export still continues.
            // Same-app future exports should replace cleanly.
        }
    }

    private ExportResult exportOneToDownloads(File src, String mimeType) throws IOException {
        ContentResolver resolver = getContentResolver();

        deleteExistingDownloadByName(src.getName());

        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, src.getName());
        if (mimeType != null && !mimeType.isEmpty()) {
            values.put(MediaStore.MediaColumns.MIME_TYPE, mimeType);
        }
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/FieldVoxLab");
        values.put(MediaStore.MediaColumns.IS_PENDING, 1);

        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) {
            throw new IOException("MediaStore insert failed for " + src.getName());
        }

        long copied = 0;

        try (FileInputStream in = new FileInputStream(src);
             OutputStream out = resolver.openOutputStream(uri, "w")) {
            if (out == null) {
                throw new IOException("openOutputStream failed for " + src.getName());
            }

            byte[] buf = new byte[64 * 1024];
            while (true) {
                int n = in.read(buf);
                if (n < 0) {
                    break;
                }
                out.write(buf, 0, n);
                copied += n;
            }
            out.flush();
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            throw e;
        }

        ContentValues done = new ContentValues();
        done.put(MediaStore.MediaColumns.IS_PENDING, 0);
        resolver.update(uri, done, null, null);

        return new ExportResult(uri, copied);
    }

    private String exportLatestPair() {
        if (running) {
            return "Stop the current session before export so the WAV header is finalized.";
        }

        SessionPair pair = findLatestClosedPair();
        if (pair == null) {
            return "No matched JSONL+WAV session pair found yet.";
        }

        try {
            ExportResult jsonlOut = exportOneToDownloads(pair.jsonl, null);
            ExportResult wavOut = exportOneToDownloads(pair.wav, "audio/wav");

            return "Exported to Downloads/FieldVoxLab:\n"
                    + pair.jsonl.getName() + " (" + jsonlOut.bytes + " bytes)\n"
                    + pair.wav.getName() + " (" + wavOut.bytes + " bytes)";
        } catch (IOException e) {
            return "Export failed: " + e.getMessage();
        }
    }

    private int countTriggerEvents(File jsonl) {
        int count = 0;
        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(jsonl))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.contains("\"event\":\"trigger\"")) {
                    count += 1;
                }
            }
        } catch (IOException ignored) {
            return -1;
        }
        return count;
    }

    private String exportSessionManifest() {
        if (running) {
            return "Stop the current session before exporting the manifest so WAV headers are finalized.";
        }

        File dir = new File(getFilesDir(), "fieldvox-sessions");
        File[] files = dir.listFiles();
        if (files == null) {
            return "No session folder found.";
        }

        java.util.Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));

        StringBuilder sb = new StringBuilder();
        sb.append("session\tjsonl_file\twav_file\tjsonl_bytes\twav_bytes\ttrigger_count\twav_duration_seconds\tjsonl_modified_ms\twav_modified_ms\n");

        int pairs = 0;
        long totalBytes = 0;

        for (File jsonl : files) {
            String name = jsonl.getName();
            if (!jsonl.isFile() || !name.startsWith("session-") || !name.endsWith(".jsonl")) {
                continue;
            }

            String stem = name.substring(0, name.length() - ".jsonl".length());
            File wav = new File(dir, stem + ".wav");
            if (!wav.isFile()) {
                continue;
            }

            long jsonlBytes = jsonl.length();
            long wavBytes = wav.length();
            int triggerCount = countTriggerEvents(jsonl);

            double durationSeconds = 0.0;
            if (wavBytes > 44) {
                durationSeconds = (double)(wavBytes - 44L) / 96000.0;
            }

            sb.append(stem).append('\t')
                    .append(jsonl.getName()).append('\t')
                    .append(wav.getName()).append('\t')
                    .append(jsonlBytes).append('\t')
                    .append(wavBytes).append('\t')
                    .append(triggerCount).append('\t')
                    .append(String.format(java.util.Locale.US, "%.3f", durationSeconds)).append('\t')
                    .append(jsonl.lastModified()).append('\t')
                    .append(wav.lastModified()).append('\n');

            pairs += 1;
            totalBytes += jsonlBytes + wavBytes;
        }

        if (pairs == 0) {
            return "No matched JSONL+WAV session pairs found for manifest.";
        }

        File manifest = new File(getFilesDir(), "fieldvox-session-manifest.tsv");

        try (java.io.FileOutputStream out = new java.io.FileOutputStream(manifest, false)) {
            out.write(sb.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException e) {
            return "Manifest write failed: " + e.getMessage();
        }

        try {
            ExportResult manifestOut = exportOneToDownloads(manifest, "text/tab-separated-values");
            return "Exported session manifest to Downloads/FieldVoxLab:\n"
                    + manifest.getName()
                    + "\nSession pairs: " + pairs
                    + "\nSession bytes: " + totalBytes
                    + "\nManifest bytes: " + manifestOut.bytes;
        } catch (IOException e) {
            return "Manifest export failed: " + e.getMessage();
        }
    }

    private String exportAllPairs() {
        if (running) {
            return "Stop the current session before export so WAV headers are finalized.";
        }

        File dir = new File(getFilesDir(), "fieldvox-sessions");
        File[] files = dir.listFiles();
        if (files == null) {
            return "No session folder found.";
        }

        int pairs = 0;
        int failures = 0;
        long bytes = 0;

        for (File jsonl : files) {
            String name = jsonl.getName();
            if (!jsonl.isFile() || !name.startsWith("session-") || !name.endsWith(".jsonl")) {
                continue;
            }

            String stem = name.substring(0, name.length() - ".jsonl".length());
            File wav = new File(dir, stem + ".wav");
            if (!wav.isFile()) {
                continue;
            }

            try {
                ExportResult jsonlOut = exportOneToDownloads(jsonl, null);
                ExportResult wavOut = exportOneToDownloads(wav, "audio/wav");
                pairs += 1;
                bytes += jsonlOut.bytes + wavOut.bytes;
            } catch (IOException e) {
                failures += 1;
            }
        }

        if (pairs == 0) {
            return "No matched JSONL+WAV session pairs found.";
        }

        String result = "Exported " + pairs + " session pair(s) to Downloads/FieldVoxLab"
                + " (" + bytes + " bytes).";
        if (failures > 0) {
            result += "\nFailures: " + failures;
        }
        return result;
    }

    private String shareAllPairs() {
        if (running) {
            return "Stop the current session before sharing so WAV headers are finalized.";
        }

        File dir = new File(getFilesDir(), "fieldvox-sessions");
        File[] files = dir.listFiles();
        if (files == null) {
            return "No session folder found.";
        }

        ArrayList<Uri> uris = new ArrayList<>();
        int pairs = 0;
        int failures = 0;
        long bytes = 0;

        for (File jsonl : files) {
            String name = jsonl.getName();
            if (!jsonl.isFile() || !name.startsWith("session-") || !name.endsWith(".jsonl")) {
                continue;
            }

            String stem = name.substring(0, name.length() - ".jsonl".length());
            File wav = new File(dir, stem + ".wav");
            if (!wav.isFile()) {
                continue;
            }

            try {
                ExportResult jsonlOut = exportOneToDownloads(jsonl, null);
                ExportResult wavOut = exportOneToDownloads(wav, "audio/wav");
                uris.add(jsonlOut.uri);
                uris.add(wavOut.uri);
                pairs += 1;
                bytes += jsonlOut.bytes + wavOut.bytes;
            } catch (IOException e) {
                failures += 1;
            }
        }

        if (uris.isEmpty()) {
            return "No matched JSONL+WAV session pairs found to share.";
        }

        try {
            Intent share = new Intent(Intent.ACTION_SEND_MULTIPLE);
            share.setType("*/*");
            share.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "Share all FieldVox sessions"));

            String result = "Sharing " + pairs + " session pair(s), " + uris.size()
                    + " file(s), from Downloads/FieldVoxLab"
                    + " (" + bytes + " bytes).";
            if (failures > 0) {
                result += "\nExport failures: " + failures;
            }
            return result;
        } catch (Exception e) {
            return "Share all failed: " + e.getMessage();
        }
    }

    private String shareLatestPair() {
        if (running) {
            return "Stop the current session before sharing so the WAV header is finalized.";
        }

        SessionPair pair = findLatestClosedPair();
        if (pair == null) {
            return "No matched JSONL+WAV session pair found yet.";
        }

        try {
            ExportResult jsonlOut = exportOneToDownloads(pair.jsonl, null);
            ExportResult wavOut = exportOneToDownloads(pair.wav, "audio/wav");

            ArrayList<Uri> streams = new ArrayList<>();
            streams.add(jsonlOut.uri);
            streams.add(wavOut.uri);

            Intent share = new Intent(Intent.ACTION_SEND_MULTIPLE);
            share.setType("*/*");
            share.putParcelableArrayListExtra(Intent.EXTRA_STREAM, streams);
            share.putExtra(Intent.EXTRA_SUBJECT, "FieldVox Lab session export");
            share.putExtra(Intent.EXTRA_TEXT,
                    "FieldVox Lab experimental sensor-triggered audio session export. "
                            + "This does not prove paranormal activity or spirit communication.");
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

            startActivity(Intent.createChooser(share, "Share FieldVox session pair"));

            return "Opened share sheet for:\n"
                    + pair.jsonl.getName() + " (" + jsonlOut.bytes + " bytes)\n"
                    + pair.wav.getName() + " (" + wavOut.bytes + " bytes)";
        } catch (Exception e) {
            return "Share failed: " + e.getMessage();
        }
    }

    private static String jsonEscape(String s) {
        if (s == null) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\') {
                out.append("\\\\");
            } else if (c == '"') {
                out.append("\\\"");
            } else if (c == '\n') {
                out.append("\\n");
            } else if (c == '\r') {
                out.append("\\r");
            } else if (c == '\t') {
                out.append("\\t");
            } else if (c < 32) {
                out.append(' ');
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    private File hitMarkerDir() {
        File dir = new File(getFilesDir(), "fieldvox-hits");
        if (!dir.isDirectory()) {
            dir.mkdirs();
        }
        return dir;
    }

    private String appVersionNameSafe() {
        try {
            android.content.pm.PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            if (info.versionName != null) {
                return info.versionName;
            }
        } catch (Exception ignored) {}
        return "unknown";
    }

    private String currentCarrierUrlSafe() {
        if (ONLINE_CARRIER_URLS.length <= 0) {
            return "";
        }
        int i = Math.max(0, Math.min(onlineCarrierIndex, ONLINE_CARRIER_URLS.length - 1));
        return ONLINE_CARRIER_URLS[i];
    }

    private String buildHitJsonLine(String hitId, long now, String label, String sessionStem, long offsetMs) {
        int nextIndex = ONLINE_CARRIER_URLS.length <= 0 ? 0 : normalizeOnlineCarrierIndex(onlineCarrierIndex + 1);
        int safeNextLabelIndex = ONLINE_CARRIER_LABELS.length <= 0 ? 0 : Math.max(0, Math.min(nextIndex, ONLINE_CARRIER_LABELS.length - 1));
        String nextLabel = ONLINE_CARRIER_LABELS.length <= 0 ? "" : ONLINE_CARRIER_LABELS[safeNextLabelIndex];

        return "{"
                + "\"event\":\"hit_marker\","
                + "\"event_type\":\"hit_marker\","
                + "\"hit_id\":\"" + jsonEscape(hitId) + "\","
                + "\"unix_ms\":" + now + ","
                + "\"session_id\":\"" + jsonEscape(sessionStem) + "\","
                + "\"session_stem\":\"" + jsonEscape(sessionStem) + "\","
                + "\"hit_offset_ms\":" + offsetMs + ","
                + "\"user_label\":\"" + jsonEscape(label) + "\","
                + "\"perceived_phrase\":\"" + jsonEscape(label) + "\","
                + "\"source_label\":\"" + jsonEscape(onlineCarrierLabel()) + "\","
                + "\"source_url\":\"" + jsonEscape(currentCarrierUrlSafe()) + "\","
                + "\"source_material\":\"real_online_spoken_word_stream\","
                + "\"gate_reason\":\"" + jsonEscape(onlineCarrierLastGateReason) + "\","
                + "\"gate_delta\":" + String.format(java.util.Locale.US, "%.4f", onlineCarrierLastGateDelta) + ","
                + "\"packet_token\":" + onlineCarrierLastPacketToken + ","
                + "\"grain_count\":" + onlineCarrierLastGrainCount + ","
                + "\"packet_ms\":" + onlineCarrierLastPacketMs + ","
                + "\"forced_silence_ms\":" + onlineCarrierLastForcedSilenceMs + ","
                + "\"packet_unix_ms\":" + onlineCarrierLastPacketUnixMs + ","
                + "\"stream_index\":" + onlineCarrierIndex + ","
                + "\"next_stream_index\":" + nextIndex + ","
                + "\"next_stream_label\":\"" + jsonEscape(nextLabel) + "\","
                + "\"sensor_snapshot\":{"
                + "\"mag_baseline\":" + String.format(java.util.Locale.US, "%.4f", onlineCarrierMagBaseline) + ","
                + "\"gyro_baseline\":" + String.format(java.util.Locale.US, "%.4f", onlineCarrierGyroBaseline) + ","
                + "\"proximity_last\":" + String.format(java.util.Locale.US, "%.4f", onlineCarrierProximityLast) + ","
                + "\"proximity_near\":" + onlineCarrierProximityNear
                + "},"
                + "\"pre_ms\":10000,"
                + "\"post_ms\":10000,"
                + "\"generated_words\":false,"
                + "\"word_bank\":false,"
                + "\"tts\":false,"
                + "\"app_version\":\"" + jsonEscape(appVersionNameSafe()) + "\","
                + "\"app_commit_short\":\"runtime-local\","
                + "\"review_rule\":\"Do not tell listeners the expected phrase before blind review.\""
                + "}";
    }

    private void appendTextFile(File file, String text) throws IOException {
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(file, true)) {
            out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
    }

    private void writeTextFile(File file, String text) throws IOException {
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(file, false)) {
            out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
    }

    private void flushPendingHitMarkers(java.io.FileOutputStream jsonOut) throws IOException {
        ArrayList<String> copy = new ArrayList<>();
        synchronized (hitMarkerLock) {
            if (pendingHitJsonLines.isEmpty()) {
                return;
            }
            copy.addAll(pendingHitJsonLines);
            pendingHitJsonLines.clear();
        }
        for (String line : copy) {
            writeJsonLine(jsonOut, line);
        }
    }

    private void promptAndMarkHit() {
        EditText input = new EditText(this);
        input.setSingleLine(false);
        input.setHint("Optional perceived phrase, e.g. what you think you heard");
        input.setTextColor(Color.WHITE);
        input.setHintTextColor(Color.rgb(160, 170, 180));
        input.setBackgroundColor(Color.rgb(20, 20, 24));

        new AlertDialog.Builder(this)
                .setTitle("Mark candidate hit")
                .setMessage("This stores your label as a listener note only. FieldVox does not generate words, use TTS, or use a word bank.")
                .setView(input)
                .setPositiveButton("Save Hit", (dialog, which) -> markCandidateHit(input.getText().toString()))
                .setNegativeButton("Unlabeled Hit", (dialog, which) -> markCandidateHit(""))
                .setNeutralButton("Cancel", null)
                .show();
    }

    private void markCandidateHit(String label) {
        long now = System.currentTimeMillis();
        String sessionStem = currentLiveSessionStem == null ? "" : currentLiveSessionStem;
        if (sessionStem.isEmpty()) {
            SessionPair latest = findLatestClosedPair();
            if (latest != null) {
                String name = latest.jsonl.getName();
                sessionStem = name.endsWith(".jsonl") ? name.substring(0, name.length() - 6) : name;
            } else {
                sessionStem = "session-unknown";
            }
        }

        long offsetMs = currentLiveSessionStartMs > 0L ? Math.max(0L, now - currentLiveSessionStartMs) : 0L;
        int localCount = ++hitMarkerCount;
        String hitId = "hit_" + sessionStem + "_" + now + "_" + localCount;
        String cleanedLabel = label == null ? "" : label.trim();
        String jsonLine = buildHitJsonLine(hitId, now, cleanedLabel, sessionStem, offsetMs);

            try {
                String sourceAuditResult = nativeSourceAuditHit(hitMarkerDir().getAbsolutePath(), hitId, jsonLine);
                Log.i("FieldVox", sourceAuditResult);
            } catch (Throwable sourceAuditThrowable) {
                Log.w("FieldVox", "Native source audit failed", sourceAuditThrowable);
            }


        File dir = hitMarkerDir();
        File hitJson = new File(dir, hitId + ".json");
        File hitJsonl = new File(dir, "fieldvox-hit-events.jsonl");

        try {
            writeTextFile(hitJson, jsonLine + "\n");
            appendTextFile(hitJsonl, jsonLine + "\n");

            synchronized (hitMarkerLock) {
                pendingHitJsonLines.add(jsonLine);
            }

            String msg = "MARK HIT saved: " + hitId
                    + "\nSession: " + sessionStem
                    + "\nOffset: " + offsetMs + " ms"
                    + "\nSource: " + onlineCarrierLabel()
                    + "\nNo generated words. No word bank. No TTS.";
            Log.i("FieldVox", msg.replace("\n", " | "));
            exportStatus.setText(msg);
            refreshSessions();
        } catch (IOException e) {
            exportStatus.setText("MARK HIT failed: " + e.getMessage());
        }
    }

    private String readWholeText(File file) throws IOException {
        StringBuilder sb = new StringBuilder();
        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(file))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append('\n');
            }
        }
        return sb.toString();
    }

    private String extractJsonString(String json, String key) {
        String needle = "\"" + key + "\":\"";
        int p = json.indexOf(needle);
        if (p < 0) {
            return "";
        }
        p += needle.length();
        StringBuilder out = new StringBuilder();
        boolean esc = false;
        for (int i = p; i < json.length(); i++) {
            char c = json.charAt(i);
            if (esc) {
                out.append(c);
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                break;
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    private long extractJsonLong(String json, String key, long fallback) {
        String needle = "\"" + key + "\":";
        int p = json.indexOf(needle);
        if (p < 0) {
            return fallback;
        }
        p += needle.length();
        int e = p;
        while (e < json.length()) {
            char c = json.charAt(e);
            if ((c >= '0' && c <= '9') || c == '-') {
                e++;
            } else {
                break;
            }
        }
        try {
            return Long.parseLong(json.substring(p, e));
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private void skipFully(FileInputStream in, long bytes) throws IOException {
        long left = bytes;
        while (left > 0) {
            long skipped = in.skip(left);
            if (skipped <= 0) {
                if (in.read() < 0) {
                    break;
                }
                skipped = 1;
            }
            left -= skipped;
        }
    }

    private long copyWavClip(File src, File dst, long centerOffsetMs, int preMs, int postMs) throws IOException {
        long srcBytes = src.length();
        if (srcBytes <= 44L) {
            throw new IOException("source WAV has no PCM data yet");
        }

        long pcmBytes = srcBytes - 44L;
        long bytesPerMs = (long)LIVE_SAMPLE_RATE * 2L / 1000L;
        long startMs = Math.max(0L, centerOffsetMs - preMs);
        long endMs = centerOffsetMs + postMs;
        long startByte = startMs * bytesPerMs;
        long endByte = Math.min(pcmBytes, endMs * bytesPerMs);
        startByte -= startByte % 2L;
        endByte -= endByte % 2L;

        if (endByte <= startByte) {
            startByte = 0L;
            endByte = Math.min(pcmBytes, 20L * 1000L * bytesPerMs);
            endByte -= endByte % 2L;
        }

        long wanted = Math.max(0L, endByte - startByte);
        try (FileInputStream in = new FileInputStream(src);
             java.io.FileOutputStream out = new java.io.FileOutputStream(dst, false)) {
            skipFully(in, 44L + startByte);
            out.write(makeWavHeader(wanted));
            byte[] buf = new byte[64 * 1024];
            long left = wanted;
            while (left > 0L) {
                int n = in.read(buf, 0, (int)Math.min((long)buf.length, left));
                if (n < 0) {
                    break;
                }
                out.write(buf, 0, n);
                left -= n;
            }
        }
        return wanted;
    }

    private String exportHits() {
        File dir = hitMarkerDir();
        File[] files = dir.listFiles();
        if (files == null) {
            return "No hit marker folder found yet.";
        }

        java.util.Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));

        String instructions = "FieldVox Lab blind hit review instructions\n\n"
                + "Do not tell listeners the expected phrase.\n"
                + "Play each blind review clip once or twice.\n"
                + "Ask: write exactly what words, if any, you hear.\n"
                + "Count a candidate hit as stronger only if multiple independent listeners write the same or close phrase.\n"
                + "FieldVox Lab does not prove paranormal activity or spirit communication.\n"
                + "No generated words. No word bank. No TTS. Source material is real online spoken-word stream audio plus microphone recording.\n";

        File instructionsFile = new File(dir, "HIT_REVIEW_INSTRUCTIONS.txt");
        File truthFile = new File(dir, "TRUTH_DO_NOT_SHOW_LISTENERS.tsv");
        StringBuilder truth = new StringBuilder();
        truth.append("hit_id\tsession_stem\thit_offset_ms\tuser_label\tsource_label\tgate_reason\tmetadata_file\tcontext_file\tblind_file\n");

        int hits = 0;
        int exported = 0;
        int clipFailures = 0;
        long bytes = 0L;

        try {
            writeTextFile(instructionsFile, instructions);
        } catch (IOException e) {
            return "Hit instructions write failed: " + e.getMessage();
        }

        for (File hitJson : files) {
            String name = hitJson.getName();
            if (!hitJson.isFile() || !name.startsWith("hit_") || !name.endsWith(".json")) {
                continue;
            }

            hits += 1;
            try {
                String json = readWholeText(hitJson);
                String hitId = extractJsonString(json, "hit_id");
                String sessionStem = extractJsonString(json, "session_stem");
                String label = extractJsonString(json, "user_label");
                String sourceLabel = extractJsonString(json, "source_label");
                String reason = extractJsonString(json, "gate_reason");
                long offsetMs = extractJsonLong(json, "hit_offset_ms", 0L);
                long unixMs = extractJsonLong(json, "unix_ms", System.currentTimeMillis());

                File sessionWav = new File(new File(getFilesDir(), "fieldvox-sessions"), sessionStem + ".wav");
                File contextWav = new File(dir, hitId + "_context.wav");
                File blindWav = new File(dir, "blind_review_clip_" + hits + "_" + unixMs + ".wav");

                String contextName = "";
                String blindName = "";
                if (sessionWav.isFile() && sessionWav.length() > 44L && !(liveMonitorRunning && sessionStem.equals(currentLiveSessionStem))) {
                    copyWavClip(sessionWav, contextWav, offsetMs, 10000, 10000);
                    copyWavClip(sessionWav, blindWav, offsetMs, 10000, 10000);
                    contextName = contextWav.getName();
                    blindName = blindWav.getName();
                } else {
                    clipFailures += 1;
                }

                truth.append(hitId).append('\t')
                        .append(sessionStem).append('\t')
                        .append(offsetMs).append('\t')
                        .append(label.replace('\t', ' ')).append('\t')
                        .append(sourceLabel.replace('\t', ' ')).append('\t')
                        .append(reason.replace('\t', ' ')).append('\t')
                        .append(hitJson.getName()).append('\t')
                        .append(contextName).append('\t')
                        .append(blindName).append('\n');
            } catch (Exception e) {
                clipFailures += 1;
            }
        }

        if (hits == 0) {
            return "No marked hits yet. Tap MARK HIT during or right after a candidate phrase.";
        }

        try {
            writeTextFile(truthFile, truth.toString());
        } catch (IOException e) {
            return "Truth manifest write failed: " + e.getMessage();
        }

        File[] exportFiles = dir.listFiles();
        if (exportFiles == null) {
            return "Hit folder disappeared before export.";
        }

        for (File f : exportFiles) {
            String n = f.getName();
            if (!f.isFile()) {
                continue;
            }
            if (!(n.endsWith(".json") || n.endsWith(".jsonl") || n.endsWith(".wav") || n.endsWith(".txt") || n.endsWith(".tsv"))) {
                continue;
            }
            try {
                String mime = null;
                if (n.endsWith(".wav")) mime = "audio/wav";
                else if (n.endsWith(".txt")) mime = "text/plain";
                else if (n.endsWith(".tsv")) mime = "text/tab-separated-values";
                else if (n.endsWith(".jsonl")) mime = null;
                else if (n.endsWith(".json")) mime = "application/json";
                ExportResult r = exportOneToDownloads(f, mime);
                bytes += r.bytes;
                exported += 1;
            } catch (IOException e) {
                clipFailures += 1;
            }
        }

        String msg = "Exported hit review bundle to Downloads/FieldVoxLab"
                + "\nHits: " + hits
                + "\nFiles exported: " + exported
                + "\nBytes: " + bytes
                + "\nClip skips/failures: " + clipFailures
                + "\nBlind clips use neutral filenames. Do not show TRUTH_DO_NOT_SHOW_LISTENERS.tsv to reviewers.";

        if (liveMonitorRunning) {
            msg += "\nCurrent live session is still running; stop/leave the app and Export Hits again to clip the newest active-session hit.";
        }

        return msg;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleAdbHitMarkerIntent(intent);
    }

    private void handleAdbHitMarkerIntent(Intent intent) {
        if (intent == null) {
            return;
        }

        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) == 0) {
            return;
        }

        String action = intent.getAction();
        Log.i("FieldVox", "ADB intent observed action=" + action
                + " mark_extra=" + intent.getBooleanExtra("fieldvox_mark_hit", false));
        boolean exportRequested = "org.fieldvox.lab.EXPORT_HITS".equals(action)
                || intent.getBooleanExtra("fieldvox_export_hits", false);

        if (exportRequested) {
            Log.i("FieldVox", "ADB EXPORT HITS requested");
            handler.postDelayed(() -> {
                String result = exportHits();
                Log.i("FieldVox", "ADB EXPORT HITS result: " + result.replace("\n", " | "));
                if (exportStatus != null) {
                    exportStatus.setText(result);
                }
                refreshSessions();
            }, 1500L);
            return;
        }

        boolean requested = "org.fieldvox.lab.MARK_HIT".equals(action)
                || intent.getBooleanExtra("fieldvox_mark_hit", false);

        if (!requested) {
            return;
        }

        String label = intent.getStringExtra("fieldvox_label");
        if (label == null) {
            label = "";
        }

        final String finalLabel = label;

        Log.i("FieldVox", "ADB MARK HIT requested label_len=" + finalLabel.length());

        handler.postDelayed(() -> {
            markCandidateHit(finalLabel);
        }, 1200L);
    }

    private void updateLiveButtons() {
        if (liveButton != null) {
            liveButton.setText(liveMonitorRunning ? "Auto Live: On" : "Auto Live: Arming");
        }
        if (startButton != null) {
            startButton.setText(liveMonitorRunning ? "LIVE" : "ARM");
        }
    }

    private String onlineCarrierLabel() {
        int i = Math.max(0, Math.min(onlineCarrierIndex, ONLINE_CARRIER_LABELS.length - 1));
        return ONLINE_CARRIER_LABELS[i];
    }

    private synchronized void openOnlineCarrierGate(String reason, float delta) {
        if (!onlineCarrierActive) {
            android.util.Log.i("FieldVox", "PHONETIC VOX gate skipped: inactive reason=" + reason);
            return;
        }

        if (!onlineCarrierPrepared || onlineCarrierPlayer == null) {
            android.util.Log.i("FieldVox", "PHONETIC VOX gate skipped: carrier not ready reason=" + reason
                    + " prepared=" + onlineCarrierPrepared);
            return;
        }

        long now = System.currentTimeMillis();

        if (onlineCarrierGateOpen || now < onlineCarrierNextAllowedGateMs) {
            long waitMs = Math.max(0L, onlineCarrierNextAllowedGateMs - now);
            android.util.Log.i("FieldVox", "PHONETIC VOX gate skipped: reason=" + reason
                    + " gate_open=" + onlineCarrierGateOpen
                    + " wait_ms=" + waitMs);
            return;
        }

        int grains = 2;
        if (reason.startsWith("proximity")) {
            grains = 2 + (int)(Math.random() * 2.0);
        } else if ("magnetometer".equals(reason)) {
            grains = 2 + Math.min(3, (int)Math.max(0.0f, delta / 2.0f));
        } else if ("gyroscope".equals(reason)) {
            grains = 1 + Math.min(3, (int)Math.max(0.0f, delta * 6.0f));
        } else if ("startup-test".equals(reason)) {
            grains = 2;
        } else if ("auto-scan".equals(reason)) {
            grains = 1 + (int)(Math.random() * 3.0);
        }

        grains = Math.max(1, Math.min(5, grains));
        final int totalGrains = grains;

        int token = ++onlineCarrierGateToken;
        int packetToken = ++onlineCarrierGranularPacketToken;
        onlineCarrierLastSweptToken = 0;
        onlineCarrierGateOpen = true;
        onlineCarrierLastGateMs = now;

        int[] grainDurations = new int[grains];
        int[] grainGaps = new int[grains];

        int packetMs = 0;
        float signature = Math.min(1.0f, Math.abs(delta) / 8.0f);

        for (int i = 0; i < grains; i++) {
            int d = 95 + (int)(Math.random() * 210.0);
            int gap = 45 + (int)(Math.random() * 155.0);

            if (reason.startsWith("proximity")) {
                d += 35;
                gap += 25;
            } else if ("magnetometer".equals(reason)) {
                d += (int)(signature * 80.0f);
            } else if ("gyroscope".equals(reason)) {
                d = Math.max(75, d - 30);
                gap = Math.max(35, gap - 15);
            }

            grainDurations[i] = Math.max(65, Math.min(390, d));
            grainGaps[i] = Math.max(30, Math.min(260, gap));
            packetMs += grainDurations[i];
            if (i + 1 < grains) {
                packetMs += grainGaps[i];
            }
        }

        int forcedSilenceMs = 1100 + (int)(Math.random() * 3600.0);
        if (reason.startsWith("proximity")) {
            forcedSilenceMs += 600;
        }

        onlineCarrierGateOpenUntilMs = now + packetMs;
        onlineCarrierNextAllowedGateMs = now + packetMs + forcedSilenceMs;

        onlineCarrierLastGateReason = reason;
        onlineCarrierLastGateDelta = delta;
        onlineCarrierLastPacketToken = packetToken;
        onlineCarrierLastGrainCount = totalGrains;
        onlineCarrierLastPacketMs = packetMs;
        onlineCarrierLastForcedSilenceMs = forcedSilenceMs;
        onlineCarrierLastPacketUnixMs = now;

        String openMsg = "PHONETIC VOX packet open: " + onlineCarrierLabel()
                + " | Reason: " + reason
                + " delta=" + String.format(java.util.Locale.US, "%.2f", delta)
                + " grains=" + grains
                + " packet_ms=" + packetMs
                + " silence_after_ms=" + forcedSilenceMs
                + " | Real news/talk stream grains. No word bank. No TTS.";
        android.util.Log.i("FieldVox", openMsg);
        exportStatus.setText(openMsg);

        int offsetMs = 0;
        for (int i = 0; i < grains; i++) {
            final int grainIndex = i + 1;
            final int grainStart = offsetMs;
            final int grainDuration = grainDurations[i];

            handler.postDelayed(() -> {
                synchronized (MainActivity.this) {
                    if (token != onlineCarrierGateToken || packetToken != onlineCarrierGranularPacketToken) {
                        return;
                    }
                    if (!onlineCarrierActive || onlineCarrierPlayer == null) {
                        return;
                    }
                }

                try {
                    if (onlineCarrierPlayer != null) {
                        onlineCarrierPlayer.setVolume(1.0f, 1.0f);
                    }
                } catch (Exception ignored) {}

                android.util.Log.i("FieldVox", "PHONETIC VOX grain open: "
                        + onlineCarrierLabel()
                        + " grain=" + grainIndex + "/" + totalGrains
                        + " duration_ms=" + grainDuration
                        + " reason=" + reason
                        + " token=" + token);
            }, grainStart);

            handler.postDelayed(() -> {
                synchronized (MainActivity.this) {
                    if (token != onlineCarrierGateToken || packetToken != onlineCarrierGranularPacketToken) {
                        return;
                    }
                }

                try {
                    if (onlineCarrierPlayer != null) {
                        onlineCarrierPlayer.setVolume(0.0f, 0.0f);
                    }
                } catch (Exception ignored) {}

                android.util.Log.i("FieldVox", "PHONETIC VOX grain closed: grain="
                        + grainIndex + "/" + totalGrains
                        + " token=" + token);
            }, grainStart + grainDuration);

            offsetMs += grainDuration;
            if (i + 1 < grains) {
                offsetMs += grainGaps[i];
            }
        }

        final int totalPacketMs = packetMs;

        handler.postDelayed(() -> {
            synchronized (MainActivity.this) {
                if (token != onlineCarrierGateToken || packetToken != onlineCarrierGranularPacketToken
                        || onlineCarrierLastSweptToken == token) {
                    return;
                }

                onlineCarrierLastSweptToken = token;
                onlineCarrierGateOpen = false;
            }

            try {
                if (onlineCarrierPlayer != null) {
                    onlineCarrierPlayer.setVolume(0.0f, 0.0f);
                }
            } catch (Exception ignored) {}

            String closeMsg = "PHONETIC VOX packet closed. Forced silence gap active. Sweeping to next stream.";
            android.util.Log.i("FieldVox", closeMsg + " token=" + token + " packet_ms=" + totalPacketMs);
            exportStatus.setText(closeMsg);

            handler.postDelayed(() -> advanceOnlineCarrierSweep("phonetic-packet-closed"), 350L);
        }, totalPacketMs);
    }


    private void startOnlineCarrierAutoGate() {
        if (onlineCarrierAutoGateRunnable != null) {
            return;
        }

        onlineCarrierAutoGateRunnable = new Runnable() {
            public void run() {
                if (!onlineCarrierActive) {
                    onlineCarrierAutoGateRunnable = null;
                    return;
                }

                if (onlineCarrierPrepared) {
                    openOnlineCarrierGate("auto-scan", 0.0f);
                }

                int nextDelayMs = 1700 + (int)(Math.random() * 4300.0);
                handler.postDelayed(this, nextDelayMs);
            }
        };

        handler.postDelayed(onlineCarrierAutoGateRunnable, 1200L);
    }

    private void stopOnlineCarrierAutoGate() {
        if (onlineCarrierAutoGateRunnable != null) {
            handler.removeCallbacks(onlineCarrierAutoGateRunnable);
            onlineCarrierAutoGateRunnable = null;
        }
    }


    private synchronized void queueOrOpenProximityGate(String reason, float delta) {
        android.util.Log.i("FieldVox", "proximity gate request reason=" + reason
                + " delta=" + delta
                + " active=" + onlineCarrierActive
                + " prepared=" + onlineCarrierPrepared
                + " gate_open=" + onlineCarrierGateOpen);

        if (onlineCarrierActive && onlineCarrierPrepared && !onlineCarrierGateOpen) {
            onlineCarrierNextAllowedGateMs = Math.min(onlineCarrierNextAllowedGateMs, System.currentTimeMillis());
            openOnlineCarrierGate(reason, delta);
            return;
        }

        onlineCarrierPendingProximityGate = true;
        onlineCarrierPendingProximityReason = reason;
        onlineCarrierPendingProximityDelta = delta;
        android.util.Log.i("FieldVox", "proximity gate queued reason=" + reason + " delta=" + delta);
    }

    private synchronized void flushPendingProximityGateIfReady() {
        if (!onlineCarrierPendingProximityGate) {
            return;
        }

        if (!onlineCarrierActive || !onlineCarrierPrepared || onlineCarrierGateOpen) {
            return;
        }

        String reason = onlineCarrierPendingProximityReason;
        float delta = onlineCarrierPendingProximityDelta;

        onlineCarrierPendingProximityGate = false;
        onlineCarrierPendingProximityReason = "";
        onlineCarrierPendingProximityDelta = 0.0f;

        onlineCarrierNextAllowedGateMs = Math.min(onlineCarrierNextAllowedGateMs, System.currentTimeMillis());
        android.util.Log.i("FieldVox", "proximity queued gate flushing reason=" + reason + " delta=" + delta);
        openOnlineCarrierGate(reason, delta);
    }


    private void startOnlineCarrierSensors() {
        if (onlineCarrierSensorListener != null) {
            return;
        }

        onlineCarrierSensorManager = (SensorManager)getSystemService(SENSOR_SERVICE);
        if (onlineCarrierSensorManager == null) {
            exportStatus.setText("Multi-sensor gate unavailable: no SensorManager.");
            return;
        }

        onlineCarrierRegisteredSensorCount = 0;
        onlineCarrierMagBaseline = -1.0f;
        onlineCarrierGyroBaseline = -1.0f;
        onlineCarrierProximityLast = -1.0f;
        onlineCarrierProximityNear = false;
        onlineCarrierLastProximityLogMs = 0L;
        onlineCarrierPendingProximityGate = false;
        onlineCarrierPendingProximityReason = "";
        onlineCarrierPendingProximityDelta = 0.0f;

        onlineCarrierSensorListener = new SensorEventListener() {
            public void onSensorChanged(SensorEvent event) {
                if (!onlineCarrierActive) {
                    return;
                }

                long now = System.currentTimeMillis();
                int type = event.sensor.getType();

                if (type == Sensor.TYPE_MAGNETIC_FIELD && event.values.length >= 3) {
                    float x = event.values[0];
                    float y = event.values[1];
                    float z = event.values[2];
                    float mag = (float)Math.sqrt(x * x + y * y + z * z);

                    if (onlineCarrierMagBaseline < 0.0f) {
                        onlineCarrierMagBaseline = mag;
                        return;
                    }

                    onlineCarrierMagBaseline = onlineCarrierMagBaseline * 0.985f + mag * 0.015f;
                    float delta = Math.abs(mag - onlineCarrierMagBaseline);

                    if (delta >= Math.max(2.10f, threshold * 0.90f)
                            && now >= onlineCarrierNextAllowedGateMs
                            && now - onlineCarrierLastGateMs >= Math.max(1800, cooldownMs + 1200)) {
                        openOnlineCarrierGate("magnetometer", delta);
                    }
                    return;
                }

                if (type == Sensor.TYPE_GYROSCOPE && event.values.length >= 3) {
                    float x = event.values[0];
                    float y = event.values[1];
                    float z = event.values[2];
                    float rot = (float)Math.sqrt(x * x + y * y + z * z);

                    if (onlineCarrierGyroBaseline < 0.0f) {
                        onlineCarrierGyroBaseline = rot;
                        return;
                    }

                    onlineCarrierGyroBaseline = onlineCarrierGyroBaseline * 0.965f + rot * 0.035f;
                    float delta = Math.abs(rot - onlineCarrierGyroBaseline);

                    // Gyro is movement-sensitive; use it as a secondary trigger, not a spam source.
                    if (delta >= 0.18f
                            && now >= onlineCarrierNextAllowedGateMs
                            && now - onlineCarrierLastGateMs >= Math.max(1900, cooldownMs + 1300)) {
                        openOnlineCarrierGate("gyroscope", delta);
                    }
                    return;
                }

                if (type == Sensor.TYPE_PROXIMITY && event.values.length >= 1) {
                    float v = event.values[0];
                    float maxRange = event.sensor.getMaximumRange();
                    boolean nearNow = (v <= 0.0f) || (v < maxRange);

                    if (onlineCarrierProximityLast < 0.0f) {
                        onlineCarrierProximityLast = v;
                        onlineCarrierProximityNear = nearNow;
                        onlineCarrierLastProximityLogMs = now;
                        android.util.Log.i("FieldVox", "proximity initial near=" + nearNow
                                + " value=" + v
                                + " max=" + maxRange);
                        return;
                    }

                    float delta = Math.abs(v - onlineCarrierProximityLast);
                    boolean changed = nearNow != onlineCarrierProximityNear;
                    onlineCarrierProximityLast = v;
                    onlineCarrierProximityNear = nearNow;

                    if (changed) {
                        android.util.Log.i("FieldVox", "proximity transition near=" + nearNow
                                + " value=" + v
                                + " max=" + maxRange
                                + " delta=" + delta);

                        // Proximity is an intentional user-testable environmental gate.
                        // It may bypass the normal silence cooldown, but still cannot overlap an open gate.
                        queueOrOpenProximityGate(nearNow ? "proximity-near" : "proximity-far", delta);
                    } else if (now - onlineCarrierLastProximityLogMs >= 8000L) {
                        onlineCarrierLastProximityLogMs = now;
                        android.util.Log.i("FieldVox", "proximity steady near=" + nearNow
                                + " value=" + v
                                + " max=" + maxRange);
                    }

                    return;
                }
            }

            public void onAccuracyChanged(Sensor sensor, int accuracy) {}
        };

        Sensor magnetic = onlineCarrierSensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD);
        Sensor gyro = onlineCarrierSensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        Sensor proximity = onlineCarrierSensorManager.getDefaultSensor(Sensor.TYPE_PROXIMITY);

        if (magnetic != null) {
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    magnetic,
                    SensorManager.SENSOR_DELAY_UI);
            onlineCarrierRegisteredSensorCount++;
        }

        if (gyro != null) {
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    gyro,
                    SensorManager.SENSOR_DELAY_UI);
            onlineCarrierRegisteredSensorCount++;
        }

        if (proximity != null) {
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    proximity,
                    SensorManager.SENSOR_DELAY_UI);
            onlineCarrierRegisteredSensorCount++;
        }

        String msg = "Multi-sensor VOX gates armed: "
                + "magnetometer=" + (magnetic != null)
                + " gyroscope=" + (gyro != null)
                + " proximity=" + (proximity != null)
                + " microphone_recording=true";
        android.util.Log.i("FieldVox", msg);
        exportStatus.setText(msg);
    }

    private void stopOnlineCarrierSensors() {
        try {
            if (onlineCarrierSensorManager != null && onlineCarrierSensorListener != null) {
                onlineCarrierSensorManager.unregisterListener(onlineCarrierSensorListener);
            }
        } catch (Exception ignored) {}

        onlineCarrierSensorListener = null;
        onlineCarrierRegisteredSensorCount = 0;
        onlineCarrierMagBaseline = -1.0f;
        onlineCarrierGyroBaseline = -1.0f;
        onlineCarrierProximityLast = -1.0f;
        onlineCarrierProximityNear = false;
    }


    private String startOnlineVoxCarrier() {
        if (onlineCarrierActive && onlineCarrierPlayer != null) {
            return "Online VOX carrier already armed: " + onlineCarrierLabel();
        }

        onlineCarrierActive = true;
        onlineCarrierPrepared = false;
        onlineCarrierMagBaseline = -1.0f;
        onlineCarrierGateOpen = false;
        onlineCarrierNextAllowedGateMs = 0L;
        onlineCarrierGateOpenUntilMs = 0L;
        onlineCarrierLastSweptToken = 0;
        onlineCarrierStartupTestDone = false;
        onlineCarrierSweepCount = 0;
        onlineCarrierIndex = 0;

        startOnlineCarrierSensors();
        prepareOnlineCarrierAtIndex(onlineCarrierIndex);

        return "KIRO/KOMO local news VOX arming. KIRO and Northwest Newsradio KOMO/KNWN are tried first; microphone records the evidence.";
    }

    private int normalizeOnlineCarrierIndex(int index) {
        if (ONLINE_CARRIER_URLS.length <= 0) {
            return 0;
        }
        int n = index % ONLINE_CARRIER_URLS.length;
        if (n < 0) {
            n += ONLINE_CARRIER_URLS.length;
        }
        return n;
    }

    private void advanceOnlineCarrierSweep(String reason) {
        if (!onlineCarrierActive || ONLINE_CARRIER_URLS.length <= 0) {
            return;
        }

        int next = normalizeOnlineCarrierIndex(onlineCarrierIndex + 1);
        onlineCarrierSweepCount++;

        String msg = "VOX sweep advance: "
                + onlineCarrierLabel()
                + " -> " + ONLINE_CARRIER_LABELS[next]
                + "\nReason: " + reason
                + " sweep_count=" + onlineCarrierSweepCount
                + "\nPreparing next online stream during silence.";
        Log.i("FieldVox", msg.replace("\n", " | "));
        exportStatus.setText(msg);

        prepareOnlineCarrierAtIndex(next);
    }

    private void prepareOnlineCarrierAtIndex(int index) {
        if (!onlineCarrierActive) {
            return;
        }

        stopOnlineCarrierPlayerOnly();

        if (ONLINE_CARRIER_URLS.length <= 0) {
            onlineCarrierActive = false;
            exportStatus.setText("Buffered-proximity carrier failed: no stream sources configured.");
            return;
        }

        index = normalizeOnlineCarrierIndex(index);
        onlineCarrierIndex = index;
        onlineCarrierPrepared = false;

        final int prepareToken = ++onlineCarrierPrepareToken;
        final int prepareIndex = index;

        try {
            MediaPlayer mp = new MediaPlayer();
            onlineCarrierPlayer = mp;

            mp.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build());

            mp.setDataSource(ONLINE_CARRIER_URLS[index]);
            mp.setVolume(0.0f, 0.0f);

            mp.setOnPreparedListener(player -> {
                if (prepareToken != onlineCarrierPrepareToken || onlineCarrierPlayer != player) {
                    try { player.release(); } catch (Exception ignored) {}
                    return;
                }

                if (!onlineCarrierActive) {
                    try { player.release(); } catch (Exception ignored) {}
                    return;
                }

                onlineCarrierPrepared = true;
                player.start();

                try {
                    player.setVolume(0.0f, 0.0f);
                } catch (Exception ignored) {}

                String msg = "Timeout-safe carrier ready: " + onlineCarrierLabel()
                        + "\nMuted until phonetic-grain gate opens. Queued proximity can now flush. No word bank. No TTS.";
                Log.i("FieldVox", msg.replace("\n", " | "));
                exportStatus.setText(msg);

                flushPendingProximityGateIfReady();
                startOnlineCarrierAutoGate();
            });

            mp.setOnErrorListener((player, what, extra) -> {
                if (prepareToken != onlineCarrierPrepareToken) {
                    return true;
                }

                try { player.release(); } catch (Exception ignored) {}
                if (onlineCarrierPlayer == player) {
                    onlineCarrierPlayer = null;
                }

                onlineCarrierPrepared = false;

                int next = normalizeOnlineCarrierIndex(onlineCarrierIndex + 1);
                Log.e("FieldVox", "Timeout-safe sweep MediaPlayer error what=" + what + " extra=" + extra
                        + " source=" + onlineCarrierLabel() + " trying_next=" + ONLINE_CARRIER_LABELS[next]);

                handler.postDelayed(() -> prepareOnlineCarrierAtIndex(next), 450L);
                return true;
            });

            Log.i("FieldVox", "Preparing timeout-safe carrier: "
                    + ONLINE_CARRIER_LABELS[index]
                    + " url=" + ONLINE_CARRIER_URLS[index]);
            exportStatus.setText("Preparing timeout-safe carrier: " + ONLINE_CARRIER_LABELS[index]);

            mp.prepareAsync();

            handler.postDelayed(() -> {
                if (!onlineCarrierActive) {
                    return;
                }

                if (prepareToken != onlineCarrierPrepareToken) {
                    return;
                }

                if (onlineCarrierPrepared) {
                    return;
                }

                String label = ONLINE_CARRIER_LABELS[prepareIndex];
                int next = normalizeOnlineCarrierIndex(prepareIndex + 1);

                Log.w("FieldVox", "Carrier prepare timeout: " + label
                        + " -> skipping to " + ONLINE_CARRIER_LABELS[next]
                        + " so queued proximity gates do not stall");

                try {
                    if (onlineCarrierPlayer != null) {
                        onlineCarrierPlayer.release();
                    }
                } catch (Exception ignored) {}

                onlineCarrierPlayer = null;
                onlineCarrierPrepared = false;
                onlineCarrierPrepareToken++;

                handler.postDelayed(() -> prepareOnlineCarrierAtIndex(next), 300L);
            }, 8500L);
        } catch (Exception e) {
            onlineCarrierPrepared = false;
            int next = normalizeOnlineCarrierIndex(index + 1);

            Log.e("FieldVox", "Timeout-safe sweep prepare exception source="
                    + ONLINE_CARRIER_LABELS[index]
                    + " next=" + ONLINE_CARRIER_LABELS[next]
                    + " error=" + (e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()));

            handler.postDelayed(() -> prepareOnlineCarrierAtIndex(next), 450L);
        }
    }



    private void stopOnlineCarrierPlayerOnly() {
        MediaPlayer mp = onlineCarrierPlayer;
        onlineCarrierPlayer = null;
        if (mp != null) {
            try { mp.stop(); } catch (Exception ignored) {}
            try { mp.release(); } catch (Exception ignored) {}
        }
    }

    private void stopOnlineVoxCarrier() {
        stopOnlineCarrierAutoGate();
        onlineCarrierActive = false;
        onlineCarrierPrepared = false;
        onlineCarrierPrepareToken++;
        onlineCarrierGateOpen = false;
        onlineCarrierNextAllowedGateMs = 0L;
        onlineCarrierGateOpenUntilMs = 0L;
        onlineCarrierGateToken++;
        onlineCarrierStartupTestDone = false;
        stopOnlineCarrierSensors();
        stopOnlineCarrierPlayerOnly();
    }



    private void autoStartEverythingOnLaunch() {
        if (liveMonitorRunning) {
            updateLiveButtons();
            return;
        }

        speakerMonitorEnabled = false;
        updateSpeakerMonitorButton();

        if (!hasMicPermission()) {
            requestMicPermissionForLiveMonitor();
            String msg = "Microphone permission needed once. After permission is granted, FieldVox opens directly into live capture.";
            exportStatus.setText(msg);
            status.setText(msg);
            updateLiveButtons();
            return;
        }

        String result = startLiveMonitor();
        String carrier = startOnlineVoxCarrier();
        exportStatus.setText("KIRO/KOMO local news VOX + live capture started.\n" + result + "\n" + carrier);
        status.setText(result + "\n" + carrier);
        updateLiveButtons();
    }




    private void updateSpeakerMonitorButton() {
        if (speakerMonitorButton != null) {
            speakerMonitorButton.setText(speakerMonitorEnabled ? "Speaker Monitor: On" : "Speaker Monitor: Off");
        }
    }

    private void toggleSpeakerMonitor() {
        speakerMonitorEnabled = !speakerMonitorEnabled;
        updateSpeakerMonitorButton();
        String msg = speakerMonitorEnabled
                ? "Speaker Monitor ON. You may hear raw mic noise/hiss. Turn it OFF for silent listening/recording."
                : "Speaker Monitor OFF. Live Monitor still records actual mic audio silently. This is the recommended default.";
        exportStatus.setText(msg);
        status.setText(msg);
    }

    private short boostedSample(short in) {
        int v = Math.round((float)in * liveOutputGain);
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return (short)v;
    }


    private boolean hasMicPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestMicPermissionForLiveMonitor() {
        requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, LIVE_PERMISSION_REQUEST);
    }

    private static byte[] makeWavHeader(long pcmBytes) {
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(44);
        b.order(java.nio.ByteOrder.LITTLE_ENDIAN);

        int byteRate = LIVE_SAMPLE_RATE * 2;
        int blockAlign = 2;
        int riffSize = (int)Math.min(36L + pcmBytes, 0x7fffffffL);
        int dataSize = (int)Math.min(pcmBytes, 0x7fffffffL);

        b.put(new byte[]{'R','I','F','F'});
        b.putInt(riffSize);
        b.put(new byte[]{'W','A','V','E'});
        b.put(new byte[]{'f','m','t',' '});
        b.putInt(16);
        b.putShort((short)1);
        b.putShort((short)1);
        b.putInt(LIVE_SAMPLE_RATE);
        b.putInt(byteRate);
        b.putShort((short)blockAlign);
        b.putShort((short)16);
        b.put(new byte[]{'d','a','t','a'});
        b.putInt(dataSize);
        return b.array();
    }

    private static void writeJsonLine(java.io.FileOutputStream out, String line) throws IOException {
        out.write(line.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        out.write('\n');
        out.flush();
    }

    private static void rewriteWavHeader(File wav, long pcmBytes) throws IOException {
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(wav, "rw")) {
            raf.seek(0);
            raf.write(makeWavHeader(pcmBytes));
        }
    }

    private String startLiveMonitor() {
        if (liveMonitorRunning) {
            return "Realtime EVP monitor already running.";
        }

        if (!hasMicPermission()) {
            requestMicPermissionForLiveMonitor();
            return "Microphone permission requested. Allow it, then tap Live Monitor again.";
        }

        File dir = new File(getFilesDir(), "fieldvox-sessions");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            return "Could not create session folder.";
        }

        final long startMs = System.currentTimeMillis();
        final String stem = "session-" + (startMs / 1000L) + "-live";
        final File jsonl = new File(dir, stem + ".jsonl");
        final File wav = new File(dir, stem + ".wav");

        currentLiveSessionStem = stem;
        currentLiveSessionStartMs = startMs;
        currentLiveJsonlFile = jsonl;
        currentLiveWavFile = wav;
        synchronized (hitMarkerLock) {
            pendingHitJsonLines.clear();
        }

        liveMonitorRunning = true;
        liveNoiseFloor = 300.0;
        liveLastTriggerMs = 0L;

        if (liveButton != null) {
            updateLiveButtons();
        }

        liveMonitorThread = new Thread(() -> {
            AudioRecord record = null;
            AudioTrack track = null;
            long pcmBytes = 0L;
            long lastUiMs = 0L;

            try (java.io.FileOutputStream jsonOut = new java.io.FileOutputStream(jsonl, false);
                 java.io.FileOutputStream wavOut = new java.io.FileOutputStream(wav, false)) {

                writeJsonLine(jsonOut,
                        "{\"event\":\"live_monitor_start\",\"mode\":\"realtime_evp_monitor\",\"sample_rate\":48000,"
                        + "\"source\":\"microphone\",\"online_carrier\":true,\"carrier\":\"kiro_komo_local_news_talk_phonetic_sweep\",\"generated_words\":false,\"word_bank\":false,"
                        + "\"note\":\"actual microphone audio only; no app-generated speech\",\"unix_ms\":"
                        + startMs + "}");

                wavOut.write(makeWavHeader(0));

                int minIn = AudioRecord.getMinBufferSize(
                        LIVE_SAMPLE_RATE,
                        AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT);

                int minOut = AudioTrack.getMinBufferSize(
                        LIVE_SAMPLE_RATE,
                        AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT);

                int bufferBytes = Math.max(4096, Math.max(minIn, minOut) * 2);
                if (bufferBytes < 4096) {
                    bufferBytes = 4096;
                }

                record = new AudioRecord(
                        MediaRecorder.AudioSource.MIC,
                        LIVE_SAMPLE_RATE,
                        AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT,
                        bufferBytes);

                if (record.getState() != AudioRecord.STATE_INITIALIZED) {
                    throw new IOException("AudioRecord failed to initialize.");
                }

                track = new AudioTrack(
                        AudioManager.STREAM_MUSIC,
                        LIVE_SAMPLE_RATE,
                        AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT,
                        bufferBytes,
                        AudioTrack.MODE_STREAM);

                short[] samples = new short[bufferBytes / 2];
                short[] monitorSamples = new short[bufferBytes / 2];
                byte[] pcm = new byte[bufferBytes];

                record.startRecording();
                track.setVolume(1.0f);
                track.play();

                handler.post(() -> {
                    status.setText("Realtime EVP monitor started\\nUse headphones to avoid feedback.\\nNo word bank. No generated speech.");
                    refreshSessions();
                });

                while (liveMonitorRunning) {
                    int n = record.read(samples, 0, samples.length);
                    if (n <= 0) {
                        continue;
                    }

                    long sumSquares = 0L;
                    int peak = 0;

                    for (int i = 0; i < n; i++) {
                        int v = samples[i];
                        int av = Math.abs(v);
                        if (av > peak) {
                            peak = av;
                        }
                        sumSquares += (long)v * (long)v;
                        pcm[i * 2] = (byte)(v & 0xff);
                        pcm[i * 2 + 1] = (byte)((v >> 8) & 0xff);
                        monitorSamples[i] = boostedSample(samples[i]);
                    }

                    wavOut.write(pcm, 0, n * 2);
                    flushPendingHitMarkers(jsonOut);
                    pcmBytes += (long)n * 2L;

                    if (speakerMonitorEnabled) {
                        track.write(monitorSamples, 0, n);
                    }

                    double rms = Math.sqrt((double)sumSquares / (double)n);
                    if (rms > liveNoiseFloor) {
                        liveNoiseFloor = liveNoiseFloor * 0.95 + rms * 0.05;
                    } else {
                        liveNoiseFloor = liveNoiseFloor * 0.995 + rms * 0.005;
                    }

                    double gate = Math.max(900.0, liveNoiseFloor * 3.0);
                    long nowMs = System.currentTimeMillis();

                    if (rms >= gate && nowMs - liveLastTriggerMs >= Math.max(250, cooldownMs)) {
                        liveLastTriggerMs = nowMs;
                        long offsetMs = nowMs - startMs;
                        writeJsonLine(jsonOut,
                                "{\"event\":\"trigger\",\"mode\":\"realtime_evp_monitor\",\"source\":\"microphone\","
                                + "\"generated_words\":false,\"word_bank\":false,"
                                + "\"offset_ms\":" + offsetMs
                                + ",\"rms\":" + String.format(java.util.Locale.US, "%.2f", rms)
                                + ",\"peak\":" + peak
                                + ",\"gate\":" + String.format(java.util.Locale.US, "%.2f", gate)
                                + ",\"unix_ms\":" + nowMs + "}");
                    }

                    if (nowMs - lastUiMs > 700L) {
                        lastUiMs = nowMs;
                        final int seconds = (int)((nowMs - startMs) / 1000L);
                        final String msg = "Realtime EVP monitor: " + seconds + "s\\n"
                                + "Mic -> live audio output. Use headphones.\\n"
                                + "RMS " + String.format(java.util.Locale.US, "%.0f", rms)
                                + " / gate " + String.format(java.util.Locale.US, "%.0f", gate)
                                + " / peak " + peak + "\\n"
                                + "No word bank. No generated speech.";
                        handler.post(() -> status.setText(msg));
                    }
                }

                long stopMs = System.currentTimeMillis();
                writeJsonLine(jsonOut,
                        "{\"event\":\"live_monitor_stop\",\"mode\":\"realtime_evp_monitor\",\"pcm_bytes\":"
                        + pcmBytes + ",\"duration_ms\":" + (stopMs - startMs)
                        + ",\"generated_words\":false,\"word_bank\":false,\"unix_ms\":" + stopMs + "}");

            } catch (Exception e) {
                final String err = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                handler.post(() -> status.setText("Realtime EVP monitor error: " + err));
            } finally {
                try {
                    if (record != null) {
                        try { record.stop(); } catch (Exception ignored) {}
                        record.release();
                    }
                } catch (Exception ignored) {}

                try {
                    if (track != null) {
                        try { track.stop(); } catch (Exception ignored) {}
                        track.release();
                    }
                } catch (Exception ignored) {}

                try {
                    rewriteWavHeader(wav, pcmBytes);
                } catch (Exception ignored) {}

                liveMonitorRunning = false;
                handler.post(() -> {
                    if (liveButton != null) {
                        updateLiveButtons();
                    }
                    updateLiveButtons();
                    refreshSessions();
                    exportStatus.setText("Auto live session saved:\\n" + jsonl.getName() + "\\n" + wav.getName());
                });
            }
        }, "FieldVox-Realtime-EVP-Monitor");

        liveMonitorThread.start();

        return "KIRO/KOMO local news VOX starting. No word bank; no TTS.";
    }

    private String stopLiveMonitor() {
        if (!liveMonitorRunning) {
            updateLiveButtons();
            return "Auto live session is not running.";
        }

        liveMonitorRunning = false;
        updateLiveButtons();
        return "Auto live session finalizing WAV header...";
    }


    private void toggleLiveMonitor() {
        String msg;
        if (liveMonitorRunning) {
            msg = "Auto Live is already running.\nNo Stop button needed. Leave or close the app to finalize the WAV.";
        } else {
            autoStartEverythingOnLaunch();
        handleAdbHitMarkerIntent(getIntent());
            msg = "Auto Live is arming.";
        }
        exportStatus.setText(msg);
        status.setText(msg);
        updateLiveButtons();
    }


    private void toggleRun() {
        String msg;
        if (liveMonitorRunning) {
            msg = "Hands-free auto mode is already live.\nNo Start/Stop needed. Leave or close the app to finalize the session.";
        } else {
            autoStartEverythingOnLaunch();
            msg = "Hands-free auto mode is arming.\nNo Start/Stop needed.";
        }
        exportStatus.setText(msg);
        status.setText(msg);
        updateLiveButtons();
    }



    private void toggleControls() {
        controlsVisible = !controlsVisible;
        controlPanel.setVisibility(controlsVisible ? View.VISIBLE : View.GONE);
    }

    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        nativeInit(getPackageName(), getFilesDir().getAbsolutePath());
        nativeSetThreshold(threshold);
        nativeSetCooldownMs(cooldownMs);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(this, 18), dp(this, 24), dp(this, 18), dp(this, 18));
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.BLACK);

        TextView title = makeLabel("FIELDVOX", Color.WHITE, 40);
        title.setShadowLayer(12.0f, 0.0f, 0.0f, Color.WHITE);
        root.addView(title);

        TextView subtitle = makeLabel("LAB", Color.WHITE, 28);
        subtitle.setShadowLayer(8.0f, 0.0f, 0.0f, Color.WHITE);
        root.addView(subtitle);

        TextView stage = makeLabel("Stage 1M-J • KIRO/KOMO local news VOX", Color.LTGRAY, 12);
        stage.setPadding(0, dp(this, 8), 0, dp(this, 18));
        root.addView(stage);

        LogoGlyphView glyph = new LogoGlyphView(this);
        root.addView(glyph, new LinearLayout.LayoutParams(dp(this, 132), dp(this, 132)));

        wavePanel = new WavePanelView(this);
        LinearLayout.LayoutParams waveParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 150)
        );
        waveParams.setMargins(0, dp(this, 34), 0, dp(this, 20));
        root.addView(wavePanel, waveParams);

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setGravity(Gravity.CENTER);
        bottom.setPadding(0, dp(this, 6), 0, dp(this, 8));

        sessionsButton = makeCircleButton("Sess");
        exportButton = makeCircleButton("Export");
        startButton = makeCircleButton("Start");
        shareButton = makeCircleButton("Share");
        controlsButton = makeCircleButton("Ctrl");

        int small = dp(this, 62);
        int big = dp(this, 72);
        int gap = dp(this, 2);

        LinearLayout.LayoutParams smallLp = new LinearLayout.LayoutParams(small, small);
        smallLp.setMargins(gap, 0, gap, 0);

        LinearLayout.LayoutParams bigLp = new LinearLayout.LayoutParams(big, big);
        bigLp.setMargins(gap, 0, gap, 0);

        bottom.addView(sessionsButton, smallLp);
        bottom.addView(exportButton, smallLp);
        bottom.addView(startButton, bigLp);
        bottom.addView(shareButton, smallLp);
        bottom.addView(controlsButton, smallLp);

        root.addView(bottom);

        exportStatus = makeLabel("Local news/talk grains • no word bank/TTS", Color.rgb(255, 230, 160), 12);
        exportStatus.setPadding(0, dp(this, 4), 0, dp(this, 8));
        root.addView(exportStatus);

        controlPanel = new LinearLayout(this);
        controlPanel.setOrientation(LinearLayout.VERTICAL);
        controlPanel.setVisibility(View.GONE);

        thresholdLabel = makeLabel("", Color.WHITE, 13);
        cooldownLabel = makeLabel("", Color.WHITE, 13);

        SeekBar thresholdBar = new SeekBar(this);
        thresholdBar.setMax(950);
        thresholdBar.setProgress(115);

        SeekBar cooldownBar = new SeekBar(this);
        cooldownBar.setMax(1900);
        cooldownBar.setProgress(350);

        Button exportAllButton = makeWideButton("Export All Sessions");
        exportAllButton.setTextSize(14);
        exportAllButton.setPadding(0, 0, 0, 0);

        Button shareAllButton = makeWideButton("Share All Sessions");
        shareAllButton.setTextSize(14);
        shareAllButton.setPadding(0, 0, 0, 0);

        Button manifestButton = makeWideButton("Export Manifest");
        manifestButton.setTextSize(14);
        manifestButton.setPadding(0, 0, 0, 0);

        liveButton = makeWideButton("Live Monitor");
        liveButton.setTextSize(14);
        liveButton.setPadding(0, 0, 0, 0);

        speakerMonitorButton = makeWideButton("Speaker Monitor: Off");
        speakerMonitorButton.setTextSize(14);
        speakerMonitorButton.setPadding(0, 0, 0, 0);

        controlPanel.addView(exportAllButton);
        controlPanel.addView(shareAllButton);
        controlPanel.addView(manifestButton);
        controlPanel.addView(liveButton);
        controlPanel.addView(speakerMonitorButton);
        controlPanel.addView(thresholdLabel);
        controlPanel.addView(thresholdBar);
        controlPanel.addView(cooldownLabel);
        controlPanel.addView(cooldownBar);

        updateLabels();

        root.addView(controlPanel);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(12);
        status.setPadding(0, dp(this, 12), 0, 0);
        status.setText(nativeStatus());

        sessions = new TextView(this);
        sessions.setTextColor(Color.rgb(210, 240, 255));
        sessions.setTextSize(12);
        sessions.setPadding(0, dp(this, 12), 0, dp(this, 24));
        sessions.setText(nativeSessionList());

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.addView(status);
        markHitButton = makeWideButton("MARK HIT");
        markHitButton.setTextSize(18);
        markHitButton.setContentDescription("MARK HIT");
        markHitButton.setBackgroundColor(Color.rgb(92, 30, 30));
        content.addView(markHitButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 62)));

        exportHitsButton = makeWideButton("Export Hits");
        exportHitsButton.setTextSize(16);
        exportHitsButton.setContentDescription("Export Hits");
        content.addView(exportHitsButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 54)));

        content.addView(sessions);


        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                1.0f
        ));

        setContentView(root);

        thresholdBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                threshold = 0.50f + ((float) progress / 100.0f);
                nativeSetThreshold(threshold);
                updateLabels();
            }

            public void onStartTrackingTouch(SeekBar seekBar) {}

            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        cooldownBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                cooldownMs = 100 + progress;
                nativeSetCooldownMs(cooldownMs);
                updateLabels();
            }

            public void onStartTrackingTouch(SeekBar seekBar) {}

            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        startButton.setOnClickListener(v -> toggleRun());

        sessionsButton.setOnClickListener(v -> {
            refreshSessions();
            exportStatus.setText("Session list refreshed.");
        });

        if (markHitButton != null) {
            markHitButton.setOnClickListener(v -> promptAndMarkHit());
        }

        if (exportHitsButton != null) {
            exportHitsButton.setOnClickListener(v -> {
                String result = exportHits();
                exportStatus.setText(result);
                refreshSessions();
                status.setText(nativeStatus());
            });
        }

        exportButton.setOnClickListener(v -> {
            String result = exportLatestPair();
            exportStatus.setText(result);
            refreshSessions();
            status.setText(nativeStatus());
        });

        shareButton.setOnClickListener(v -> {
            String result = shareLatestPair();
            exportStatus.setText(result);
            refreshSessions();
            status.setText(nativeStatus());
        });

        controlsButton.setOnClickListener(v -> toggleControls());

        exportAllButton.setOnClickListener(v -> {
            String result = exportAllPairs();
            exportStatus.setText(result);
            refreshSessions();
            status.setText(nativeStatus());
        });

        shareAllButton.setOnClickListener(v -> {
            String result = shareAllPairs();
            exportStatus.setText(result);
            refreshSessions();
            status.setText(nativeStatus());
        });

        manifestButton.setOnClickListener(v -> {
            String result = exportSessionManifest();
            exportStatus.setText(result);
            refreshSessions();
            status.setText(nativeStatus());
        });

        liveButton.setOnClickListener(v -> toggleLiveMonitor());
        speakerMonitorButton.setOnClickListener(v -> toggleSpeakerMonitor());
        updateSpeakerMonitorButton();

        handler.post(tick);
    
        handler.postDelayed(() -> autoStartEverythingOnLaunch(), 700);
}


    protected void onResume() {
        super.onResume();
        // ADB_MARK_HIT_EXISTING_ONRESUME_POLL: deterministic test hook because UIAutomator cannot idle during live VOX.
        handler.postDelayed(() -> handleAdbHitMarkerIntent(getIntent()), 900L);
        handler.postDelayed(() -> autoStartEverythingOnLaunch(), 350);
    }


    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode == LIVE_PERMISSION_REQUEST) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                handler.postDelayed(() -> autoStartEverythingOnLaunch(), 250);
            } else {
                String msg = "Microphone permission denied. FieldVox cannot auto-start live capture without mic permission.";
                exportStatus.setText(msg);
                status.setText(msg);
                updateLiveButtons();
            }
        }
    }


    protected void onStop() {
        stopOnlineVoxCarrier();
        liveMonitorRunning = false;
        updateLiveButtons();
        super.onStop();
    }

    protected void onDestroy() {
        stopOnlineVoxCarrier();
        liveMonitorRunning = false;
        nativeStop();
        handler.removeCallbacks(tick);
        super.onDestroy();
    }

    public static final class LogoGlyphView extends View {
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint soft = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);

        public LogoGlyphView(Activity ctx) {
            super(ctx);

            stroke.setColor(Color.WHITE);
            stroke.setStrokeWidth(6.0f);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeCap(Paint.Cap.ROUND);
            stroke.setStrokeJoin(Paint.Join.ROUND);

            soft.setColor(Color.WHITE);
            soft.setStrokeWidth(3.0f);
            soft.setStyle(Paint.Style.STROKE);
            soft.setStrokeCap(Paint.Cap.ROUND);
            soft.setAlpha(110);

            fill.setColor(Color.WHITE);
            fill.setStyle(Paint.Style.FILL);
        }

        protected void onDraw(Canvas c) {
            super.onDraw(c);

            float w = getWidth();
            float h = getHeight();
            float cx = w * 0.5f;
            float cy = h * 0.5f;
            float r = Math.min(w, h) * 0.36f;

            // Original FieldVox signal mark:
            // sensor ring + compass ticks + waveform core + antenna spine.
            // Intentionally no ghost/body silhouette.

            c.drawCircle(cx, cy, r, stroke);
            c.drawCircle(cx, cy, r * 0.63f, soft);

            // Compass/sensor ticks.
            c.drawLine(cx, cy - r - h * 0.08f, cx, cy - r + h * 0.06f, stroke);
            c.drawLine(cx, cy + r - h * 0.06f, cx, cy + r + h * 0.08f, stroke);
            c.drawLine(cx - r - w * 0.08f, cy, cx - r + w * 0.06f, cy, stroke);
            c.drawLine(cx + r - w * 0.06f, cy, cx + r + w * 0.08f, cy, stroke);

            // Diagonal pulse ticks.
            float d = r * 0.73f;
            float e = r * 1.08f;
            c.drawLine(cx - e, cy - e, cx - d, cy - d, soft);
            c.drawLine(cx + d, cy - d, cx + e, cy - e, soft);
            c.drawLine(cx - e, cy + e, cx - d, cy + d, soft);
            c.drawLine(cx + d, cy + d, cx + e, cy + e, soft);

            // Central waveform.
            float baseY = cy;
            float left = cx - r * 0.68f;
            float step = (r * 1.36f) / 8.0f;
            float[] amps = new float[] {
                    0.10f, -0.34f, 0.22f, -0.52f, 0.50f, -0.20f, 0.35f, -0.12f, 0.08f
            };

            float lastX = left;
            float lastY = baseY + amps[0] * r;
            for (int i = 1; i < amps.length; i++) {
                float x = left + step * i;
                float y = baseY + amps[i] * r;
                c.drawLine(lastX, lastY, x, y, stroke);
                lastX = x;
                lastY = y;
            }

            // Antenna spine and field dots.
            c.drawLine(cx, cy - r * 0.25f, cx, cy + r * 0.55f, soft);
            c.drawCircle(cx, cy, r * 0.08f, fill);

            c.drawCircle(cx - r * 0.48f, cy - r * 0.42f, r * 0.055f, fill);
            c.drawCircle(cx + r * 0.50f, cy + r * 0.34f, r * 0.055f, fill);
            c.drawCircle(cx + r * 0.38f, cy - r * 0.50f, r * 0.04f, fill);
            c.drawCircle(cx - r * 0.34f, cy + r * 0.48f, r * 0.04f, fill);
        }
    }

    public static final class WavePanelView extends View {
        private final Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG);
        private boolean running = false;
        private int phase = 0;

        public WavePanelView(Activity ctx) {
            super(ctx);
            bg.setColor(Color.rgb(18, 18, 20));
            bg.setStyle(Paint.Style.FILL);

            glow.setColor(Color.WHITE);
            glow.setStrokeWidth(10.0f);
            glow.setAlpha(60);
            glow.setStrokeCap(Paint.Cap.ROUND);

            line.setColor(Color.WHITE);
            line.setStrokeWidth(6.0f);
            line.setStrokeCap(Paint.Cap.ROUND);
        }

        public void setRunning(boolean r) {
            running = r;
            phase = (phase + 1) % 1000;
        }

        protected void onDraw(Canvas c) {
            super.onDraw(c);

            float w = getWidth();
            float h = getHeight();
            RectF box = new RectF(0, 0, w, h);
            c.drawRoundRect(box, 18.0f, 18.0f, bg);

            float mid = h * 0.52f;
            int bars = 34;
            float gap = w / (bars + 1);

            for (int i = 0; i < bars; i++) {
                float x = gap * (i + 1);
                float a = (float)Math.sin((i * 0.75f) + (phase * 0.28f));
                float b = (float)Math.cos((i * 0.39f) + (phase * 0.18f));
                float mag = Math.abs(a * 0.55f + b * 0.45f);
                if (!running) {
                    mag = 0.42f + (mag * 0.18f);
                }
                float amp = 20.0f + mag * (h * 0.28f);
                c.drawLine(x, mid - amp, x, mid + amp, glow);
                c.drawLine(x, mid - amp, x, mid + amp, line);
            }

            if (running) {
                postInvalidateDelayed(60);
            }
        }
    }
}
