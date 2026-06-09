// SPDX-License-Identifier: GPL-3.0-or-later
package org.fieldvox.lab;

import android.app.Activity;
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
        @Override
        public void run() {
            status.setText(nativeStatus());
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

    private void toggleRun() {
        if (!running) {
            running = nativeStart();
            startButton.setText(running ? "Stop" : "Start!");
        } else {
            nativeStop();
            running = false;
            startButton.setText("Start");
            refreshSessions();
        }
        status.setText(nativeStatus());
        wavePanel.setRunning(running);
        wavePanel.invalidate();
    }

    private void toggleControls() {
        controlsVisible = !controlsVisible;
        controlPanel.setVisibility(controlsVisible ? View.VISIBLE : View.GONE);
    }

    @Override
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

        TextView stage = makeLabel("Stage 1J-A • session manifest console", Color.LTGRAY, 12);
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

        exportStatus = makeLabel("Exports: Downloads/FieldVoxLab • Share uses content URIs", Color.rgb(255, 230, 160), 12);
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

        controlPanel.addView(exportAllButton);
        controlPanel.addView(shareAllButton);
        controlPanel.addView(manifestButton);
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
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                threshold = 0.50f + ((float) progress / 100.0f);
                nativeSetThreshold(threshold);
                updateLabels();
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        cooldownBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                cooldownMs = 100 + progress;
                nativeSetCooldownMs(cooldownMs);
                updateLabels();
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        startButton.setOnClickListener(v -> toggleRun());

        sessionsButton.setOnClickListener(v -> {
            refreshSessions();
            exportStatus.setText("Session list refreshed.");
        });

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

        handler.post(tick);
    }

    @Override
    protected void onDestroy() {
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

        @Override
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

        @Override
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
