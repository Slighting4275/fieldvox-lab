package org.fieldvox.lab;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Locale;

public final class RadioBrowserDiscoveryCache {
    public static final String STAGE = "1Q-A";
    public static final String USER_AGENT = "FieldVoxLab/0.1.0-stage1q-a (+https://codeberg.org/Exalted6165/fieldvox-lab)";
    private static final String DNS_DISCOVERY_HOST = "all.api.radio-browser.info";
    private static final String QUERY_PATH = "/json/stations/search?countrycode=US&language=english&hidebroken=true&order=clickcount&reverse=true&limit=80";

    private RadioBrowserDiscoveryCache() {
    }

    public static JSONObject refresh(Context context, int requestedSlots) throws Exception {
        int targetSlots = Math.max(4, Math.min(24, requestedSlots));
        long startedMs = System.currentTimeMillis();

        ArrayList<String> servers = discoverServers();
        if (servers.isEmpty()) {
            addFallbackServers(servers);
        }
        Collections.shuffle(servers);

        Exception lastError = null;
        String selectedServer = "";
        JSONArray rawStations = null;
        String selectedUrl = "";

        for (String server : servers) {
            try {
                selectedUrl = "https://" + server + QUERY_PATH;
                String body = httpGet(selectedUrl);
                rawStations = new JSONArray(body);
                selectedServer = server;
                break;
            } catch (Exception e) {
                lastError = e;
            }
        }

        if (rawStations == null) {
            if (lastError != null) {
                throw lastError;
            }
            throw new IllegalStateException("Radio Browser discovery returned no server response");
        }

        JSONArray slots = new JSONArray();
        JSONArray privateStations = new JSONArray();
        LinkedHashSet<String> seenUuid = new LinkedHashSet<>();

        for (int i = 0; i < rawStations.length() && slots.length() < targetSlots; i++) {
            JSONObject station = rawStations.optJSONObject(i);
            if (station == null) {
                continue;
            }

            String stationuuid = safeString(station, "stationuuid");
            String countrycode = safeString(station, "countrycode").toUpperCase(Locale.US);
            String name = safeString(station, "name");
            String url = safeString(station, "url");
            String urlResolved = safeString(station, "url_resolved");
            String codec = safeString(station, "codec").toUpperCase(Locale.US);
            int lastcheckok = station.optInt("lastcheckok", 0);
            int hls = station.optInt("hls", 0);

            if (stationuuid.length() < 8 || seenUuid.contains(stationuuid)) {
                continue;
            }
            if (countrycode.length() != 2) {
                continue;
            }
            if (name.length() == 0) {
                continue;
            }
            if (lastcheckok != 1) {
                continue;
            }
            if (hls != 0) {
                continue;
            }
            if (url.length() == 0 && urlResolved.length() == 0) {
                continue;
            }

            seenUuid.add(stationuuid);

            JSONObject slot = new JSONObject();
            slot.put("slot_index", slots.length());
            slot.put("slot_id", "radio_browser_slot_" + slots.length());
            slot.put("source_label", name);
            slot.put("stationuuid", stationuuid);
            slot.put("countrycode", countrycode);
            slot.put("language", safeString(station, "language"));
            slot.put("languagecodes", safeString(station, "languagecodes"));
            slot.put("tags", safeString(station, "tags"));
            slot.put("codec", codec);
            slot.put("bitrate", station.optInt("bitrate", 0));
            slot.put("hls", hls);
            slot.put("lastcheckok", lastcheckok);
            slot.put("lastchecktime_iso8601", safeString(station, "lastchecktime_iso8601"));
            slot.put("lastcheckoktime_iso8601", safeString(station, "lastcheckoktime_iso8601"));
            slot.put("clickcount", station.optInt("clickcount", 0));
            slot.put("votes", station.optInt("votes", 0));
            slot.put("homepage_host_hash", sha256Hex(hostOnly(safeString(station, "homepage"))));
            slot.put("stream_url_sha256", sha256Hex(url));
            slot.put("stream_url_resolved_sha256", sha256Hex(urlResolved));
            slot.put("raw_stream_url_exported", false);
            slot.put("private_cache_file", "radio_browser_station_cache_private.jsonl");
            slots.put(slot);

            JSONObject privateStation = new JSONObject();
            privateStation.put("event_type", "radio_browser_private_station");
            privateStation.put("stage", STAGE);
            privateStation.put("slot_index", slots.length() - 1);
            privateStation.put("source_label", name);
            privateStation.put("stationuuid", stationuuid);
            privateStation.put("countrycode", countrycode);
            privateStation.put("url", url);
            privateStation.put("url_resolved", urlResolved);
            privateStation.put("homepage", safeString(station, "homepage"));
            privateStation.put("codec", codec);
            privateStation.put("bitrate", station.optInt("bitrate", 0));
            privateStation.put("lastcheckok", lastcheckok);
            privateStation.put("raw_stream_url_private_only", true);
            privateStations.put(privateStation);
        }

        if (slots.length() < 4) {
            throw new IllegalStateException("Radio Browser vetted slot count too low: " + slots.length());
        }

        long finishedMs = System.currentTimeMillis();

        JSONArray serverArray = new JSONArray();
        for (String s : servers) {
            serverArray.put(s);
        }

        JSONObject sourceSlots = new JSONObject();
        sourceSlots.put("event_type", "source_slots");
        sourceSlots.put("stage", STAGE);
        sourceSlots.put("catalog_provider", "radio_browser_live_discovery_cache_v1");
        sourceSlots.put("radio_browser_compatible", true);
        sourceSlots.put("server_discovery_policy", "dns_all_api_radio_browser_info_randomize_retry");
        sourceSlots.put("server_discovery_host", DNS_DISCOVERY_HOST);
        sourceSlots.put("server_candidates", serverArray);
        sourceSlots.put("server_selected", selectedServer);
        sourceSlots.put("query_path", QUERY_PATH);
        sourceSlots.put("http_user_agent", USER_AGENT);
        sourceSlots.put("uuid_policy", "stationuuid_required");
        sourceSlots.put("countrycode_policy", "countrycode_required");
        sourceSlots.put("hidebroken", true);
        sourceSlots.put("language_filter", "english");
        sourceSlots.put("countrycode_filter", "US");
        sourceSlots.put("small_vetted_list", true);
        sourceSlots.put("raw_stream_urls_exported", false);
        sourceSlots.put("raw_stream_urls_private_cache_only", true);
        sourceSlots.put("playback_engine_changed", false);
        sourceSlots.put("cache_created_unix_ms", finishedMs);
        sourceSlots.put("discovery_duration_ms", finishedMs - startedMs);
        sourceSlots.put("slot_count", slots.length());
        sourceSlots.put("slots", slots);

        JSONObject manifest = new JSONObject();
        manifest.put("event_type", "radio_browser_discovery_manifest");
        manifest.put("stage", STAGE);
        manifest.put("catalog_provider", "radio_browser_live_discovery_cache_v1");
        manifest.put("radio_browser_compatible", true);
        manifest.put("server_discovery_policy", "dns_all_api_radio_browser_info_randomize_retry");
        manifest.put("server_discovery_host", DNS_DISCOVERY_HOST);
        manifest.put("server_candidates", serverArray);
        manifest.put("server_selected", selectedServer);
        manifest.put("request_url", selectedUrl);
        manifest.put("http_user_agent", USER_AGENT);
        manifest.put("uuid_policy", "stationuuid_required");
        manifest.put("countrycode_policy", "countrycode_required");
        manifest.put("query_path", QUERY_PATH);
        manifest.put("raw_station_count", rawStations.length());
        manifest.put("vetted_slot_count", slots.length());
        manifest.put("raw_stream_urls_exported", false);
        manifest.put("private_cache_contains_raw_stream_urls", true);
        manifest.put("playback_engine_changed", false);
        manifest.put("cache_created_unix_ms", finishedMs);
        manifest.put("discovery_duration_ms", finishedMs - startedMs);
        manifest.put("source_slots_file", "source_slots.json");
        manifest.put("private_cache_file", "radio_browser_station_cache_private.jsonl");

        File dir = new File(context.getFilesDir(), "fieldvox-hits");
        if (!dir.exists() && !dir.mkdirs()) {
            throw new IllegalStateException("Cannot create fieldvox-hits directory");
        }

        writeText(new File(dir, "source_slots.json"), sourceSlots.toString() + "\n");
        writeText(new File(dir, "radio_browser_discovery_manifest.json"), manifest.toString() + "\n");
        writeText(new File(dir, "radio_browser_station_cache_private.jsonl"), jsonl(privateStations));
        writeText(new File(dir, "RADIO_BROWSER_CACHE_README.txt"), readmeText());

        return sourceSlots;
    }

