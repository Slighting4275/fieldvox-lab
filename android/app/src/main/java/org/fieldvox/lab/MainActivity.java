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
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
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
import android.widget.FrameLayout;
import android.content.res.Configuration;
import android.widget.TextView;
import android.widget.SeekBar;
import android.widget.EditText;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.widget.Toast;
import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.Locale;

public class MainActivity extends Activity {

    // FIELDVOX_STAGE1R_NATIVE_AUDIO_WINDOW
    static {
        try {
            System.loadLibrary("fieldvox_stage1r_native");
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private native int nativePlayStage1RShortBurst(int durationMs, int frequencyHz, float amplitude);

    // FIELDVOX_STAGE1R_B_NATIVE_WINDOW_SEQUENCER
    private native int nativePlayStage1RWindowSequence(int burstCount, int burstMs, int gapMs, float amplitude);

    // FIELDVOX_STAGE1S_A_NATIVE_MIC_METRICS
    private native String nativeProbeStage1SMicWindow(int durationMs);

    // FIELDVOX_STAGE1S_B_NATIVE_MIC_BASELINE
    private native String nativeProbeStage1SMicBaseline(int windowCount, int windowMs, int gapMs);


    // FIELDVOX_STAGE1T_A_BOUNDED_VOICE_FIELD_TEST
    // FIELDVOX_STAGE1T_B_FORCE_BOUNDED_GRAIN_FIELD_TEST
    // FIELDVOX_STAGE1U_A_NATIVE_BLOB_1000_RING
    // FIELDVOX_STAGE1U_E_NATIVE_BLOB_CLASS_SCOPE_WIRE
    // FIELDVOX_STAGE1U_K_BEAUTIFY_TINY_VISIBLE_CHANNEL
    // FIELDVOX_STAGE1U_L_FAST_NATIVE_SWEEP
    // FIELDVOX_STAGE1U_M_ONE_SECOND_NATIVE_SWEEP
    // FIELDVOX_STAGE1U_N_AUDIO_DWELL_SINGLE_GUI_TICK
    // Visible channel clock remains 1000 ms; network audio source dwell is 6000 ms to allow buffering.
    // Native C now defines a user-tuned 1000 ms/channel cadence.
    // Native C defines a user-tuned 1000 ms/channel cadence.
    // Existing visible status area becomes a small three-digit channel number box; app controls get dark/neon polish.
    // Runtime source selection uses the native C binary station blob ring.
    // Native C binary station-blob ring: 1000 logical stations, continuous sweep scaffold.
    // Temporary forced bounded source-grain field test; no generated words and no proof claims.
    // Temporary bounded source-grain field test; no generated words and no proof claims.
    // FIELDVOX_STAGE1S_D_C_UI_TEXT_KILL
    // Stale carrier UI labels removed by C patcher.
    // FIELDVOX_STAGE1S_C_NATIVE_CARRIER_KILL
    private native boolean nativeStage1SCDisableLegacyCarrier();
    private native void nativeStage1SCNoteLegacyCarrierBlocked(String methodName);

    private native int nativeStage1UStationRingCount();
    private native String nativeStage1UStationAt(int logicalIndex);
    private native String nativeStage1UDecoderProbe();
    private native int nativeStage1ULFastSweepPeriodMs();
    private native int nativeStage1UNAudioDwellPeriodMs();

    // Native C / Zig replacements for Java DSP, Sensors, and Station Blob
    private native void nativeResetAudioDsp();
    private native void nativeProcessAudioDsp(
            short[] samples,
            byte[] pcm,
            short[] monitor,
            int count,
            boolean reverseEnabled,
            boolean echoEnabled,
            boolean noiseEnabled,
            float boostFactor,
            float[] metrics);

    private native void nativeResetSensorTelemetry();
    private native boolean nativeUpdateSensorTelemetry(
            int sensorType,
            float x,
            float y,
            float z,
            float threshold,
            float[] outTelemetry);

    private native int nativeGetStationCount();
    private native String nativeGetStationLabel(int slot);
    private native String nativeGetStationCallsign(int slot);
    private native String nativeGetStationUrl(int slot);

    private int stage1UNativeLogicalCursor = 0;
    private int stage1ULastRingCount = 0;
    private String stage1ULastNativeStationJson = "";
    private String stage1ULastNativeStationLabel = "";
    private String stage1ULastStreamUrlSha256 = "";
    private android.os.Handler stage1ULFastSweepHandler = null;
    private java.lang.Runnable stage1ULFastSweepRunnable = null;
    private boolean stage1ULFastSweepStarted = false;
    private int stage1ULFastSweepCursor = 0;
    private android.os.Handler stage1UNAudioDwellHandler = null;
    private java.lang.Runnable stage1UNAudioDwellRunnable = null;
    private boolean stage1UNAudioDwellStarted = false;
    private boolean stage1UProbeLogged = false;


    // STAGE_1Q_D_ONE_STREAM_LIVE_PLAYBACK
    private TextView liveStreamStatusText;
    private Button liveStreamButton;
    private MediaPlayer liveStreamPlayer;
    private FieldVoxLiveSource currentLiveSource;
    private boolean liveStreamActive = false;

    // FIELDVOX_STAGE1QE_SHORT_DWELL_HARD_STOP
    private static final long FIELDVOX_STAGE1QD_DWELL_MS = 5000L;

    private static final class FieldVoxLiveSource {
        String stationuuid = "";
        String countrycode = "";
        String sourceLabel = "";
        String codec = "";
        String bitrate = "";
        String streamUrlSha256 = "";
        String streamUrlResolvedSha256 = "";
        String privateUrl = "";
        String privateUrlResolved = "";
    }


    static {
        try {
            System.loadLibrary("fieldvox");
        } catch (UnsatisfiedLinkError ignored) {
        }
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
    private volatile AudioRecord liveAudioRecord;
    private boolean micPermissionRequestPending = false;
    private volatile boolean speakerMonitorEnabled = false;
    private volatile Thread liveMonitorThread;
    private Button speakerMonitorButton;
    private Button markHitButton;
    private Button exportHitsButton;

    // GhostTube-Style EMF & Audio Suite
    private EmfMeterView emfMeterView;
    private TextView channelBadgeView;
    private int userSweepCadenceMs = 250;
    private int userSweepDirection = 1; // 1 = FWD, -1 = REV, 0 = RND
    private volatile boolean userEchoFxEnabled = false;
    private volatile boolean userWhiteNoiseEnabled = false;
    private volatile boolean userReverseAudioEnabled = false;

    private Button sweepCadenceButton;
    private Button sweepDirectionButton;
    private Button audioFxEchoButton;
    private Button audioNoiseButton;
    private Button audioRevButton;

    // FIELDVOX_STAGE1Q_C_VISIBLE_SOURCE_CATALOG
    private android.widget.Button liveSourcesButton;
    private android.widget.TextView liveSourcesStatusView;
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
    private long onlineCarrierLastWaitingLogMs = 0L;
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

    private int getOnlineStationCount() {
        try {
            int cnt = nativeGetStationCount();
            if (cnt > 0) return cnt;
        } catch (Throwable ignored) {}
        return 64;
    }

    private String getOnlineStationUrl(int index) {
        try {
            String url = nativeGetStationUrl(index);
            if (url != null && !url.isEmpty()) return url;
        } catch (Throwable ignored) {}
        return "";
    }

    private String getOnlineStationLabel(int index) {
        try {
            String label = nativeGetStationLabel(index);
            if (label != null && !label.isEmpty()) return label;
        } catch (Throwable ignored) {}
        return "Station #" + (index + 1);
    }


    private static final int LIVE_SAMPLE_RATE = 48000;
    private static final int LIVE_PERMISSION_REQUEST = 4101;
    private static final int MAX_HIT_LABEL_LENGTH = 512;
    private static final long WAV_MAX_PCM_BYTES = 0xffffffffL - 36L;


    private native String nativeTimelineRecord(String eventKind, String detail);
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
                status.setText("native audio safety mode running\nBounded station-ring audio is active. No word bank. No TTS.");
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
        b.setTextSize(12);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setAllCaps(false);
        b.setSingleLine(true);
        b.setEllipsize(TextUtils.TruncateAt.END);
        b.setGravity(Gravity.CENTER);
        b.setIncludeFontPadding(false);
        b.setBackground(stage1KRoundRect(0xFF151722, 0xFF6FE7FF, 12.0f, 1));
        b.setPadding(dp(this, 4), dp(this, 6), dp(this, 4), dp(this, 6));
        return b;
    }

    private Button makePillButton(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextColor(0xFFE8F7FF);
        b.setTextSize(10.0f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setAllCaps(false);
        b.setSingleLine(true);
        b.setGravity(Gravity.CENTER);
        b.setIncludeFontPadding(false);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setBackground(stage1KRoundRect(0xFF101622, 0xFF384E6C, 8.0f, 1));
        b.setPadding(dp(this, 1), dp(this, 2), dp(this, 1), dp(this, 2));
        return b;
    }

    private void updateFxButtonState(Button b, boolean active, String label) {
        if (b == null) return;
        b.setText(label);
        if (active) {
            b.setTextColor(0xFF00E676);
            b.setBackground(stage1KRoundRect(0xFF082215, 0xFF00E676, 8.0f, 1));
        } else {
            b.setTextColor(0xFFE8F7FF);
            b.setBackground(stage1KRoundRect(0xFF101622, 0xFF384E6C, 8.0f, 1));
        }
    }

    private void cycleSweepCadence() {
        if (userSweepCadenceMs == 100) {
            userSweepCadenceMs = 250;
        } else if (userSweepCadenceMs == 250) {
            userSweepCadenceMs = 500;
        } else if (userSweepCadenceMs == 500) {
            userSweepCadenceMs = 1000;
        } else {
            userSweepCadenceMs = 100;
        }
        if (sweepCadenceButton != null) {
            sweepCadenceButton.setText(userSweepCadenceMs >= 1000 ? "1.0s" : userSweepCadenceMs + "ms");
        }
    }

    private void cycleSweepDirection() {
        if (userSweepDirection == 1) {
            userSweepDirection = -1;
            if (sweepDirectionButton != null) sweepDirectionButton.setText("◀ REV");
        } else if (userSweepDirection == -1) {
            userSweepDirection = 0;
            if (sweepDirectionButton != null) sweepDirectionButton.setText("🔀 RND");
        } else {
            userSweepDirection = 1;
            if (sweepDirectionButton != null) sweepDirectionButton.setText("▶ FWD");
        }
    }

    private void toggleAudioFxEcho() {
        userEchoFxEnabled = !userEchoFxEnabled;
        updateFxButtonState(audioFxEchoButton, userEchoFxEnabled, "ECHO");
    }

    private void toggleAudioNoise() {
        userWhiteNoiseEnabled = !userWhiteNoiseEnabled;
        updateFxButtonState(audioNoiseButton, userWhiteNoiseEnabled, "NOISE");
    }

    private void toggleAudioRev() {
        userReverseAudioEnabled = !userReverseAudioEnabled;
        updateFxButtonState(audioRevButton, userReverseAudioEnabled, "REV");
    }

    private void autoDiscoverSourcesIfNeeded() {
        File slotsFile = liveSourceSlotsFile();
        boolean needsRefresh = true;
        if (slotsFile.isFile()) {
            try {
                JSONObject obj = new JSONObject(readSmallTextFile(slotsFile, 512 * 1024));
                int slotCount = obj.optInt("slot_count", 0);
                if (slotCount >= 32) {
                    needsRefresh = false;
                }
            } catch (Throwable ignored) {
            }
        }
        if (needsRefresh) {
            final Context appContext = getApplicationContext();
            new Thread(() -> {
                try {
                    JSONObject result = RadioBrowserDiscoveryCache.refresh(appContext, 1000);
                    runOnUiThread(() -> {
                        refreshLiveSourcesStatus();
                        stage1KShowChannelNumberOnly(stage1ULFastSweepCursor);
                    });
                } catch (Throwable t) {
                    Log.w("FieldVox", "Auto-discovery initial fetch deferred: " + t);
                }
            }, "fieldvox-auto-discover").start();
        }
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
        thresholdLabel.setText(String.format(Locale.getDefault(), "Sensitivity: %.2f uT delta", threshold));
        cooldownLabel.setText(String.format(Locale.getDefault(), "Cooldown: %d ms", cooldownMs));
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

    private static boolean isLegacyPrivateHitExportName(String name) {
        if (name == null) {
            return false;
        }
        return name.equals("radio_browser_station_cache_private.jsonl")
                || name.equals("live_stream_events_stage1q_d.jsonl")
                || name.equals("radio_browser_catalog.json")
                || name.equals("radio_browser_discovery_manifest.json")
                || name.equals("source_slots.json")
                || name.equals("RADIO_BROWSER_CACHE_README.txt")
                || name.equals("fieldvox-source-audit.jsonl")
                || name.equals("SOURCE_AUDIT_README.txt")
                || name.equals("sweep_manifest.json")
                || name.equals("sweep_schedule.jsonl")
                || (name.startsWith("source_audit_") && name.endsWith(".json"));
    }

    private void deleteLegacyPrivateHitExports() {
        ContentResolver resolver = getContentResolver();
        String rel = Environment.DIRECTORY_DOWNLOADS + "/FieldVoxLab";
        String selection = MediaStore.MediaColumns.RELATIVE_PATH + " LIKE ?";
        String[] args = new String[] { rel + "%" };
        String[] projection = new String[] {
                MediaStore.MediaColumns._ID,
                MediaStore.MediaColumns.DISPLAY_NAME
        };

        try (Cursor rows = resolver.query(
                MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                projection,
                selection,
                args,
                null)) {
            if (rows == null) {
                return;
            }
            int idColumn = rows.getColumnIndexOrThrow(MediaStore.MediaColumns._ID);
            int nameColumn = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.DISPLAY_NAME);
            while (rows.moveToNext()) {
                String name = rows.getString(nameColumn);
                if (!isLegacyPrivateHitExportName(name)) {
                    continue;
                }
                Uri row = ContentUris.withAppendedId(
                        MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                        rows.getLong(idColumn));
                resolver.delete(row, null, null);
            }
        } catch (Exception e) {
            Log.w("FieldVox", "Could not remove legacy private hit exports", e);
        }
    }

    private ExportResult exportOneToDownloads(File src, String mimeType) throws IOException {
        ContentResolver resolver = getContentResolver();
        long expectedBytes = src.length();

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

        if (copied != expectedBytes) {
            resolver.delete(uri, null, null);
            throw new IOException("Source changed during export for " + src.getName()
                    + " (expected " + expectedBytes + " bytes, copied " + copied + ").");
        }

        ContentValues done = new ContentValues();
        done.put(MediaStore.MediaColumns.IS_PENDING, 0);
        if (resolver.update(uri, done, null, null) != 1) {
            resolver.delete(uri, null, null);
            throw new IOException("Could not finalize MediaStore export for " + src.getName());
        }

        return new ExportResult(uri, copied);
    }

    private String exportLatestPair() {
        if (running || isLiveCaptureActive()) {
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

    static int countTriggerEvents(File jsonl) {
        int count = 0;
        try (java.io.BufferedReader br = new java.io.BufferedReader(
                new java.io.InputStreamReader(
                        new java.io.FileInputStream(jsonl),
                        java.nio.charset.StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.contains("\"event\":\"trigger\"") || line.contains("\"type\":\"trigger\"")) {
                    count += 1;
                }
            }
        } catch (IOException ignored) {
            return -1;
        }
        return count;
    }

    private String exportSessionManifest() {
        if (running || isLiveCaptureActive()) {
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
        if (running || isLiveCaptureActive()) {
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
        if (running || isLiveCaptureActive()) {
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
        if (running || isLiveCaptureActive()) {
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

    private static String sha256Text(String text) {
        try {
            java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest((text == null ? "" : text)
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8));
            StringBuilder out = new StringBuilder(hash.length * 2);
            for (byte value : hash) {
                int v = value & 0xff;
                out.append(Character.forDigit((v >>> 4) & 0x0f, 16));
                out.append(Character.forDigit(v & 0x0f, 16));
            }
            return out.toString();
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new IllegalStateException("SHA-256 unavailable", impossible);
        }
    }

    private String currentCarrierUrlHashSafe() {
        return stage1ULastStreamUrlSha256 == null ? "" : stage1ULastStreamUrlSha256;
    }

    private String buildHitJsonLine(String hitId, long now, String label, String sessionStem, long offsetMs) {
        int nextIndex = normalizeOnlineCarrierIndex(onlineCarrierIndex + 1);
        String nextLabel = getOnlineStationLabel(nextIndex);

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
                + "\"source_url_sha256\":\"" + jsonEscape(currentCarrierUrlHashSafe()) + "\","
                + "\"raw_stream_url_exported\":false,"
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
        synchronized (hitMarkerLock) {
            Iterator<String> pending = pendingHitJsonLines.iterator();
            while (pending.hasNext()) {
                writeJsonLine(jsonOut, pending.next());
                pending.remove();
            }
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


    private void recordNativeTimelineEvent(String eventKind, String detail) {
        try {
            String timelineResult = nativeTimelineRecord(
                    eventKind == null ? "unknown" : eventKind,
                    detail == null ? "" : detail);
            Log.i("FieldVox", timelineResult);
        } catch (Throwable nativeTimelineThrowable) {
            Log.w("FieldVox", "Native timeline record failed", nativeTimelineThrowable);
        }
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
        if (cleanedLabel.length() > MAX_HIT_LABEL_LENGTH) {
            cleanedLabel = cleanedLabel.substring(0, MAX_HIT_LABEL_LENGTH);
        }
        String jsonLine = buildHitJsonLine(hitId, now, cleanedLabel, sessionStem, offsetMs);

            try {
                recordNativeTimelineEvent("mark_hit", jsonLine);
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

            if (liveMonitorRunning && sessionStem.equals(currentLiveSessionStem)) {
                synchronized (hitMarkerLock) {
                    pendingHitJsonLines.add(jsonLine);
                }
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
            exportStatus.setText(getString(R.string.mark_hit_failed, e.getMessage()));
        }
    }

    private String readWholeText(File file) throws IOException {
        StringBuilder sb = new StringBuilder();
        try (java.io.BufferedReader br = new java.io.BufferedReader(
                new java.io.InputStreamReader(
                        new java.io.FileInputStream(file),
                        java.nio.charset.StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append('\n');
            }
        }
        return sb.toString();
    }

    private String extractJsonString(String json, String key) {
        try {
            return new JSONObject(json).optString(key, "");
        } catch (Exception ignored) {
            return "";
        }
    }

    private long extractJsonLong(String json, String key, long fallback) {
        try {
            return new JSONObject(json).optLong(key, fallback);
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
                    throw new IOException("unexpected end of WAV while seeking clip start");
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
        long copied = 0L;
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
                copied += n;
            }
        }
        if (copied != wanted) {
            if (!dst.delete()) {
                Log.w("FieldVox", "Could not remove incomplete WAV clip " + dst.getName());
            }
            throw new IOException("source WAV ended before the requested clip was complete");
        }
        return copied;
    }

    private static boolean isSafeArtifactComponent(String value, int maxLength) {
        if (value == null || value.isEmpty() || value.length() > maxLength || value.contains("..")) {
            return false;
        }
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (!((c >= 'a' && c <= 'z')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')
                    || c == '-' || c == '_' || c == '.')) {
                return false;
            }
        }
        return true;
    }

    static boolean isSafeSessionStem(String value) {
        return isSafeArtifactComponent(value, 160)
                && value.startsWith("session-");
    }

    private static String tsvCell(String value) {
        if (value == null) {
            return "";
        }
        StringBuilder clean = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            clean.append(c == '\t' || c == '\r' || c == '\n' || Character.isISOControl(c) ? ' ' : c);
        }
        return clean.toString();
    }

    private static JSONObject sanitizedHitObject(String json) throws Exception {
        JSONObject hit = new JSONObject(json);
        String rawUrl = hit.optString("source_url", "");
        String[] rawUrlKeys = new String[] {
                "source_url", "stream_url", "stream_url_resolved",
                "private_url", "private_url_resolved", "url", "url_resolved"
        };
        for (String key : rawUrlKeys) {
            hit.remove(key);
        }
        if (hit.optString("source_url_sha256", "").isEmpty() && !rawUrl.isEmpty()) {
            hit.put("source_url_sha256", sha256Text(rawUrl));
        }
        hit.put("raw_stream_url_exported", false);
        return hit;
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
        int rejectedHits = 0;
        int exported = 0;
        int clipFailures = 0;
        long bytes = 0L;
        java.util.LinkedHashSet<File> approvedFiles = new java.util.LinkedHashSet<>();
        StringBuilder sanitizedEvents = new StringBuilder();

        try {
            writeTextFile(instructionsFile, instructions);
            approvedFiles.add(instructionsFile);
        } catch (IOException e) {
            return "Hit instructions write failed: " + e.getMessage();
        }

        for (File hitJson : files) {
            String name = hitJson.getName();
            if (!hitJson.isFile() || !name.startsWith("hit_") || !name.endsWith(".json")) {
                continue;
            }

            try {
                String json = readWholeText(hitJson);
                JSONObject hit = sanitizedHitObject(json);
                String fileHitId = name.substring(0, name.length() - ".json".length());
                String hitId = hit.optString("hit_id", "");
                String sessionStem = hit.optString("session_stem", "");
                if (!hitId.equals(fileHitId)
                        || !isSafeArtifactComponent(hitId, 220)
                        || !isSafeSessionStem(sessionStem)) {
                    rejectedHits += 1;
                    continue;
                }

                String label = hit.optString("user_label", "");
                String sourceLabel = hit.optString("source_label", "");
                String reason = hit.optString("gate_reason", "");
                long offsetMs = Math.max(0L, hit.optLong("hit_offset_ms", 0L));
                long unixMs = hit.optLong("unix_ms", System.currentTimeMillis());
                if (unixMs < 0L) {
                    unixMs = 0L;
                }

                String sanitizedJson = hit.toString();
                writeTextFile(hitJson, sanitizedJson + "\n");
                sanitizedEvents.append(sanitizedJson).append('\n');
                approvedFiles.add(hitJson);
                hits += 1;

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
                    approvedFiles.add(contextWav);
                    approvedFiles.add(blindWav);
                } else {
                    clipFailures += 1;
                }

                truth.append(tsvCell(hitId)).append('\t')
                        .append(tsvCell(sessionStem)).append('\t')
                        .append(offsetMs).append('\t')
                        .append(tsvCell(label)).append('\t')
                        .append(tsvCell(sourceLabel)).append('\t')
                        .append(tsvCell(reason)).append('\t')
                        .append(tsvCell(hitJson.getName())).append('\t')
                        .append(tsvCell(contextName)).append('\t')
                        .append(tsvCell(blindName)).append('\n');
            } catch (Exception e) {
                rejectedHits += 1;
                clipFailures += 1;
                Log.w("FieldVox", "Rejected hit metadata " + name, e);
            }
        }

        if (hits == 0) {
            return "No marked hits yet. Tap MARK HIT during or right after a candidate phrase.";
        }

        try {
            writeTextFile(truthFile, truth.toString());
            approvedFiles.add(truthFile);

            File eventsFile = new File(dir, "fieldvox-hit-events.jsonl");
            writeTextFile(eventsFile, sanitizedEvents.toString());
            approvedFiles.add(eventsFile);
        } catch (IOException e) {
            return "Hit bundle manifest write failed: " + e.getMessage();
        }

        deleteLegacyPrivateHitExports();

        for (File f : approvedFiles) {
            String n = f.getName();
            if (!f.isFile()) {
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
                + "\nRejected metadata: " + rejectedHits
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
        handleRadioBrowserDiscoveryIntent(intent);
    }

    private void handleAdbHitMarkerIntent(Intent intent) {
        if (intent == null) {
            return;
        }

        if (!isDebuggableBuild()) {
            return;
        }

        String action = intent.getAction();
        Log.i("FieldVox", "ADB intent observed action=" + action
                + " mark_extra=" + intent.getBooleanExtra("fieldvox_mark_hit", false));
        boolean exportRequested = "org.fieldvox.lab.EXPORT_HITS".equals(action)
                || intent.getBooleanExtra("fieldvox_export_hits", false);

        if (exportRequested) {
            consumeDebugIntent(intent, "org.fieldvox.lab.EXPORT_HITS", "fieldvox_export_hits");
            Log.i("FieldVox", "ADB EXPORT HITS requested");
            recordNativeTimelineEvent("adb_export_hits_requested", "EXPORT_HITS");
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

        consumeDebugIntent(intent, "org.fieldvox.lab.MARK_HIT", "fieldvox_mark_hit");

        String label = intent.getStringExtra("fieldvox_label");
        if (label == null) {
            label = "";
        }

        final String finalLabel = label;

        Log.i("FieldVox", "ADB MARK HIT requested label_len=" + finalLabel.length());
        recordNativeTimelineEvent("adb_mark_hit_requested", "label_len=" + finalLabel.length());

        handler.postDelayed(() -> {
            markCandidateHit(finalLabel);
        }, 1200L);
    }


    private void handleRadioBrowserDiscoveryIntent(Intent intent) {
        if (intent == null) {
            return;
        }
        if (!isDebuggableBuild()) {
            return;
        }
        String action = intent.getAction();
        boolean requested = "org.fieldvox.lab.RADIO_BROWSER_DISCOVER".equals(action)
                || intent.getBooleanExtra("fieldvox_radio_browser_discover", false);
        if (!requested) {
            return;
        }

        consumeDebugIntent(intent, "org.fieldvox.lab.RADIO_BROWSER_DISCOVER",
                "fieldvox_radio_browser_discover");

        Log.i("FieldVox", "RADIO_BROWSER_DISCOVER requested");

        final android.content.Context appContext = getApplicationContext();
        new Thread(() -> {
            try {
                org.json.JSONObject result = RadioBrowserDiscoveryCache.refresh(appContext, 1000);
                int slotCount = result.optInt("slot_count", 0);
                String server = result.optString("server_selected", "");
                int catalogCount = result.optInt("catalog_count", 0);
                Log.i("FieldVox", "RADIO_BROWSER_CACHE_OK catalog=" + catalogCount + " slots=" + slotCount + " server=" + server);
                runOnUiThread(() -> android.widget.Toast.makeText(
                        this,
                        "Radio Browser sources cached: " + slotCount,
                        android.widget.Toast.LENGTH_LONG
                ).show());
            } catch (Throwable t) {
                Log.w("FieldVox", "RADIO_BROWSER_CACHE_FAILED", t);
                runOnUiThread(() -> android.widget.Toast.makeText(
                        this,
                        "Radio Browser source cache failed",
                        android.widget.Toast.LENGTH_LONG
                ).show());
            }
        }, "fieldvox-radio-browser-discovery").start();
    }

    private boolean isDebuggableBuild() {
        return (getApplicationInfo().flags
                & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    private static void consumeDebugIntent(Intent intent, String action, String booleanExtra) {
        intent.removeExtra(booleanExtra);
        if (action.equals(intent.getAction())) {
            intent.setAction(null);
        }
    }


    // FIELDVOX_STAGE1Q_C_VISIBLE_SOURCE_CATALOG
    private java.io.File liveSourceSlotsFile() {
        return new java.io.File(new java.io.File(getFilesDir(), "fieldvox-hits"), "source_slots.json");
    }

    private java.io.File liveSourceCatalogFile() {
        return new java.io.File(new java.io.File(getFilesDir(), "fieldvox-hits"), "radio_browser_catalog.json");
    }

    private String readSmallTextFile(java.io.File file, int maxBytes) throws java.io.IOException {
        if (file == null || !file.isFile()) {
            return "";
        }
        java.io.FileInputStream in = new java.io.FileInputStream(file);
        try {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int total = 0;
            int n;
            while ((n = in.read(buf)) >= 0) {
                total += n;
                if (total > maxBytes) {
                    throw new java.io.IOException("file too large for UI preview: " + file.getName());
                }
                out.write(buf, 0, n);
            }
            return out.toString("UTF-8");
        } finally {
            in.close();
        }
    }

    private String buildLiveSourcesStatusText() {
        java.io.File sourceSlots = liveSourceSlotsFile();
        java.io.File catalogFile = liveSourceCatalogFile();

        if (!sourceSlots.isFile()) {
            return "Live Source Catalog\n"
                    + "No cached live sources yet.\n"
                    + "Playback: unchanged\n"
                    + "Raw URLs: private only";
        }

        try {
            org.json.JSONObject root = new org.json.JSONObject(readSmallTextFile(sourceSlots, 1024 * 1024));
            String stage = root.optString("stage", "unknown");
            int catalogCount = root.optInt("catalog_count", 0);
            int slotCount = root.optInt("slot_count", 0);
            String server = root.optString("server_selected", "unknown");
            boolean playbackChanged = root.optBoolean("playback_engine_changed", false);
            boolean rawExported = root.optBoolean("raw_stream_urls_exported", true);
            String policy = root.optString("source_pool_policy", "unknown");

            return "Live Source Catalog\n"
                    + "Stage: " + stage + "\n"
                    + "Catalog cached: " + catalogCount + " stations\n"
                    + "Session pool: " + slotCount + " stations\n"
                    + "Server: " + server + "\n"
                    + "Policy: " + policy + "\n"
                    + "Playback: " + (playbackChanged ? "changed" : "unchanged") + "\n"
                    + "Raw URLs exported: " + rawExported + "\n"
                    + "Catalog file: " + (catalogFile.isFile() ? "present" : "missing") + "\n"
                    + "Raw URLs: private only";
        } catch (Throwable t) {
            return "Live Source Catalog\n"
                    + "Could not read source cache.\n"
                    + "Error: " + t.getClass().getSimpleName() + "\n"
                    + "Playback: unchanged\n"
                    + "Raw URLs: private only";
        }
    }

    private String buildLiveSourcesDialogText() {
        java.lang.StringBuilder b = new java.lang.StringBuilder();
        b.append(buildLiveSourcesStatusText()).append("\n\n");

        try {
            org.json.JSONObject root = new org.json.JSONObject(readSmallTextFile(liveSourceSlotsFile(), 1024 * 1024));
            org.json.JSONArray slots = root.optJSONArray("slots");
            if (slots == null || slots.length() == 0) {
                b.append("No session pool entries found.");
                return b.toString();
            }

            b.append("Sample session pool labels:\n");
            int max = Math.min(16, slots.length());
            for (int i = 0; i < max; i++) {
                org.json.JSONObject slot = slots.optJSONObject(i);
                if (slot == null) {
                    continue;
                }
                String label = slot.optString("source_label", "unknown");
                String country = slot.optString("countrycode", "--");
                String codec = slot.optString("codec", "--");
                int bitrate = slot.optInt("bitrate", 0);
                b.append(i + 1)
                        .append(". ")
                        .append(label)
                        .append(" [")
                        .append(country)
                        .append(" / ")
                        .append(codec)
                        .append(" / ")
                        .append(bitrate)
                        .append("kbps]\n");
            }

            b.append("\nRaw stream URLs are not shown here. They remain app-private.");
        } catch (Throwable t) {
            b.append("Could not preview session pool: ").append(t.getClass().getSimpleName());
        }

        return b.toString();
    }

    private void refreshLiveSourcesStatus() {
        if (liveSourcesStatusView != null) {
            liveSourcesStatusView.setText(buildLiveSourcesStatusText());
        }
    }

    private void showLiveSourcesDialog() {
        refreshLiveSourcesStatus();
        new android.app.AlertDialog.Builder(this)
                .setTitle("Live Source Catalog")
                .setMessage(buildLiveSourcesDialogText())
                .setPositiveButton("OK", null)
                .setNeutralButton("Refresh Online Stations", (dialog, which) -> {
                    Toast.makeText(this, "Discovering online radio streams...", Toast.LENGTH_SHORT).show();
                    final Context appContext = getApplicationContext();
                    new Thread(() -> {
                        try {
                            JSONObject result = RadioBrowserDiscoveryCache.refresh(appContext, 1000);
                            int count = result.optInt("slot_count", 0);
                            runOnUiThread(() -> {
                                refreshLiveSourcesStatus();
                                stage1KShowChannelNumberOnly(stage1ULFastSweepCursor);
                                Toast.makeText(this, "Cached " + count + " active station slots", Toast.LENGTH_LONG).show();
                            });
                        } catch (Throwable t) {
                            runOnUiThread(() -> Toast.makeText(this, "Discovery failed: " + t.getMessage(), Toast.LENGTH_LONG).show());
                        }
                    }, "user-refresh-stations").start();
                })
                .show();
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
        if (stage1ULastNativeStationLabel != null
                && !stage1ULastNativeStationLabel.isEmpty()) {
            return stage1ULastNativeStationLabel;
        }
        return getOnlineStationLabel(onlineCarrierIndex);
    }

    private synchronized void openOnlineCarrierGate(String reason, float delta) {
        if (!onlineCarrierActive) {
            android.util.Log.i("FieldVox", "PHONETIC VOX gate skipped: inactive reason=" + reason);
            return;
        }

        long now = System.currentTimeMillis();
        if (!onlineCarrierPrepared || onlineCarrierPlayer == null) {
            onlineCarrierNextAllowedGateMs = Math.max(onlineCarrierNextAllowedGateMs, now + 500L);
            if (now - onlineCarrierLastWaitingLogMs >= 1500L) {
                onlineCarrierLastWaitingLogMs = now;
                android.util.Log.i("FieldVox", "BOUNDED FIELD WAITING FOR CARRIER reason=" + reason
                        + " prepared=" + onlineCarrierPrepared);
            }
            return;
        }

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

        if (stage1SCBlockLegacyCarrier("phoneticPacketOpen")) {
            return;
        }

        String openMsg = "BOUNDED FIELD PACKET OPEN: " + onlineCarrierLabel()
                + " | Reason: " + reason
                + " delta=" + String.format(java.util.Locale.US, "%.2f", delta)
                + " grains=" + grains
                + " packet_ms=" + packetMs
                + " silence_after_ms=" + forcedSilenceMs
                + " | bounded field metrics. No word bank. No TTS.";
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

                android.util.Log.i("FieldVox", "BOUNDED FIELD GRAIN OPEN: "
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

            String closeMsg = "BOUNDED FIELD PACKET CLOSED. Forced silence gap active. Sweeping to next stream.";
            android.util.Log.i("FieldVox", closeMsg + " token=" + token + " packet_ms=" + totalPacketMs);
            exportStatus.setText(closeMsg);

            handler.postDelayed(() -> advanceOnlineCarrierSweep("phonetic-packet-closed"), 350L);
        }, totalPacketMs);
    }


    private void startOnlineCarrierAutoGate() {
        if (stage1SCBlockLegacyCarrier("startOnlineCarrierAutoGate")) {
            return;
        }
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
            exportStatus.setText(R.string.sensor_manager_unavailable);
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
                long now = System.currentTimeMillis();
                int type = event.sensor.getType();

                if (type == Sensor.TYPE_MAGNETIC_FIELD && event.values.length >= 3) {
                    float[] telemetry = new float[6];
                    boolean isSpike = nativeUpdateSensorTelemetry(
                            type,
                            event.values[0],
                            event.values[1],
                            event.values[2],
                            threshold,
                            telemetry
                    );
                    float mag = telemetry[0];
                    float delta = telemetry[1];
                    onlineCarrierMagBaseline = telemetry[2];

                    if (emfMeterView != null) {
                        emfMeterView.updateEmf(mag, delta, isSpike);
                    }

                    if (!onlineCarrierActive) {
                        return;
                    }

                    if (isSpike
                            && now >= onlineCarrierNextAllowedGateMs
                            && now - onlineCarrierLastGateMs >= Math.max(1800, cooldownMs + 1200)) {
                        openOnlineCarrierGate("magnetometer", delta);
                    }
                    return;
                }

                if (!onlineCarrierActive) {
                    return;
                }

                if (type == Sensor.TYPE_GYROSCOPE && event.values.length >= 3) {
                    float[] telemetry = new float[6];
                    boolean isSpike = nativeUpdateSensorTelemetry(
                            type,
                            event.values[0],
                            event.values[1],
                            event.values[2],
                            threshold,
                            telemetry
                    );
                    float delta = telemetry[1];
                    onlineCarrierGyroBaseline = telemetry[2];

                    // Gyro is movement-sensitive; use it as a secondary trigger, not a spam source.
                    if (isSpike
                            && now >= onlineCarrierNextAllowedGateMs
                            && now - onlineCarrierLastGateMs >= Math.max(1900, cooldownMs + 1300)) {
                        openOnlineCarrierGate("gyroscope", delta);
                    }
                    return;
                }

                if (type == Sensor.TYPE_PROXIMITY && event.values.length >= 1) {
                    float v = event.values[0];
                    float maxRange = event.sensor.getMaximumRange();
                    if (!Float.isFinite(v) || !Float.isFinite(maxRange) || maxRange <= 0.0f) {
                        return;
                    }
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

        boolean magneticRegistered = magnetic != null &&
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    magnetic,
                    SensorManager.SENSOR_DELAY_UI);
        if (magneticRegistered) {
            onlineCarrierRegisteredSensorCount++;
        }

        boolean gyroRegistered = gyro != null &&
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    gyro,
                    SensorManager.SENSOR_DELAY_UI);
        if (gyroRegistered) {
            onlineCarrierRegisteredSensorCount++;
        }

        boolean proximityRegistered = proximity != null &&
            onlineCarrierSensorManager.registerListener(
                    onlineCarrierSensorListener,
                    proximity,
                    SensorManager.SENSOR_DELAY_UI);
        if (proximityRegistered) {
            onlineCarrierRegisteredSensorCount++;
        }

        String msg = "Multi-sensor VOX gates armed: "
                + "magnetometer=" + magneticRegistered
                + " gyroscope=" + gyroRegistered
                + " proximity=" + proximityRegistered
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

        return "Bounded station sweep is active. Microphone and field metrics are logged for review.";
    }

    private int normalizeOnlineCarrierIndex(int index) {
        int count = getOnlineStationCount();
        if (count <= 0) {
            return 0;
        }
        int n = index % count;
        if (n < 0) {
            n += count;
        }
        return n;
    }



    private String stage1KThreeDigitChannelNumber(int logicalIndex) {
        int safe = logicalIndex;
        if (safe < 0) safe = 0;
        int ring = stage1ULastRingCount > 0 ? stage1ULastRingCount : 1000;
        if (ring <= 0) ring = 1000;
        safe = safe % ring;
        return String.format(java.util.Locale.US, "%03d", safe);
    }

    private android.graphics.drawable.GradientDrawable stage1KRoundRect(int color, int strokeColor, float radiusDp, int strokeDp) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setShape(android.graphics.drawable.GradientDrawable.RECTANGLE);
        g.setColor(color);
        g.setCornerRadius(radiusDp * getResources().getDisplayMetrics().density);
        if (strokeDp > 0) {
            g.setStroke((int) (strokeDp * getResources().getDisplayMetrics().density), strokeColor);
        }
        return g;
    }

    private boolean stage1KOldStatusText(CharSequence rawText) {
        if (rawText == null) return false;
        String t = rawText.toString();
        if (t.length() == 0) return false;
        return t.contains("BOUNDED FIELD")
                || t.contains("Bounded field")
                || t.contains("bounded field")
                || t.contains("bounded station")
                || t.contains("Bounded station")
                || t.contains("Legacy carrier")
                || t.contains("legacy carrier")
                || t.contains("Realtime EVP monitor")
                || t.contains("Native C short audio window")
                || t.contains("Long-form station playback")
                || t.contains("This does not prove")
                || t.contains("No word bank")
                || t.contains("generated speech")
                || t.contains("native audio safety mode")
                || t.contains("NATIVE BLOB SWEEP")
                || t.contains("source: native C binary blob")
                || t.contains("Stage 1U-F native channel display");
    }

    private void stage1KHideOldStatusText(android.view.View root) {
        if (root == null) return;
        if (root instanceof android.widget.TextView) {
            android.widget.TextView tv = (android.widget.TextView) root;
            if (tv != exportStatus && stage1KOldStatusText(tv.getText())) {
                tv.setText("");
                tv.setVisibility(android.view.View.GONE);
                return;
            }
        }
        if (root instanceof android.view.ViewGroup) {
            android.view.ViewGroup group = (android.view.ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) {
                stage1KHideOldStatusText(group.getChildAt(i));
            }
        }
    }

    private void stage1KBeautifyTree(android.view.View root) {
        if (root == null) return;
        try {
            if (root == sweepCadenceButton || root == sweepDirectionButton
                    || root == audioFxEchoButton || root == audioNoiseButton || root == audioRevButton
                    || root == sessionsButton || root == exportButton || root == startButton
                    || root == shareButton || root == controlsButton || root == markHitButton) {
                return;
            }
            if (root instanceof android.widget.Button) {
                android.widget.Button b = (android.widget.Button) root;
                b.setTextColor(0xFFFFFFFF);
                b.setTextSize(15.0f);
                b.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
                b.setAllCaps(false);
                b.setBackground(stage1KRoundRect(0xFF151722, 0xFF6FE7FF, 8.0f, 1));
                b.setPadding(10, 8, 10, 8);
            } else if (root instanceof android.widget.TextView) {
                android.widget.TextView tv = (android.widget.TextView) root;
                if (tv != exportStatus && tv != channelBadgeView && tv != thresholdLabel && tv != cooldownLabel) {
                    tv.setTextColor(0xFFE8F7FF);
                    tv.setShadowLayer(6.0f, 0.0f, 0.0f, 0xAA6FE7FF);
                }
            }
            if (root instanceof android.view.ViewGroup) {
                android.view.ViewGroup group = (android.view.ViewGroup) root;
                for (int i = 0; i < group.getChildCount(); i++) {
                    stage1KBeautifyTree(group.getChildAt(i));
                }
            }
        } catch (Throwable ignored) {
        }
    }

    private void stage1KMakeExportStatusTinyNumberBox(String number) {
        if (exportStatus == null || number == null) return;
        exportStatus.setVisibility(android.view.View.VISIBLE);
        exportStatus.setText(number);
        exportStatus.setSingleLine(true);
        exportStatus.setIncludeFontPadding(false);
        exportStatus.setGravity(android.view.Gravity.CENTER);
        exportStatus.setTextSize(22.0f);
        exportStatus.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        exportStatus.setTextColor(0xFFFFFFFF);
        exportStatus.setShadowLayer(8.0f, 0.0f, 0.0f, 0xFF6FE7FF);
        exportStatus.setBackground(stage1KRoundRect(0xFF07121A, 0xFF6FE7FF, 8.0f, 1));
        int w = (int) (58.0f * getResources().getDisplayMetrics().density);
        int h = (int) (34.0f * getResources().getDisplayMetrics().density);
        android.view.ViewGroup.LayoutParams lp = exportStatus.getLayoutParams();
        if (lp != null) {
            lp.width = w;
            lp.height = h;
            exportStatus.setLayoutParams(lp);
        }
        exportStatus.setPadding(0, 0, 0, 0);
        exportStatus.bringToFront();
    }

    private void stage1KShowChannelNumberOnly(final int logicalIndex) {
        try {
            final String number = stage1KThreeDigitChannelNumber(logicalIndex);
            final String label = stage1UNativeStationLabel(logicalIndex);
            final int ring = stage1ULastRingCount > 0 ? stage1ULastRingCount : 1000;
            appendStage1QDLiveEvent("native_station_ring_gui_number_only", null, "ready", number);
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    try {
                        View root = getWindow() != null ? getWindow().getDecorView() : null;
                        stage1KBeautifyTree(root);
                        stage1KHideOldStatusText(root);
                        if (channelBadgeView != null) {
                            channelBadgeView.setText(String.format(Locale.US, "CH %s / %03d • %s", number, ring, label));
                        }
                    } catch (Throwable t) {
                        android.util.Log.w("FieldVox", "Stage 1U-K channel GUI failed: " + t);
                    }
                }
            });
        } catch (Throwable ignored) {
        }
    }

    private int stage1ULFastSweepPeriodMs() {
        return userSweepCadenceMs > 0 ? userSweepCadenceMs : 250;
    }

    private void stage1ULFastSweepTick() {
        try {
            int ring = nativeStage1UStationRingCount();
            if (ring <= 0) ring = 1000;
            int logical;
            if (userSweepDirection == 1) { // Forward
                logical = stage1ULFastSweepCursor % ring;
                stage1ULFastSweepCursor = (logical + 1) % ring;
            } else if (userSweepDirection == -1) { // Reverse
                logical = stage1ULFastSweepCursor % ring;
                stage1ULFastSweepCursor = (logical - 1 + ring) % ring;
            } else { // Random
                logical = (int)(Math.random() * ring);
                stage1ULFastSweepCursor = logical;
            }
            stage1KShowChannelNumberOnly(logical);
            appendStage1QDLiveEvent("native_station_ring_one_second_sweep", null, "ready",
                    String.format(java.util.Locale.US, "%03d period_ms=%d", logical, stage1ULFastSweepPeriodMs()));
            appendStage1QDLiveEvent("native_station_ring_one_second_gui_tick_only", null, "ready",
                    String.format(java.util.Locale.US, "%03d period_ms=%d", logical, stage1ULFastSweepPeriodMs()));
        } catch (Throwable t) {
            appendStage1QDLiveEvent("native_station_ring_one_second_sweep_error", null, "error", String.valueOf(t));
        }
    }

    private void stage1ULStartFastNativeSweep() {
        if (stage1ULFastSweepStarted) return;
        stage1ULFastSweepStarted = true;
        if (stage1ULFastSweepHandler == null) {
            stage1ULFastSweepHandler = new android.os.Handler(android.os.Looper.getMainLooper());
        }
        appendStage1QDLiveEvent("native_station_ring_one_second_sweep_started", null, "ready",
                "period_ms=" + stage1ULFastSweepPeriodMs());
        stage1ULFastSweepRunnable = new java.lang.Runnable() {
            @Override
            public void run() {
                try {
                    stage1ULFastSweepTick();
                    if (stage1ULFastSweepHandler != null) {
                        stage1ULFastSweepHandler.postDelayed(this, stage1ULFastSweepPeriodMs());
                    }
                } catch (Throwable ignored) {
                }
            }
        };
        stage1ULFastSweepHandler.postDelayed(stage1ULFastSweepRunnable, 80);
        stage1UNStartAudioDwellSweep();
    }

    private int stage1UNAudioDwellPeriodMs() {
        try {
            int ms = nativeStage1UNAudioDwellPeriodMs();
            if (ms < 3000) return 3000;
            if (ms > 10000) return 10000;
            return ms;
        } catch (Throwable ignored) {
            return 6000;
        }
    }

    private void stage1UNAudioDwellTick() {
        try {
            appendStage1QDLiveEvent("native_station_ring_audio_dwell_sweep", null, "ready",
                    "period_ms=" + stage1UNAudioDwellPeriodMs());
            advanceOnlineCarrierSweep("stage1u_n_audio_dwell_source_sweep");
        } catch (Throwable t) {
            appendStage1QDLiveEvent("native_station_ring_audio_dwell_sweep_skip", null, "skip", String.valueOf(t));
        }
    }

    private void stage1UNStartAudioDwellSweep() {
        if (stage1UNAudioDwellStarted) return;
        stage1UNAudioDwellStarted = true;
        if (stage1UNAudioDwellHandler == null) {
            stage1UNAudioDwellHandler = new android.os.Handler(android.os.Looper.getMainLooper());
        }
        appendStage1QDLiveEvent("native_station_ring_audio_dwell_started", null, "ready",
                "period_ms=" + stage1UNAudioDwellPeriodMs());
        stage1UNAudioDwellRunnable = new java.lang.Runnable() {
            @Override
            public void run() {
                try {
                    stage1UNAudioDwellTick();
                    if (stage1UNAudioDwellHandler != null) {
                        stage1UNAudioDwellHandler.postDelayed(this, stage1UNAudioDwellPeriodMs());
                    }
                } catch (Throwable ignored) {
                }
            }
        };
        stage1UNAudioDwellHandler.postDelayed(stage1UNAudioDwellRunnable, 650);
    }

    private void stopStage1USweeps() {
        if (stage1ULFastSweepHandler != null && stage1ULFastSweepRunnable != null) {
            stage1ULFastSweepHandler.removeCallbacks(stage1ULFastSweepRunnable);
        }
        if (stage1UNAudioDwellHandler != null && stage1UNAudioDwellRunnable != null) {
            stage1UNAudioDwellHandler.removeCallbacks(stage1UNAudioDwellRunnable);
        }
        stage1ULFastSweepRunnable = null;
        stage1UNAudioDwellRunnable = null;
        stage1ULFastSweepStarted = false;
        stage1UNAudioDwellStarted = false;
    }

    private String stage1UJsonValue(String json, String key) {
        if (json == null || key == null) {
            return "";
        }
        try {
            Object value = new JSONObject(json).opt(key);
            return value == null || value == JSONObject.NULL ? "" : String.valueOf(value);
        } catch (Exception ignored) {
            return "";
        }
    }

    private String stage1URedactedStationJson(String json) {
        try {
            JSONObject station = new JSONObject(json == null ? "{}" : json);
            station.remove("url");
            station.put("raw_stream_url_exported", false);
            return station.toString();
        } catch (Exception ignored) {
            return "{\"station_metadata\":\"redaction_failed\",\"raw_stream_url_exported\":false}";
        }
    }

    private String stage1UNativeStationUrl(int logicalIndex) {
        try {
            stage1ULastRingCount = nativeStage1UStationRingCount();
            String rawStationJson = nativeStage1UStationAt(logicalIndex);
            String url = stage1UJsonValue(rawStationJson, "url");
            stage1ULastNativeStationLabel = stage1UJsonValue(rawStationJson, "label");
            stage1ULastNativeStationJson = stage1URedactedStationJson(rawStationJson);
            if (url != null && (url.startsWith("https://") || url.startsWith("http://"))) {
                stage1ULastStreamUrlSha256 = sha256Text(url);
                return url;
            }
        } catch (Throwable t) {
            android.util.Log.w("FieldVox", "Stage 1U-E native binary blob ring lookup failed: " + t);
        }
        return null;
    }

    private String stage1UNativeStationLabel(int logicalIndex) {
        try {
            if (logicalIndex < 0) logicalIndex = 0;
            String json = nativeStage1UStationAt(logicalIndex);
            String label = stage1UJsonValue(json, "label");
            String logical = stage1UJsonValue(json, "logical");
            if (label != null && label.length() > 0) return label + " [native-slot " + logical + "]";
        } catch (Throwable ignored) {
        }
        return "native-slot " + logicalIndex;
    }

    private void stage1ULogNativeRingProbeOnce() {
        if (stage1UProbeLogged) return;
        try {
            String probe = nativeStage1UDecoderProbe();
            appendStage1QDLiveEvent("native_blob_decoder_probe", null, "complete", probe);
            int ringCount = nativeStage1UStationRingCount();
            if (ringCount <= 0) {
                throw new IllegalStateException("native station ring is empty");
            }
            stage1UProbeLogged = true;
            appendStage1QDLiveEvent("native_station_ring_ready", null, "ready", "ring_count=" + ringCount);
            stage1KShowChannelNumberOnly(0);
            stage1ULStartFastNativeSweep();
            android.util.Log.i("FieldVox", "Stage 1U-E native binary blob ring ready count=" + ringCount + " probe=" + probe);
            for (int i = 0; i < 24; i++) {
                appendStage1QDLiveEvent("native_station_ring_slot", null, "ready",
                        stage1URedactedStationJson(nativeStage1UStationAt(i)));
            }
        } catch (Throwable t) {
            appendStage1QDLiveEvent("native_station_ring_error", null, "error", String.valueOf(t));
            android.util.Log.w("FieldVox", "Stage 1U-E native binary blob ring probe failed: " + t);
        }
    }
    private boolean stage1SCBlockLegacyCarrier(String methodName) {
        boolean disabled = true;
        try {
            disabled = nativeStage1SCDisableLegacyCarrier();
        } catch (Throwable ignored) {
            disabled = true;
        }
        if (!disabled) {
            return false;
        }

        onlineCarrierActive = false;
        onlineCarrierPrepared = false;
        onlineCarrierGateOpen = false;
        onlineCarrierPrepareToken++;
        onlineCarrierGateToken++;

        try {
            if (onlineCarrierPlayer != null) {
                onlineCarrierPlayer.stop();
            }
        } catch (Throwable ignored) {
        }

        try {
            if (onlineCarrierPlayer != null) {
                onlineCarrierPlayer.release();
            }
        } catch (Throwable ignored) {
        }

        onlineCarrierPlayer = null;

        try {
            appendStage1QDLiveEvent("legacy_carrier_disabled", null, "disabled", "Stage 1S-C native C policy blocked " + methodName);
        } catch (Throwable ignored) {
        }

        try {
            nativeStage1SCNoteLegacyCarrierBlocked(methodName);
        } catch (Throwable ignored) {
        }

        android.util.Log.w("FieldVox", "Stage 1S-C native C policy blocked legacy carrier path: " + methodName);
        return true;
    }

    private void advanceOnlineCarrierSweep(String reason) {
        if (stage1SCBlockLegacyCarrier("advanceOnlineCarrierSweep")) {
            return;
        }
        if (!onlineCarrierActive || getOnlineStationCount() <= 0) {
            return;
        }

        int next = normalizeOnlineCarrierIndex(onlineCarrierIndex + 1);
        onlineCarrierSweepCount++;

        String msg = "BOUNDED FIELD SWEEP: "
                + onlineCarrierLabel()
                + " -> " + getOnlineStationLabel(next)
                + "\nReason: " + reason
                + " sweep_count=" + onlineCarrierSweepCount
                + "\nPreparing next online stream during silence.";
        Log.i("FieldVox", msg.replace("\n", " | "));
        exportStatus.setText(msg);

        prepareOnlineCarrierAtIndex(next);
    }

    private void prepareOnlineCarrierAtIndex(int index) {
        if (stage1SCBlockLegacyCarrier("prepareOnlineCarrierAtIndex")) {
            return;
        }
        if (!onlineCarrierActive) {
            return;
        }

        stopOnlineCarrierPlayerOnly();

        if (getOnlineStationCount() <= 0) {
            onlineCarrierActive = false;
            exportStatus.setText(R.string.no_stream_sources);
            return;
        }

        index = normalizeOnlineCarrierIndex(index);
        onlineCarrierIndex = index;
        onlineCarrierPrepared = false;
        stage1ULastNativeStationLabel = "";
        stage1ULastNativeStationJson = "";
        stage1ULastStreamUrlSha256 = "";

        final int prepareToken = ++onlineCarrierPrepareToken;
        final int prepareIndex = index;

        try {
            MediaPlayer mp = new MediaPlayer();
            onlineCarrierPlayer = mp;

            mp.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build());

            stage1ULogNativeRingProbeOnce();
            String stage1UNativeUrl = stage1UNativeStationUrl(stage1UNativeLogicalCursor++);
            if (stage1UNativeUrl != null) {
                appendStage1QDLiveEvent("native_station_ring_selected", null, "ready", stage1ULastNativeStationJson);
                appendStage1QDLiveEvent("native_station_ring_gui_selection_suppressed", null, "ready", stage1ULastNativeStationJson);
                android.util.Log.i("FieldVox", "Stage 1U-E native binary blob selected " + stage1UNativeStationLabel(stage1UNativeLogicalCursor - 1));
            }
            String selectedUrl = stage1UNativeUrl != null
                    ? stage1UNativeUrl
                    : getOnlineStationUrl(index);
            if (stage1ULastNativeStationLabel.isEmpty()) {
                stage1ULastNativeStationLabel = getOnlineStationLabel(index);
            }
            if (stage1ULastStreamUrlSha256.isEmpty()) {
                stage1ULastStreamUrlSha256 = sha256Text(selectedUrl);
            }
            mp.setDataSource(selectedUrl);
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

                String msg = "BOUNDED FIELD CARRIER: " + onlineCarrierLabel()
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
                        + " source=" + onlineCarrierLabel() + " trying_next=" + getOnlineStationLabel(next));

                handler.postDelayed(() -> prepareOnlineCarrierAtIndex(next), 450L);
                return true;
            });

            Log.i("FieldVox", "BOUNDED FIELD CARRIER: "
                    + getOnlineStationLabel(index)
                    + " url_sha256=" + stage1ULastStreamUrlSha256);
            exportStatus.setText(getString(R.string.bounded_field_carrier, getOnlineStationLabel(index)));

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

                String label = getOnlineStationLabel(prepareIndex);
                int next = normalizeOnlineCarrierIndex(prepareIndex + 1);

                Log.w("FieldVox", "Carrier prepare timeout: " + label
                        + " -> skipping to " + getOnlineStationLabel(next)
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
                    + getOnlineStationLabel(index)
                    + " next=" + getOnlineStationLabel(next)
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
        exportStatus.setText(getString(R.string.native_ring_active, result, carrier));
        status.setText(getString(R.string.two_line_status, result, carrier));
        updateLiveButtons();
        autoDiscoverSourcesIfNeeded();
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

    private boolean isLiveCaptureActive() {
        Thread thread = liveMonitorThread;
        return liveMonitorRunning || (thread != null && thread.isAlive());
    }

    private static int writeAllSamples(AudioTrack track, short[] samples, int count) {
        int offset = 0;
        while (offset < count) {
            int written = track.write(samples, offset, count - offset, AudioTrack.WRITE_BLOCKING);
            if (written <= 0) {
                return written < 0 ? written : AudioTrack.ERROR;
            }
            offset += written;
        }
        return offset;
    }


    private boolean hasMicPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestMicPermissionForLiveMonitor() {
        if (micPermissionRequestPending || hasMicPermission()) {
            return;
        }
        micPermissionRequestPending = true;
        try {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, LIVE_PERMISSION_REQUEST);
        } catch (RuntimeException e) {
            micPermissionRequestPending = false;
            throw e;
        }
    }

    private static byte[] makeWavHeader(long pcmBytes) {
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(44);
        b.order(java.nio.ByteOrder.LITTLE_ENDIAN);

        int byteRate = LIVE_SAMPLE_RATE * 2;
        int blockAlign = 2;
        long cappedPcmBytes = Math.max(0L, Math.min(pcmBytes, WAV_MAX_PCM_BYTES));
        int riffSize = (int)(36L + cappedPcmBytes);
        int dataSize = (int)cappedPcmBytes;

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

    private static boolean wavHeaderMatches(File wav, long pcmBytes) {
        if (wav.length() < 44L) {
            return false;
        }
        byte[] header = new byte[44];
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(wav, "r")) {
            raf.readFully(header);
        } catch (IOException e) {
            return false;
        }

        java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(header)
                .order(java.nio.ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(0) != 0x46464952
                || b.getInt(8) != 0x45564157
                || b.getInt(12) != 0x20746d66
                || b.getInt(36) != 0x61746164) {
            return false;
        }

        long expectedData = Math.min(pcmBytes, WAV_MAX_PCM_BYTES);
        long riffSize = Integer.toUnsignedLong(b.getInt(4));
        long dataSize = Integer.toUnsignedLong(b.getInt(40));
        return riffSize == 36L + expectedData && dataSize == expectedData;
    }

    private static void recoverInterruptedLiveSessions(File dir) {
        File[] files = dir.listFiles((ignored, name) ->
                name.startsWith("session-") && name.endsWith("-live.wav"));
        if (files == null) {
            return;
        }
        for (File wav : files) {
            long pcmBytes = Math.max(0L, wav.length() - 44L);
            pcmBytes -= pcmBytes % 2L;
            if (wavHeaderMatches(wav, pcmBytes)) {
                continue;
            }
            try {
                rewriteWavHeader(wav, pcmBytes);
            } catch (IOException e) {
                Log.w("FieldVox", "Could not recover WAV header for " + wav.getName(), e);
            }
        }
    }

    private String startLiveMonitor() {
        Thread existingThread = liveMonitorThread;
        if (liveMonitorRunning || (existingThread != null && existingThread.isAlive())) {
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
        recoverInterruptedLiveSessions(dir);

        final long startMs = System.currentTimeMillis();
        String stemCandidate = "session-" + startMs + "-live";
        for (int suffix = 1;
                new File(dir, stemCandidate + ".jsonl").exists()
                        || new File(dir, stemCandidate + ".wav").exists();
                suffix++) {
            stemCandidate = "session-" + startMs + "-" + suffix + "-live";
        }
        final String stem = stemCandidate;
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
            long lastHeaderCheckpointMs = 0L;
            String failureMessage = null;

            try (java.io.FileOutputStream jsonOut = new java.io.FileOutputStream(jsonl, false);
                 java.io.FileOutputStream wavOut = new java.io.FileOutputStream(wav, false)) {

                writeJsonLine(jsonOut,
                        "{\"event\":\"live_monitor_start\",\"mode\":\"realtime_evp_monitor\",\"sample_rate\":48000,"
                        + "\"source\":\"microphone\",\"online_carrier\":true,\"carrier\":\"native_station_ring_bounded_grains\",\"generated_words\":false,\"word_bank\":false,"
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

                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                        != PackageManager.PERMISSION_GRANTED) {
                    throw new SecurityException("Microphone permission was revoked before capture started.");
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
                liveAudioRecord = record;

                track = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                                .build())
                        .setAudioFormat(new AudioFormat.Builder()
                                .setSampleRate(LIVE_SAMPLE_RATE)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build())
                        .setBufferSizeInBytes(bufferBytes)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build();

                if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                    throw new IOException("AudioTrack failed to initialize.");
                }

                short[] samples = new short[bufferBytes / 2];
                short[] monitorSamples = new short[bufferBytes / 2];
                byte[] pcm = new byte[bufferBytes];

                record.startRecording();
                if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                    throw new IOException("AudioRecord did not enter recording state.");
                }
                track.setVolume(1.0f);
                track.play();

                handler.post(() -> {
                    status.setText(R.string.live_monitor_started);
                    refreshSessions();
                });

                while (liveMonitorRunning) {
                    int n = record.read(samples, 0, samples.length);
                    if (n < 0) {
                        if (!liveMonitorRunning) {
                            break;
                        }
                        throw new IOException("AudioRecord read failed with code " + n + ".");
                    }
                    if (n == 0) {
                        continue;
                    }
                    if (pcmBytes + (long)n * 2L > WAV_MAX_PCM_BYTES) {
                        throw new IOException("WAV reached the 4 GiB RIFF size limit.");
                    }

                    float[] dspMetrics = new float[2];
                    float boostFactor = liveOutputGain;
                    nativeProcessAudioDsp(
                            samples,
                            pcm,
                            speakerMonitorEnabled ? monitorSamples : null,
                            n,
                            userReverseAudioEnabled,
                            userEchoFxEnabled,
                            userWhiteNoiseEnabled,
                            boostFactor,
                            dspMetrics
                    );
                    int peak = (int)dspMetrics[0];
                    double rms = dspMetrics[1];

                    wavOut.write(pcm, 0, n * 2);
                    flushPendingHitMarkers(jsonOut);
                    pcmBytes += (long)n * 2L;

                    if (speakerMonitorEnabled) {
                        int written = writeAllSamples(track, monitorSamples, n);
                        if (written < 0) {
                            speakerMonitorEnabled = false;
                            final int audioTrackError = written;
                            handler.post(() -> {
                                updateSpeakerMonitorButton();
                                Log.w("FieldVox", "AudioTrack write failed with code " + audioTrackError);
                            });
                        }
                    }
                    if (rms > liveNoiseFloor) {
                        liveNoiseFloor = liveNoiseFloor * 0.95 + rms * 0.05;
                    } else {
                        liveNoiseFloor = liveNoiseFloor * 0.995 + rms * 0.005;
                    }

                    double gate = Math.max(900.0, liveNoiseFloor * 3.0);
                    long nowMs = System.currentTimeMillis();
                    if (nowMs - lastHeaderCheckpointMs >= 1000L) {
                        wavOut.flush();
                        rewriteWavHeader(wav, pcmBytes);
                        lastHeaderCheckpointMs = nowMs;
                    }

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
                        final String msg = "Realtime EVP monitor: " + seconds + "s\n"
                                + "Mic + bounded station grains.\n"
                                + "RMS " + String.format(java.util.Locale.US, "%.0f", rms)
                                + " / gate " + String.format(java.util.Locale.US, "%.0f", gate)
                                + " / peak " + peak + "\n"
                                + "No word bank. No generated speech.";
                        handler.post(() -> status.setText(msg));
                    }
                }

                flushPendingHitMarkers(jsonOut);
                long stopMs = System.currentTimeMillis();
                writeJsonLine(jsonOut,
                        "{\"event\":\"live_monitor_stop\",\"mode\":\"realtime_evp_monitor\",\"pcm_bytes\":"
                        + pcmBytes + ",\"duration_ms\":" + (stopMs - startMs)
                        + ",\"generated_words\":false,\"word_bank\":false,\"unix_ms\":" + stopMs + "}");

            } catch (Exception e) {
                final String err = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                failureMessage = err;
                handler.post(() -> status.setText(getString(R.string.live_monitor_error, err)));
            } finally {
                try {
                    if (record != null) {
                        try { record.stop(); } catch (Exception ignored) {}
                        record.release();
                    }
                } catch (Exception ignored) {}
                if (liveAudioRecord == record) {
                    liveAudioRecord = null;
                }

                try {
                    if (track != null) {
                        try { track.stop(); } catch (Exception ignored) {}
                        track.release();
                    }
                } catch (Exception ignored) {}

                try {
                    rewriteWavHeader(wav, pcmBytes);
                } catch (Exception e) {
                    if (failureMessage == null) {
                        failureMessage = "WAV finalization failed: " + e.getMessage();
                    }
                }

                if (liveMonitorThread == Thread.currentThread()) {
                    liveMonitorRunning = false;
                    liveMonitorThread = null;
                }
                final String completionError = failureMessage;
                handler.post(() -> {
                    if (liveButton != null) {
                        updateLiveButtons();
                    }
                    updateLiveButtons();
                    refreshSessions();
                    if (completionError == null) {
                        exportStatus.setText(getString(
                                R.string.live_session_saved, jsonl.getName(), wav.getName()));
                    } else {
                        exportStatus.setText(getString(R.string.live_session_failed, completionError));
                    }
                });
            }
        }, "FieldVox-Realtime-EVP-Monitor");

        liveMonitorThread.start();

        return "Bounded voice/radio grains enabled. No word bank; no TTS.";
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

    private void stopLiveMonitorAndWait() {
        liveMonitorRunning = false;
        AudioRecord record = liveAudioRecord;
        if (record != null) {
            try {
                record.stop();
            } catch (IllegalStateException ignored) {
                // The capture thread may already have stopped or be releasing it.
            }
        }
        Thread thread = liveMonitorThread;
        if (thread == null || thread == Thread.currentThread()) {
            return;
        }
        try {
            thread.join(1500L);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (thread.isAlive()) {
            Log.w("FieldVox", "Live monitor thread did not stop within 1500 ms.");
        }
    }


    private void toggleLiveMonitor() {
        String msg;
        if (liveMonitorRunning) {
            msg = "Auto Live is already running.\nNo Stop button needed. Leave or close the app to finalize the WAV.";
        } else {
            autoStartEverythingOnLaunch();
            handleAdbHitMarkerIntent(getIntent());
            handleRadioBrowserDiscoveryIntent(getIntent());
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

        boolean isLandscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;

        ScrollView rootScroll = new ScrollView(this);
        rootScroll.setFillViewport(true);
        rootScroll.setBackgroundColor(Color.BLACK);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(this, 12), dp(this, 8), dp(this, 12), dp(this, 12));
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.BLACK);

        rootScroll.setOnApplyWindowInsetsListener((v, insets) -> {
            int top = insets.getSystemWindowInsetTop();
            int bottom = insets.getSystemWindowInsetBottom();
            int left = insets.getSystemWindowInsetLeft();
            int right = insets.getSystemWindowInsetRight();
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                android.view.DisplayCutout cutout = insets.getDisplayCutout();
                if (cutout != null) {
                    top = Math.max(top, cutout.getSafeInsetTop());
                    bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                    left = Math.max(left, cutout.getSafeInsetLeft());
                    right = Math.max(right, cutout.getSafeInsetRight());
                }
            }
            v.setPadding(left + dp(this, 12), top + dp(this, 4), right + dp(this, 12), bottom + dp(this, 8));
            return insets;
        });

        TextView title = makeLabel("FIELDVOX LAB", Color.WHITE, isLandscape ? 22 : 28);
        title.setShadowLayer(10.0f, 0.0f, 0.0f, 0xFF6FE7FF);
        root.addView(title);

        TextView stage = makeLabel("GhostTube ITC Mode • 64–128 Channels • Live EMF", 0xFF6FE7FF, isLandscape ? 10 : 11);
        stage.setPadding(0, dp(this, 1), 0, dp(this, isLandscape ? 2 : 4));
        root.addView(stage);

        LogoGlyphView glyph = new LogoGlyphView(this);
        int glyphSize = isLandscape ? dp(this, 44) : dp(this, 84);
        root.addView(glyph, new LinearLayout.LayoutParams(glyphSize, glyphSize));

        wavePanel = new WavePanelView(this);
        int waveHeight = isLandscape ? dp(this, 54) : dp(this, 88);
        LinearLayout.LayoutParams waveParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                waveHeight
        );
        waveParams.setMargins(0, dp(this, 2), 0, dp(this, isLandscape ? 3 : 5));
        root.addView(wavePanel, waveParams);

        emfMeterView = new EmfMeterView(this);
        int emfHeight = isLandscape ? dp(this, 42) : dp(this, 52);
        LinearLayout.LayoutParams emfParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                emfHeight
        );
        emfParams.setMargins(0, 0, 0, dp(this, isLandscape ? 3 : 5));
        root.addView(emfMeterView, emfParams);

        channelBadgeView = makeLabel("CH 000 / 064 • INITIALIZING SWEEP", 0xFFE8F7FF, 12);
        channelBadgeView.setBackground(stage1KRoundRect(0xFF07121A, 0xFF6FE7FF, 6.0f, 1));
        channelBadgeView.setPadding(dp(this, 8), dp(this, 6), dp(this, 8), dp(this, 6));
        channelBadgeView.setTypeface(Typeface.DEFAULT_BOLD);
        channelBadgeView.setShadowLayer(6.0f, 0.0f, 0.0f, 0xFF6FE7FF);
        LinearLayout.LayoutParams channelLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        channelLp.setMargins(0, 0, 0, dp(this, 6));
        root.addView(channelBadgeView, channelLp);

        LinearLayout sweepBar = new LinearLayout(this);
        sweepBar.setOrientation(LinearLayout.HORIZONTAL);
        sweepBar.setGravity(Gravity.CENTER);
        sweepBar.setPadding(0, 0, 0, dp(this, 6));

        sweepCadenceButton = makePillButton("250ms");
        sweepCadenceButton.setOnClickListener(v -> cycleSweepCadence());

        sweepDirectionButton = makePillButton("▶ FWD");
        sweepDirectionButton.setOnClickListener(v -> cycleSweepDirection());

        audioFxEchoButton = makePillButton("ECHO");
        audioFxEchoButton.setOnClickListener(v -> toggleAudioFxEcho());

        audioNoiseButton = makePillButton("NOISE");
        audioNoiseButton.setOnClickListener(v -> toggleAudioNoise());

        audioRevButton = makePillButton("REV");
        audioRevButton.setOnClickListener(v -> toggleAudioRev());

        LinearLayout.LayoutParams pillLp = new LinearLayout.LayoutParams(0, dp(this, 36), 1.0f);
        int pGap = dp(this, 2);
        pillLp.setMargins(pGap, 0, pGap, 0);

        sweepBar.addView(sweepCadenceButton, pillLp);
        sweepBar.addView(sweepDirectionButton, pillLp);
        sweepBar.addView(audioFxEchoButton, pillLp);
        sweepBar.addView(audioNoiseButton, pillLp);
        sweepBar.addView(audioRevButton, pillLp);

        root.addView(sweepBar);

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setGravity(Gravity.CENTER);
        bottom.setPadding(0, 0, 0, dp(this, 6));

        sessionsButton = makeCircleButton("Sess");
        exportButton = makeCircleButton("Export");
        startButton = makeCircleButton("Start");
        shareButton = makeCircleButton("Share");
        controlsButton = makeCircleButton("Ctrl");

        int gap = dp(this, 2);
        LinearLayout.LayoutParams smallLp = new LinearLayout.LayoutParams(0, dp(this, 44), 1.0f);
        smallLp.setMargins(gap, 0, gap, 0);

        LinearLayout.LayoutParams bigLp = new LinearLayout.LayoutParams(0, dp(this, 44), 1.15f);
        bigLp.setMargins(gap, 0, gap, 0);

        bottom.addView(sessionsButton, smallLp);
        bottom.addView(exportButton, smallLp);
        bottom.addView(startButton, bigLp);
        bottom.addView(shareButton, smallLp);
        bottom.addView(controlsButton, smallLp);

        root.addView(bottom);

        exportStatus = makeLabel("Auto Live active • 64 channels ready", Color.rgb(255, 230, 160), 12);
        exportStatus.setPadding(0, dp(this, 2), 0, dp(this, 6));
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
        status.setPadding(0, dp(this, 8), 0, dp(this, 4));
        status.setText(nativeStatus());
        root.addView(status);

        markHitButton = makeWideButton("MARK HIT");
        markHitButton.setTextSize(18);
        markHitButton.setContentDescription("MARK HIT");
        markHitButton.setBackgroundColor(Color.rgb(92, 30, 30));
        LinearLayout.LayoutParams markHitLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 54));
        markHitLp.setMargins(0, dp(this, 4), 0, dp(this, 4));
        root.addView(markHitButton, markHitLp);

        exportHitsButton = makeWideButton("Export Hits");
        exportHitsButton.setTextSize(16);
        exportHitsButton.setContentDescription("Export Hits");
        LinearLayout.LayoutParams expHitsLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 48));
        expHitsLp.setMargins(0, dp(this, 2), 0, dp(this, 2));
        root.addView(exportHitsButton, expHitsLp);

        liveSourcesButton = makeWideButton("Live Sources");
        liveSourcesButton.setTextSize(16.0f);
        liveSourcesButton.setContentDescription("Live Sources");
        liveSourcesButton.setMinHeight(dp(this, 48));
        LinearLayout.LayoutParams liveSrcLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(this, 48));
        liveSrcLp.setMargins(0, dp(this, 2), 0, dp(this, 2));
        liveSourcesButton.setOnClickListener(v -> showLiveSourcesDialog());
        refreshLiveSourcesStatus();
        root.addView(liveSourcesButton, liveSrcLp);

        liveStreamStatusText = new TextView(this);
        liveStreamStatusText.setText("Experimental Live Stream: stopped\nOne cached source only. Raw stream URLs stay app-private.\nThis does not prove paranormal communication.");
        liveStreamStatusText.setTextSize(12.0f);
        liveStreamStatusText.setPadding(0, dp(this, 4), 0, dp(this, 2));
        root.addView(liveStreamStatusText);

        liveStreamButton = new Button(this);
        liveStreamButton.setText(R.string.live_stream_disabled);
        liveStreamButton.setAllCaps(false);
        liveStreamButton.setOnClickListener(v -> startStage1QDLiveStream());
        root.addView(liveStreamButton);

        if (isDebuggableBuild() && getIntent() != null
                && getIntent().getBooleanExtra("fieldvox_stage1qd_autostart", false)) {
            getIntent().removeExtra("fieldvox_stage1qd_autostart");
            liveStreamButton.postDelayed(() -> {
                if (!liveStreamActive && liveStreamPlayer == null) {
                    startStage1QDLiveStream();
                }
            }, 900);
        }

        liveSourcesStatusView = new android.widget.TextView(this);
        liveSourcesStatusView.setTextSize(13.0f);
        liveSourcesStatusView.setTextColor(android.graphics.Color.WHITE);
        liveSourcesStatusView.setPadding(dp(this, 12), dp(this, 6), dp(this, 12), dp(this, 6));
        liveSourcesStatusView.setText("Live Source Catalog\nLoading cached source summary...");
        liveSourcesStatusView.setContentDescription("Live Source Catalog Status");
        root.addView(liveSourcesStatusView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        sessions = new TextView(this);
        sessions.setTextColor(Color.rgb(210, 240, 255));
        sessions.setTextSize(12);
        sessions.setPadding(0, dp(this, 8), 0, dp(this, 24));
        sessions.setText(nativeSessionList());
        root.addView(sessions);

        rootScroll.addView(root, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
        ));

        setContentView(rootScroll);

        stage1ULogNativeRingProbeOnce();

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
            exportStatus.setText(R.string.session_list_refreshed);
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
        startOnlineCarrierSensors();
        autoDiscoverSourcesIfNeeded();
        refreshLiveSourcesStatus();
        super.onResume();
        handler.removeCallbacks(tick);
        handler.post(tick);
        stage1ULogNativeRingProbeOnce();
        if (stage1UProbeLogged) {
            stage1ULStartFastNativeSweep();
        }
        // ADB_MARK_HIT_EXISTING_ONRESUME_POLL: deterministic test hook because UIAutomator cannot idle during live VOX.
        handler.postDelayed(() -> {
            Intent debugIntent = getIntent();
            handleAdbHitMarkerIntent(debugIntent);
            handleRadioBrowserDiscoveryIntent(debugIntent);
        }, 900L);
        handler.postDelayed(() -> autoStartEverythingOnLaunch(), 350);
    }


    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode == LIVE_PERMISSION_REQUEST) {
            micPermissionRequestPending = false;
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
        handler.removeCallbacks(tick);
        stopStage1USweeps();
        stopStage1QDLiveStreamIfActive("activity_stopped");
        stopOnlineVoxCarrier();
        stopLiveMonitorAndWait();
        updateLiveButtons();
        super.onStop();
    }

    protected void onDestroy() {
        stopStage1USweeps();
        stopStage1QDLiveStreamIfActive("activity_destroyed");
        stopOnlineVoxCarrier();
        stopLiveMonitorAndWait();
        nativeStop();
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    public static final class LogoGlyphView extends View {
        private static final float[] WAVE_AMPLITUDES = {
                0.10f, -0.34f, 0.22f, -0.52f, 0.50f, -0.20f, 0.35f, -0.12f, 0.08f
        };
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint soft = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);

        public LogoGlyphView(android.content.Context ctx) {
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
            float lastX = left;
            float lastY = baseY + WAVE_AMPLITUDES[0] * r;
            for (int i = 1; i < WAVE_AMPLITUDES.length; i++) {
                float x = left + step * i;
                float y = baseY + WAVE_AMPLITUDES[i] * r;
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
        private final RectF box = new RectF();
        private boolean running = false;
        private int phase = 0;

        public WavePanelView(android.content.Context ctx) {
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
            box.set(0, 0, w, h);
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

    public static final class EmfMeterView extends View {
        private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint barPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private final RectF seg = new RectF();

        private float currentMag = 0.0f;
        private float currentDelta = 0.0f;
        private boolean isSpike = false;
        private String statusText = "BASELINE";

        public EmfMeterView(android.content.Context ctx) {
            super(ctx);
            bgPaint.setColor(0xFF0C1017);
            bgPaint.setStyle(Paint.Style.FILL);

            textPaint.setColor(0xFFE8F7FF);
            textPaint.setTextSize(12.5f * getResources().getDisplayMetrics().density);
            textPaint.setTypeface(Typeface.DEFAULT_BOLD);
            textPaint.setAntiAlias(true);

            glowPaint.setStyle(Paint.Style.STROKE);
            glowPaint.setStrokeWidth(1.5f * getResources().getDisplayMetrics().density);
        }

        public void updateEmf(float mag, float delta, boolean spike) {
            this.currentMag = mag;
            this.currentDelta = delta;
            this.isSpike = spike;
            if (spike) {
                this.statusText = "SPIKE DETECTED";
            } else if (delta >= 1.5f) {
                this.statusText = "ELEVATED";
            } else if (delta >= 0.5f) {
                this.statusText = "FLUCTUATING";
            } else {
                this.statusText = "BASELINE";
            }
            postInvalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            super.onDraw(c);
            float w = getWidth();
            float h = getHeight();
            box.set(0, 0, w, h);

            int strokeColor = isSpike ? 0xFFFF1744 : (currentDelta >= 1.5f ? 0xFFFFD600 : 0xFF283548);
            glowPaint.setColor(strokeColor);
            c.drawRoundRect(box, 10.0f, 10.0f, bgPaint);
            c.drawRoundRect(box, 10.0f, 10.0f, glowPaint);

            String header = String.format(Locale.US, "EMF: %.1f µT   Δ %.1f µT  [%s]", currentMag, currentDelta, statusText);
            int textColor = isSpike ? 0xFFFF5252 : (currentDelta >= 1.5f ? 0xFFFFEE58 : 0xFF6FE7FF);
            textPaint.setColor(textColor);
            c.drawText(header, 14.0f, 18.0f * getResources().getDisplayMetrics().density, textPaint);

            int totalSegs = 10;
            float padX = 12.0f * getResources().getDisplayMetrics().density;
            float segY = 24.0f * getResources().getDisplayMetrics().density;
            float segH = h - segY - (8.0f * getResources().getDisplayMetrics().density);
            if (segH < 6.0f) segH = 6.0f;
            float segGap = 4.0f * getResources().getDisplayMetrics().density;
            float totalGap = segGap * (totalSegs - 1);
            float segW = (w - (padX * 2) - totalGap) / totalSegs;

            int activeSegs = (int)Math.min(totalSegs, Math.max(1, (currentDelta / 2.5f) * totalSegs));

            for (int i = 0; i < totalSegs; i++) {
                float sx = padX + i * (segW + segGap);
                seg.set(sx, segY, sx + segW, segY + segH);
                if (i < activeSegs) {
                    if (i < 4) {
                        barPaint.setColor(0xFF00E676); // Green
                    } else if (i < 7) {
                        barPaint.setColor(0xFFFFD600); // Yellow/Amber
                    } else if (i < 9) {
                        barPaint.setColor(0xFFFF9100); // Orange
                    } else {
                        barPaint.setColor(0xFFFF1744); // Red
                    }
                } else {
                    barPaint.setColor(0xFF161F2E);
                }
                c.drawRoundRect(seg, 3.0f, 3.0f, barPaint);
            }
        }
    }


    // STAGE_1Q_D_ONE_STREAM_LIVE_PLAYBACK
    private void toggleStage1QDLiveStream() {
        if (liveStreamActive || liveStreamPlayer != null) {
            stopStage1QDLiveStream("button");
            return;
        }
        startStage1QDLiveStream();
    }

    private void startStage1QDLiveStream() {
        // FIELDVOX_STAGE1QF_DISABLE_LONG_STREAM_PLAYBACK
        stopStage1QDLiveStreamIfActive("long_stream_disabled_before_start");
        appendStage1QDLiveEvent("live_stream_disabled", null, "disabled", "Long-form radio playback disabled after Stage 1Q-D/1Q-E test. Next implementation must use short buffered windows only.");
        appendStage1QDLiveEvent("native_short_audio_window_started", null, "started", "C_AAudio static_pcm_blob duration_ms=650");
        new Thread(() -> {
            int rc = -9999;
            try {
                rc = nativePlayStage1RShortBurst(650, 660, 0.30f);
                appendStage1QDLiveEvent("native_short_audio_window_complete", null, rc == 0 ? "complete" : "error", "C_AAudio rc=" + rc + " duration_ms=650");

                appendStage1QDLiveEvent("native_window_sequence_started", null, "started", "C_AAudio static_pcm_blob bursts=5 burst_ms=110 gap_ms=70");
                int seqRc = nativePlayStage1RWindowSequence(5, 110, 70, 0.28f);
                appendStage1QDLiveEvent("native_window_sequence_complete", null, seqRc == 0 ? "complete" : "error", "C_AAudio rc=" + seqRc + " bursts=5 burst_ms=110 gap_ms=70");

                appendStage1QDLiveEvent("native_mic_metrics_started", null, "started", "C_AAudio input_metrics duration_ms=900");
                String micDetail;
                try {
                    micDetail = nativeProbeStage1SMicWindow(900);
                } catch (Throwable t) {
                    micDetail = "rc=-99 step=java_throwable detail=" + t.toString();
                }
                appendStage1QDLiveEvent("native_mic_metrics_complete", null, micDetail.startsWith("rc=0") ? "complete" : "error", micDetail);

                appendStage1QDLiveEvent("native_mic_baseline_started", null, "started", "C_AAudio baseline windows=5 window_ms=180 gap_ms=60");
                String baselineDetail;
                try {
                    baselineDetail = nativeProbeStage1SMicBaseline(5, 180, 60);
                } catch (Throwable t) {
                    baselineDetail = "rc=-199 step=java_throwable detail=" + t.toString();
                }
                appendStage1QDLiveEvent("native_mic_baseline_complete", null, baselineDetail.startsWith("rc=0") ? "complete" : "error", baselineDetail);



            } catch (Throwable t) {
                appendStage1QDLiveEvent("native_short_audio_window_error", null, "error", t.toString());
            }
            final int rcFinal = rc;
            runOnUiThread(() -> setStage1QDStatus("Native C short audio window: " + (rcFinal == 0 ? "complete" : "error rc=" + rcFinal) + "\nLong-form station playback remains disabled.\nThis does not prove paranormal communication."));
        }, "fieldvox-stage1r-native-audio-window").start();
        runOnUiThread(() -> {
            if (liveStreamButton != null) {
                liveStreamButton.setText(R.string.live_stream_disabled);
                liveStreamButton.setEnabled(false);
            }
            setStage1QDStatus("Experimental Live Stream: disabled\nLong-form station playback is disabled.\nNext implementation must use short buffered windows only.\nThis does not prove paranormal communication.");
        });
    }

    private void stopStage1QDLiveStream(String reason) {
        FieldVoxLiveSource src = currentLiveSource;
        MediaPlayer mp = liveStreamPlayer;
        liveStreamPlayer = null;
        liveStreamActive = false;

        if (mp != null) {
            try {
                if (mp.isPlaying()) {
                    mp.stop();
                }
            } catch (Exception ignored) {
            }
            try {
                mp.release();
            } catch (Exception ignored) {
            }
        }

        appendStage1QDLiveEvent("live_stream_stopped", src, "stopped", reason);
        runOnUiThread(() -> {
            if (liveStreamButton != null) {
                liveStreamButton.setText(R.string.live_stream_disabled);
            }
            setStage1QDStatus(formatStage1QDStatus("stopped", src, reason));
        });
    }


    // FIELDVOX_STAGE1QE_SHORT_DWELL_HARD_STOP
    private void stopStage1QDLiveStreamIfActive(String reason) {
        if (liveStreamActive || liveStreamPlayer != null) {
            stopStage1QDLiveStream(reason);
        }
    }

    private FieldVoxLiveSource loadStage1QDLiveSource() throws Exception {
        File dir = new File(getFilesDir(), "fieldvox-hits");
        File publicSlots = new File(dir, "source_slots.json");
        File privateCache = new File(dir, "radio_browser_station_cache_private.jsonl");

        if (!publicSlots.isFile()) {
            throw new IllegalStateException("source_slots.json missing");
        }
        if (!privateCache.isFile()) {
            throw new IllegalStateException("private Radio Browser cache missing");
        }

        JSONObject root = new JSONObject(readStage1QDFile(publicSlots));
        JSONArray slots = root.optJSONArray("slots");
        if (slots == null) {
            slots = root.optJSONArray("source_slots");
        }
        if (slots == null) {
            slots = root.optJSONArray("session_pool");
        }
        if (slots == null || slots.length() == 0) {
            throw new IllegalStateException("no cached source slots found");
        }

        JSONObject slot = slots.getJSONObject(0);
        FieldVoxLiveSource src = new FieldVoxLiveSource();
        src.stationuuid = pickStage1QD(slot, "stationuuid", "station_uuid", "uuid");
        src.countrycode = pickStage1QD(slot, "countrycode", "country_code");
        src.sourceLabel = pickStage1QD(slot, "source_label", "label", "name");
        src.codec = pickStage1QD(slot, "codec");
        src.bitrate = pickStage1QD(slot, "bitrate");
        src.streamUrlSha256 = pickStage1QD(slot, "stream_url_sha256", "url_sha256");
        src.streamUrlResolvedSha256 = pickStage1QD(slot, "stream_url_resolved_sha256", "url_resolved_sha256");

        if (src.stationuuid.length() == 0) {
            throw new IllegalStateException("selected source has no stationuuid");
        }

        BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(privateCache), StandardCharsets.UTF_8));
        try {
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.length() == 0) {
                    continue;
                }
                JSONObject p = new JSONObject(line);
                String puuid = pickStage1QD(p, "stationuuid", "station_uuid", "uuid");
                if (!src.stationuuid.equals(puuid)) {
                    continue;
                }
                src.privateUrl = pickStage1QD(p, "url", "stream_url");
                src.privateUrlResolved = pickStage1QD(p, "url_resolved", "stream_url_resolved");
                if (src.sourceLabel.length() == 0) src.sourceLabel = pickStage1QD(p, "source_label", "name");
                if (src.countrycode.length() == 0) src.countrycode = pickStage1QD(p, "countrycode", "country_code");
                if (src.codec.length() == 0) src.codec = pickStage1QD(p, "codec");
                if (src.bitrate.length() == 0) src.bitrate = pickStage1QD(p, "bitrate");
                break;
            }
        } finally {
            br.close();
        }

        if ((src.privateUrl == null || src.privateUrl.length() == 0) && (src.privateUrlResolved == null || src.privateUrlResolved.length() == 0)) {
            throw new IllegalStateException("matched private cache row has no playable URL");
        }

        return src;
    }

    private String readStage1QDFile(File f) throws Exception {
        BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8));
        StringBuilder sb = new StringBuilder();
        try {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append('\n');
            }
        } finally {
            br.close();
        }
        return sb.toString();
    }

    private String pickStage1QD(JSONObject obj, String... keys) {
        for (String key : keys) {
            if (obj.has(key) && !obj.isNull(key)) {
                Object v = obj.opt(key);
                if (v != null) {
                    String out = String.valueOf(v);
                    if (out != null && out.length() > 0 && !"null".equals(out)) {
                        return out;
                    }
                }
            }
        }

        Iterator<String> it = obj.keys();
        while (it.hasNext()) {
            String key = it.next();
            for (String wanted : keys) {
                if (key.equalsIgnoreCase(wanted)) {
                    Object v = obj.opt(key);
                    if (v != null) {
                        String out = String.valueOf(v);
                        if (out != null && out.length() > 0 && !"null".equals(out)) {
                            return out;
                        }
                    }
                }
            }
        }
        return "";
    }

    private void setStage1QDStatus(String text) {
        if (liveStreamStatusText != null) {
            liveStreamStatusText.setText(text);
        }
    }

    private String formatStage1QDStatus(String state, FieldVoxLiveSource src, String detail) {
        StringBuilder sb = new StringBuilder();
        sb.append("Experimental Live Stream: ").append(state == null ? "unknown" : state).append('\n');
        sb.append("One active cached Radio Browser source. Raw URLs stay app-private.").append('\n');
        sb.append("This does not prove paranormal communication.").append('\n');

        if (src != null) {
            sb.append("Source: ").append(safeStage1QD(src.sourceLabel)).append('\n');
            sb.append("Country: ").append(safeStage1QD(src.countrycode)).append('\n');
            sb.append("UUID: ").append(safeStage1QD(src.stationuuid)).append('\n');
            sb.append("Codec/bitrate: ").append(safeStage1QD(src.codec)).append(" / ").append(safeStage1QD(src.bitrate)).append('\n');
            sb.append("URL hash: ").append(safeStage1QD(src.streamUrlSha256)).append('\n');
        }

        if (detail != null && detail.length() > 0) {
            sb.append("Detail: ").append(detail);
        }

        return sb.toString();
    }

    private String safeStage1QD(String s) {
        if (s == null || s.length() == 0) {
            return "(unknown)";
        }
        return s;
    }

    private void appendStage1QDLiveEvent(String eventType, FieldVoxLiveSource src, String state, String detail) {
        try {
            JSONObject ev = new JSONObject();
            ev.put("event_type", eventType);
            ev.put("stage", "1Q-D");
            ev.put("state", state == null ? "" : state);
            ev.put("detail", detail == null ? "" : detail);
            ev.put("raw_stream_url_exported", false);
            ev.put("private_cache_used", true);
            ev.put("playback_engine_changed", true);

            if (src != null) {
                ev.put("stationuuid", src.stationuuid == null ? "" : src.stationuuid);
                ev.put("countrycode", src.countrycode == null ? "" : src.countrycode);
                ev.put("source_label", src.sourceLabel == null ? "" : src.sourceLabel);
                ev.put("codec", src.codec == null ? "" : src.codec);
                ev.put("bitrate", src.bitrate == null ? "" : src.bitrate);
                ev.put("stream_url_sha256", src.streamUrlSha256 == null ? "" : src.streamUrlSha256);
                ev.put("stream_url_resolved_sha256", src.streamUrlResolvedSha256 == null ? "" : src.streamUrlResolvedSha256);
            }

            appendStage1QDJsonLine(ev.toString());
        } catch (Exception ignored) {
        }
    }

    private void appendStage1QDJsonLine(String line) {
        try {
            File dir = new File(getFilesDir(), "fieldvox-hits");
            if (!dir.exists()) {
                dir.mkdirs();
            }
            File out = new File(dir, "live_stream_events_stage1q_d.jsonl");
            java.io.FileOutputStream fos = new java.io.FileOutputStream(out, true);
            try {
                fos.write(line.getBytes(StandardCharsets.UTF_8));
                fos.write('\n');
            } finally {
                fos.close();
            }
        } catch (Exception ignored) {
        }
    }



    // FIELDVOX_STAGE1QE_SHORT_DWELL_HARD_STOP
    @Override
    protected void onPause() {
        stopStage1QDLiveStreamIfActive("activity_paused");
        super.onPause();
    }

}
