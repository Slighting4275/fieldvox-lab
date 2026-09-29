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
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Locale;

public final class RadioBrowserDiscoveryCache {
    public static final String STAGE = "1Q-B";
    public static final String USER_AGENT = "FieldVoxLab/0.1.0 (+https://codeberg.org/Exalted6165/fieldvox-lab)";
    private static final String DNS_DISCOVERY_HOST = "all.api.radio-browser.info";
    private static final int TARGET_CATALOG_COUNT = 1000;
    private static final int TARGET_SESSION_POOL_COUNT = 128;
    private static final int MAX_RESPONSE_CHARS = 24 * 1024 * 1024;

    static {
        try {
            System.loadLibrary("fieldvox");
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static native int nativeMinimumAcceptableCatalogCount(int target);
    private static native String nativeSha256Hex(String text);
    private static native String nativeCleanText(String text);

    private RadioBrowserDiscoveryCache() {
    }

    public static synchronized JSONObject refresh(Context context, int requestedSlots) throws Exception {
        int catalogTarget = Math.max(64, Math.min(TARGET_CATALOG_COUNT, requestedSlots));
        if (requestedSlots >= TARGET_CATALOG_COUNT) {
            catalogTarget = TARGET_CATALOG_COUNT;
        }
        int sessionPoolTarget = Math.min(TARGET_SESSION_POOL_COUNT, catalogTarget);

        long startedMs = System.currentTimeMillis();

        ArrayList<String> servers = discoverServers();
        if (servers.isEmpty()) {
            addFallbackServers(servers);
        }
        Collections.shuffle(servers);

        Exception lastError = null;
        String selectedServer = "";
        JSONArray rawStations = null;
        JSONArray queryUrls = new JSONArray();

        for (String server : servers) {
            try {
                rawStations = fetchCatalogFromServer(server, catalogTarget, queryUrls);
                selectedServer = server;
                if (rawStations != null && rawStations.length() > 0) {
                    break;
                }
            } catch (Exception e) {
                lastError = e;
            }
        }

        if (rawStations == null || rawStations.length() == 0) {
            if (lastError != null) {
                throw lastError;
            }
            throw new IllegalStateException("Radio Browser catalog discovery returned no station response");
        }

        JSONArray publicCatalog = new JSONArray();
        JSONArray privateCatalog = new JSONArray();
        LinkedHashSet<String> seenUuid = new LinkedHashSet<>();

        for (int i = 0; i < rawStations.length() && publicCatalog.length() < catalogTarget; i++) {
            JSONObject station = rawStations.optJSONObject(i);
            if (station == null) {
                continue;
            }

            String stationuuid = safeString(station, "stationuuid");
            String countrycode = safeString(station, "countrycode").toUpperCase(Locale.US);
            String name = cleanPublicText(safeString(station, "name"));
            String url = safeHttpUrl(safeString(station, "url"));
            String urlResolved = safeHttpUrl(safeString(station, "url_resolved"));
            String codec = cleanPublicText(safeString(station, "codec").toUpperCase(Locale.US));
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

            int index = publicCatalog.length();

            JSONObject publicStation = new JSONObject();
            publicStation.put("catalog_index", index);
            publicStation.put("source_label", name);
            publicStation.put("stationuuid", stationuuid);
            publicStation.put("countrycode", countrycode);
            publicStation.put("language", cleanPublicText(safeString(station, "language")));
            publicStation.put("languagecodes", cleanPublicText(safeString(station, "languagecodes")));
            publicStation.put("tags", cleanPublicText(safeString(station, "tags")));
            publicStation.put("codec", codec);
            publicStation.put("bitrate", station.optInt("bitrate", 0));
            publicStation.put("hls", hls);
            publicStation.put("lastcheckok", lastcheckok);
            publicStation.put("lastchecktime_iso8601", cleanPublicText(safeString(station, "lastchecktime_iso8601")));
            publicStation.put("lastcheckoktime_iso8601", cleanPublicText(safeString(station, "lastcheckoktime_iso8601")));
            publicStation.put("clickcount", station.optInt("clickcount", 0));
            publicStation.put("votes", station.optInt("votes", 0));
            String homepage = safeHttpUrl(safeString(station, "homepage"));
            publicStation.put("homepage_host_hash", sha256Hex(hostOnly(homepage)));
            publicStation.put("stream_url_sha256", url.isEmpty() ? "" : sha256Hex(url));
            publicStation.put("stream_url_resolved_sha256", urlResolved.isEmpty() ? "" : sha256Hex(urlResolved));
            publicStation.put("raw_stream_url_exported", false);
            publicStation.put("private_cache_file", "radio_browser_station_cache_private.jsonl");
            publicCatalog.put(publicStation);

            JSONObject privateStation = new JSONObject();
            privateStation.put("event_type", "radio_browser_private_station");
            privateStation.put("stage", STAGE);
            privateStation.put("catalog_index", index);
            privateStation.put("source_label", name);
            privateStation.put("stationuuid", stationuuid);
            privateStation.put("countrycode", countrycode);
            privateStation.put("url", url);
            privateStation.put("url_resolved", urlResolved);
            privateStation.put("homepage", homepage);
            privateStation.put("codec", codec);
            privateStation.put("bitrate", station.optInt("bitrate", 0));
            privateStation.put("lastcheckok", lastcheckok);
            privateStation.put("hls", hls);
            privateStation.put("raw_stream_url_private_only", true);
            privateCatalog.put(privateStation);
        }

        int minAcceptable = minimumAcceptableCatalogCount(catalogTarget);
        if (publicCatalog.length() < minAcceptable) {
            throw new IllegalStateException("Radio Browser vetted catalog count too low: " + publicCatalog.length());
        }

        JSONArray sourcePool = buildSessionPool(publicCatalog, sessionPoolTarget);
        long finishedMs = System.currentTimeMillis();

        JSONArray serverArray = new JSONArray();
        for (String s : servers) {
            serverArray.put(s);
        }

        JSONObject catalog = new JSONObject();
        catalog.put("event_type", "radio_browser_catalog");
        catalog.put("stage", STAGE);
        catalog.put("catalog_provider", "radio_browser_live_discovery_catalog_v1");
        catalog.put("radio_browser_compatible", true);
        catalog.put("server_discovery_policy", "dns_all_api_radio_browser_info_randomize_retry");
        catalog.put("server_discovery_host", DNS_DISCOVERY_HOST);
        catalog.put("server_candidates", serverArray);
        catalog.put("server_selected", selectedServer);
        catalog.put("catalog_target_count", catalogTarget);
        catalog.put("catalog_count", publicCatalog.length());
        catalog.put("session_pool_target_count", sessionPoolTarget);
        catalog.put("http_user_agent", USER_AGENT);
        catalog.put("uuid_policy", "stationuuid_required");
        catalog.put("countrycode_policy", "countrycode_required");
        catalog.put("hidebroken", true);
        catalog.put("hls_allowed", false);
        catalog.put("raw_stream_urls_exported", false);
        catalog.put("raw_stream_urls_private_cache_only", true);
        catalog.put("playback_engine_changed", false);
        catalog.put("cache_created_unix_ms", finishedMs);
        catalog.put("discovery_duration_ms", finishedMs - startedMs);
        catalog.put("query_urls", queryUrls);
        catalog.put("stations", publicCatalog);

        JSONObject sourceSlots = new JSONObject();
        sourceSlots.put("event_type", "source_slots");
        sourceSlots.put("stage", STAGE);
        sourceSlots.put("catalog_provider", "radio_browser_live_discovery_catalog_v1");
        sourceSlots.put("radio_browser_compatible", true);
        sourceSlots.put("source_pool_policy", "session_randomized_pool_from_1000_station_catalog");
        sourceSlots.put("catalog_file", "radio_browser_catalog.json");
        sourceSlots.put("catalog_count", publicCatalog.length());
        sourceSlots.put("slot_count", sourcePool.length());
        sourceSlots.put("session_pool_target_count", sessionPoolTarget);
        sourceSlots.put("server_selected", selectedServer);
        sourceSlots.put("http_user_agent", USER_AGENT);
        sourceSlots.put("uuid_policy", "stationuuid_required");
        sourceSlots.put("countrycode_policy", "countrycode_required");
        sourceSlots.put("raw_stream_urls_exported", false);
        sourceSlots.put("raw_stream_urls_private_cache_only", true);
        sourceSlots.put("playback_engine_changed", false);
        sourceSlots.put("cache_created_unix_ms", finishedMs);
        sourceSlots.put("slots", sourcePool);

        JSONObject manifest = new JSONObject();
        manifest.put("event_type", "radio_browser_discovery_manifest");
        manifest.put("stage", STAGE);
        manifest.put("catalog_provider", "radio_browser_live_discovery_catalog_v1");
        manifest.put("radio_browser_compatible", true);
        manifest.put("server_discovery_policy", "dns_all_api_radio_browser_info_randomize_retry");
        manifest.put("server_discovery_host", DNS_DISCOVERY_HOST);
        manifest.put("server_candidates", serverArray);
        manifest.put("server_selected", selectedServer);
        manifest.put("query_urls", queryUrls);
        manifest.put("http_user_agent", USER_AGENT);
        manifest.put("uuid_policy", "stationuuid_required");
        manifest.put("countrycode_policy", "countrycode_required");
        manifest.put("raw_response_count", rawStations.length());
        manifest.put("vetted_catalog_count", publicCatalog.length());
        manifest.put("session_pool_count", sourcePool.length());
        manifest.put("raw_stream_urls_exported", false);
        manifest.put("private_cache_contains_raw_stream_urls", true);
        manifest.put("playback_engine_changed", false);
        manifest.put("cache_created_unix_ms", finishedMs);
        manifest.put("discovery_duration_ms", finishedMs - startedMs);
        manifest.put("catalog_file", "radio_browser_catalog.json");
        manifest.put("source_slots_file", "source_slots.json");
        manifest.put("private_cache_file", "radio_browser_station_cache_private.jsonl");

        File dir = new File(context.getFilesDir(), "fieldvox-hits");
        if ((!dir.exists() && !dir.mkdirs()) || !dir.isDirectory()) {
            throw new IllegalStateException("Cannot create fieldvox-hits directory");
        }

        writeText(new File(dir, "radio_browser_catalog.json"), catalog.toString() + "\n");
        writeText(new File(dir, "source_slots.json"), sourceSlots.toString() + "\n");
        writeText(new File(dir, "radio_browser_discovery_manifest.json"), manifest.toString() + "\n");
        writeText(new File(dir, "radio_browser_station_cache_private.jsonl"), jsonl(privateCatalog));
        writeText(new File(dir, "RADIO_BROWSER_CACHE_README.txt"), readmeText());

        return sourceSlots;
    }

    static int minimumAcceptableCatalogCount(int catalogTarget) {
        try {
            return nativeMinimumAcceptableCatalogCount(catalogTarget);
        } catch (UnsatisfiedLinkError ignored) {
            return Math.min(500, Math.max(1, catalogTarget / 2));
        }
    }

    private static JSONArray buildSessionPool(JSONArray publicCatalog, int sessionPoolTarget) throws Exception {
        ArrayList<Integer> indexes = new ArrayList<>();
        for (int i = 0; i < publicCatalog.length(); i++) {
            indexes.add(i);
        }
        Collections.shuffle(indexes);

        JSONArray pool = new JSONArray();
        int count = Math.min(sessionPoolTarget, indexes.size());

        for (int i = 0; i < count; i++) {
            JSONObject src = publicCatalog.getJSONObject(indexes.get(i));
            JSONObject slot = new JSONObject();
            slot.put("slot_index", i);
            slot.put("slot_id", "radio_browser_session_slot_" + i);
            slot.put("catalog_index", src.optInt("catalog_index", -1));
            slot.put("source_label", src.optString("source_label", ""));
            slot.put("stationuuid", src.optString("stationuuid", ""));
            slot.put("countrycode", src.optString("countrycode", ""));
            slot.put("language", src.optString("language", ""));
            slot.put("languagecodes", src.optString("languagecodes", ""));
            slot.put("tags", src.optString("tags", ""));
            slot.put("codec", src.optString("codec", ""));
            slot.put("bitrate", src.optInt("bitrate", 0));
            slot.put("hls", src.optInt("hls", 0));
            slot.put("lastcheckok", src.optInt("lastcheckok", 0));
            slot.put("clickcount", src.optInt("clickcount", 0));
            slot.put("votes", src.optInt("votes", 0));
            slot.put("homepage_host_hash", src.optString("homepage_host_hash", ""));
            slot.put("stream_url_sha256", src.optString("stream_url_sha256", ""));
            slot.put("stream_url_resolved_sha256", src.optString("stream_url_resolved_sha256", ""));
            slot.put("raw_stream_url_exported", false);
            slot.put("private_cache_file", "radio_browser_station_cache_private.jsonl");
            pool.put(slot);
        }

        return pool;
    }

    private static JSONArray fetchCatalogFromServer(String server, int targetCount, JSONArray queryUrls) throws Exception {
        JSONArray merged = new JSONArray();
        LinkedHashSet<String> seenUuid = new LinkedHashSet<>();

        ArrayList<String> paths = new ArrayList<>();
        paths.add("/json/stations/search?hidebroken=true&order=random&limit=" + targetCount);
        paths.add("/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=" + targetCount);
        paths.add("/json/stations/search?hidebroken=true&order=votes&reverse=true&limit=" + targetCount);
        paths.add("/json/stations/topclick?hidebroken=true&limit=" + targetCount);
        paths.add("/json/stations/topvote?hidebroken=true&limit=" + targetCount);
        paths.add("/json/stations/lastclick?hidebroken=true&limit=" + targetCount);

        for (String path : paths) {
            if (merged.length() >= targetCount) {
                break;
            }
            String url = "https://" + server + path;
            queryUrls.put(url);
            JSONArray arr = new JSONArray(httpGet(url));
            for (int i = 0; i < arr.length() && merged.length() < targetCount * 2; i++) {
                JSONObject station = arr.optJSONObject(i);
                if (station == null) {
                    continue;
                }
                String uuid = safeString(station, "stationuuid");
                if (uuid.length() == 0 || seenUuid.contains(uuid)) {
                    continue;
                }
                seenUuid.add(uuid);
                merged.put(station);
                if (merged.length() >= targetCount) {
                    break;
                }
            }
        }

        return merged;
    }

    private static ArrayList<String> discoverServers() {
        LinkedHashSet<String> set = new LinkedHashSet<>();
        try {
            InetAddress[] addresses = InetAddress.getAllByName(DNS_DISCOVERY_HOST);
            for (InetAddress address : addresses) {
                addIfServerName(set, address.getCanonicalHostName());
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
        try {
            conn.setConnectTimeout(12000);
            conn.setReadTimeout(24000);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("User-Agent", USER_AGENT);
            conn.setRequestProperty("Accept", "application/json");
            conn.setRequestProperty("Cache-Control", "no-cache");

            int rc = conn.getResponseCode();
            InputStream in = (rc >= 200 && rc < 300) ? conn.getInputStream() : conn.getErrorStream();
            String body = readAll(in);

            if (rc < 200 || rc >= 300) {
                throw new IllegalStateException("HTTP " + rc + " from " + httpsUrl + ": " + body);
            }
            return body;
        } finally {
            conn.disconnect();
        }
    }

    private static String readAll(InputStream in) throws Exception {
        if (in == null) {
            return "";
        }
        StringBuilder b = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
            char[] buf = new char[8192];
            int n;
            while ((n = r.read(buf)) >= 0) {
                b.append(buf, 0, n);
                if (b.length() > MAX_RESPONSE_CHARS) {
                    throw new IllegalStateException("Radio Browser response too large");
                }
            }
        }
        return b.toString();
    }

    private static String safeString(JSONObject object, String key) {
        String s = object.optString(key, "");
        return s == null ? "" : s.trim();
    }

    private static String cleanPublicText(String s) {
        if (s == null) {
            return "";
        }
        try {
            return nativeCleanText(s);
        } catch (UnsatisfiedLinkError ignored) {
            String t = s.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ').trim();
            t = t.replace("http://", "hxxp://").replace("https://", "hxxps://");
            if (t.length() > 240) {
                t = t.substring(0, 240);
            }
            return t;
        }
    }

    private static String safeHttpUrl(String value) {
        if (value == null) {
            return "";
        }
        String candidate = value.trim();
        if (candidate.isEmpty() || candidate.length() > 4096) {
            return "";
        }
        try {
            URI uri = new URI(candidate);
            String scheme = uri.getScheme();
            if (scheme == null
                    || (!(scheme.equalsIgnoreCase("http")) && !(scheme.equalsIgnoreCase("https")))
                    || uri.getHost() == null
                    || uri.getHost().isEmpty()
                    || uri.getUserInfo() != null) {
                return "";
            }
            return candidate;
        } catch (Exception ignored) {
            return "";
        }
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

    private static String sha256Hex(String text) {
        if (text == null) {
            return "";
        }
        try {
            return nativeSha256Hex(text);
        } catch (UnsatisfiedLinkError ignored) {
            try {
                MessageDigest md = MessageDigest.getInstance("SHA-256");
                byte[] data = md.digest(text.getBytes(StandardCharsets.UTF_8));
                StringBuilder b = new StringBuilder(data.length * 2);
                for (byte x : data) {
                    b.append(String.format(Locale.US, "%02x", x & 0xff));
                }
                return b.toString();
            } catch (Exception e) {
                return "";
            }
        }
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
        try {
            try (FileOutputStream out = new FileOutputStream(tmp, false)) {
                out.write(text.getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.getFD().sync();
            }
            try {
                Files.move(tmp.toPath(), file.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            if (tmp.exists() && !tmp.delete()) {
                tmp.deleteOnExit();
            }
        }
    }

    private static String readmeText() {
        return ""
            + "FieldVox Lab Stage 1Q-B Radio Browser 1000-catalog cache.\n"
            + "\n"
            + "This stage scales the Stage 1Q-A Radio Browser cache from a small proof cache to a larger catalog cache.\n"
            + "\n"
            + "Playback is intentionally unchanged in Stage 1Q-B.\n"
            + "\n"
            + "Export-safe files:\n"
            + "- radio_browser_catalog.json: up to 1000 vetted stations, metadata and stream URL hashes only.\n"
            + "- source_slots.json: 64 randomized per-session source slots selected from the catalog.\n"
            + "- radio_browser_discovery_manifest.json: discovery policy and counts.\n"
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