    private static ArrayList<String> discoverServers() {
        LinkedHashSet<String> set = new LinkedHashSet<>();
        try {
            InetAddress[] addresses = InetAddress.getAllByName(DNS_DISCOVERY_HOST);
            for (InetAddress address : addresses) {
                String canonical = address.getCanonicalHostName();
                addIfServerName(set, canonical);
                addIfServerName(set, address.getHostName());
            }
        } catch (Throwable ignored) {
        }

        ArrayList<String> out = new ArrayList<>(set);
        if (out.isEmpty()) {
            addFallbackServers(out);
        }
        return out;
    }

    private static void addFallbackServers(ArrayList<String> servers) {
        addIfMissing(servers, "de1.api.radio-browser.info");
        addIfMissing(servers, "nl1.api.radio-browser.info");
        addIfMissing(servers, "at1.api.radio-browser.info");
        addIfMissing(servers, "fi1.api.radio-browser.info");
        addIfMissing(servers, "fr1.api.radio-browser.info");
    }

    private static void addIfServerName(LinkedHashSet<String> set, String host) {
        if (host == null) {
            return;
        }
        String h = host.trim().toLowerCase(Locale.US);
        if (h.endsWith(".api.radio-browser.info") && !DNS_DISCOVERY_HOST.equals(h)) {
            set.add(h);
        }
    }

    private static void addIfMissing(ArrayList<String> list, String value) {
        if (!list.contains(value)) {
            list.add(value);
        }
    }

    private static String httpGet(String httpsUrl) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(httpsUrl).openConnection();
        conn.setConnectTimeout(8000);
        conn.setReadTimeout(14000);
        conn.setRequestMethod("GET");
        conn.setRequestProperty("User-Agent", USER_AGENT);
        conn.setRequestProperty("Accept", "application/json");
        conn.setRequestProperty("Cache-Control", "no-cache");

        int rc = conn.getResponseCode();
        InputStream in = (rc >= 200 && rc < 300) ? conn.getInputStream() : conn.getErrorStream();
        String body = readAll(in);
        conn.disconnect();

        if (rc < 200 || rc >= 300) {
            throw new IllegalStateException("HTTP " + rc + " from " + httpsUrl + ": " + body);
        }
        return body;
    }

    private static String readAll(InputStream in) throws Exception {
        if (in == null) {
            return "";
        }
        StringBuilder b = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        char[] buf = new char[4096];
        int n;
        while ((n = r.read(buf)) >= 0) {
            b.append(buf, 0, n);
            if (b.length() > 4 * 1024 * 1024) {
                throw new IllegalStateException("Radio Browser response too large");
            }
        }
        r.close();
        return b.toString();
    }

    private static String safeString(JSONObject object, String key) {
        String s = object.optString(key, "");
        return s == null ? "" : s.trim();
    }

    private static String hostOnly(String url) {
        try {
            if (url == null || url.trim().isEmpty()) {
                return "";
            }
            return new URL(url).getHost();
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static String sha256Hex(String text) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] data = md.digest((text == null ? "" : text).getBytes(StandardCharsets.UTF_8));
        StringBuilder b = new StringBuilder(data.length * 2);
        for (byte x : data) {
            b.append(String.format(Locale.US, "%02x", x & 0xff));
        }
        return b.toString();
    }

    private static String jsonl(JSONArray array) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < array.length(); i++) {
            JSONObject o = array.optJSONObject(i);
            if (o != null) {
                b.append(o.toString()).append('\n');
            }
        }
        return b.toString();
    }

    private static void writeText(File file, String text) throws Exception {
        File tmp = new File(file.getParentFile(), file.getName() + ".tmp");
        FileOutputStream out = new FileOutputStream(tmp, false);
        out.write(text.getBytes(StandardCharsets.UTF_8));
        out.flush();
        out.close();
        if (file.exists() && !file.delete()) {
            throw new IllegalStateException("Could not replace " + file.getName());
        }
        if (!tmp.renameTo(file)) {
            throw new IllegalStateException("Could not rename " + tmp.getName() + " to " + file.getName());
        }
    }

    private static String readmeText() {
        return ""
            + "FieldVox Lab Stage 1Q-A Radio Browser discovery cache.\n"
            + "\n"
            + "This stage discovers Radio Browser API servers, randomizes/retries them, sends a descriptive User-Agent, fetches stationuuid and countrycode metadata, and writes a small vetted station cache.\n"
            + "\n"
            + "Playback is intentionally unchanged in Stage 1Q-A.\n"
            + "\n"
            + "Export-safe file:\n"
            + "- source_slots.json: station metadata and stream URL hashes only.\n"
            + "\n"
            + "App-private file:\n"
            + "- radio_browser_station_cache_private.jsonl: raw stream URLs for future playback wiring. Do not export this file to public review bundles.\n"
            + "\n"
            + "Safety/evidence notes:\n"
            + "- raw_stream_urls_exported=false\n"
            + "- playback_engine_changed=false\n"
            + "- uuid_policy=stationuuid_required\n"
            + "- countrycode_policy=countrycode_required\n";
    }
}
