const std = @import("std");

pub const StationRecord = extern struct {
    slot_id: u32,
    callsign: [32]u8,
    name: [64]u8,
    country: [4]u8,
    lang: [4]u8,
    url_hash: [32]u8,
    url: [160]u8,
};

pub const BlobHeader = extern struct {
    magic: u32,
    version: u32,
    station_count: u32,
    checksum_crc32: u32,
};

fn padString(comptime N: usize, src: []const u8) [N]u8 {
    var out: [N]u8 = [_]u8{0} ** N;
    const len = if (src.len > N - 1) N - 1 else src.len;
    @memcpy(out[0..len], src[0..len]);
    return out;
}

fn writeStr(fp: *std.c.FILE, s: []const u8) void {
    _ = std.c.fwrite(s.ptr, 1, s.len, fp);
}

const STATIONS = [_]struct {
    slot: u32,
    callsign: []const u8,
    name: []const u8,
    country: []const u8,
    lang: []const u8,
    hash: []const u8,
    url: []const u8,
}{
    .{ .slot = 0, .callsign = "KIRO 97.3", .name = "KIRO Newsradio Seattle 97.3", .country = "US", .lang = "en", .hash = "hash_kiro", .url = "https://bonneville.cdnstream1.com/2643_48.aac" },
    .{ .slot = 1, .callsign = "KOMO 1000", .name = "Northwest Newsradio KOMO/KNWN", .country = "US", .lang = "en", .hash = "hash_komo", .url = "https://playerservices.streamtheworld.com/api/livestream-redirect/bounded-fieldAM.mp3" },
    .{ .slot = 2, .callsign = "KUOW 94.9", .name = "KUOW Seattle 94.9", .country = "US", .lang = "en", .hash = "hash_kuow", .url = "https://playerservices.streamtheworld.com/api/livestream-redirect/bounded-fieldFM_LOW_MP3.mp3" },
    .{ .slot = 3, .callsign = "BBC WS", .name = "BBC World Service", .country = "GB", .lang = "en", .hash = "hash_bbc", .url = "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service" },
    .{ .slot = 4, .callsign = "WNYC AM", .name = "WNYC AM 820 New York", .country = "US", .lang = "en", .hash = "hash_wnyc", .url = "https://fm939.wnyc.org/wnycam" },
    .{ .slot = 5, .callsign = "WBUR 90.9", .name = "WBUR 90.9 Boston", .country = "US", .lang = "en", .hash = "hash_wbur", .url = "https://wbur-sc.streamguys1.com/wbur" },
    .{ .slot = 6, .callsign = "WAMU 88.5", .name = "WAMU 88.5 Washington DC", .country = "US", .lang = "en", .hash = "hash_wamu", .url = "https://wamu-1.streamguys1.com/" },
    .{ .slot = 7, .callsign = "MPR News", .name = "Minnesota Public Radio News", .country = "US", .lang = "en", .hash = "hash_mpr", .url = "https://nis.stream.publicradio.org/nis.mp3" },
    .{ .slot = 8, .callsign = "VPR News", .name = "Vermont Public Radio", .country = "US", .lang = "en", .hash = "hash_vpr", .url = "https://vpr.streamguys1.com/vpr64.mp3" },
    .{ .slot = 9, .callsign = "WBEZ 91.5", .name = "WBEZ 91.5 Chicago", .country = "US", .lang = "en", .hash = "hash_wbez", .url = "https://stream.wbez.org/wbez128.mp3" },
    .{ .slot = 10, .callsign = "OPB Radio", .name = "Oregon Public Broadcasting OPB", .country = "US", .lang = "en", .hash = "hash_opb", .url = "https://stream1.opb.org/radio.mp3" },
    .{ .slot = 11, .callsign = "KCRW 89.9", .name = "KCRW 89.9 Santa Monica", .country = "US", .lang = "en", .hash = "hash_kcrw", .url = "https://kcrw.streamguys1.com/kcrw_192k_mp3_news" },
    .{ .slot = 12, .callsign = "WUNC 91.5", .name = "WUNC 91.5 North Carolina", .country = "US", .lang = "en", .hash = "hash_wunc", .url = "https://wunc-ice.streamguys1.com/wunc-128-mp3" },
    .{ .slot = 13, .callsign = "WHYY 90.9", .name = "WHYY 90.9 Philadelphia", .country = "US", .lang = "en", .hash = "hash_whyy", .url = "https://whyy.streamguys1.com/whyy-mp3" },
    .{ .slot = 14, .callsign = "KERA 90.1", .name = "KERA 90.1 North Texas", .country = "US", .lang = "en", .hash = "hash_kera", .url = "https://kera-ice.streamguys1.com/kera" },
    .{ .slot = 15, .callsign = "WLRN 91.3", .name = "WLRN 91.3 South Florida", .country = "US", .lang = "en", .hash = "hash_wlrn", .url = "https://stream.wlrn.mobi/WLRNFMAAC" },
    .{ .slot = 16, .callsign = "KQED 88.5", .name = "KQED 88.5 San Francisco", .country = "US", .lang = "en", .hash = "hash_kqed", .url = "https://streams.kqed.org/kqedradio" },
    .{ .slot = 17, .callsign = "WPR Ideas", .name = "Wisconsin Public Radio News", .country = "US", .lang = "en", .hash = "hash_wpr", .url = "https://wpr-ice.streamguys1.com/wpr-ideas-mp3-64" },
    .{ .slot = 18, .callsign = "IPR News", .name = "Iowa Public Radio News", .country = "US", .lang = "en", .hash = "hash_ipr", .url = "https://news-stream.iowapublicradio.org/NewsLow.mp3" },
    .{ .slot = 19, .callsign = "NOAA Wx", .name = "NOAA Puget Sound Marine Weather", .country = "US", .lang = "en", .hash = "hash_noaa", .url = "http://161.55.224.79:88" },
    .{ .slot = 20, .callsign = "C-SPAN", .name = "C-SPAN Radio Washington", .country = "US", .lang = "en", .hash = "hash_cspan", .url = "https://playerservices.streamtheworld.com/api/livestream-redirect/CSPANRADIO.mp3" },
    .{ .slot = 21, .callsign = "CBC One", .name = "CBC Radio One Toronto", .country = "CA", .lang = "en", .hash = "hash_cbc", .url = "https://cbcradiolive.akamaized.net/hls/live/2041267/ES_R1NAT/master.m3u8" },
    .{ .slot = 22, .callsign = "RTE Radio 1", .name = "RTE Radio 1 Dublin", .country = "IE", .lang = "en", .hash = "hash_rte", .url = "https://icecast2.rte.ie/radio1" },
    .{ .slot = 23, .callsign = "ABC News", .name = "ABC NewsRadio Sydney", .country = "AU", .lang = "en", .hash = "hash_abc", .url = "https://live-radio01.mediahubaustralia.com/2NEWS/mp3/" },
    .{ .slot = 24, .callsign = "BBC R4", .name = "BBC Radio 4 London", .country = "GB", .lang = "en", .hash = "hash_bbc4", .url = "http://stream.live.vc.bbcmedia.co.uk/bbc_radio_fourfm" },
    .{ .slot = 25, .callsign = "FranceInfo", .name = "France Info Paris", .country = "FR", .lang = "fr", .hash = "hash_finfo", .url = "https://icecast.radiofrance.fr/franceinfo-midfi.mp3" },
    .{ .slot = 26, .callsign = "DW English", .name = "Deutsche Welle English", .country = "DE", .lang = "en", .hash = "hash_dw", .url = "https://dwstream4-lh.akamaihd.net/i/dwstream4_live@123962/master.m3u8" },
    .{ .slot = 27, .callsign = "NHK Tokyo", .name = "NHK World Radio Tokyo", .country = "JP", .lang = "en", .hash = "hash_nhk", .url = "https://m-radiko.nhk.jp/world_en/live.m3u8" },
    .{ .slot = 28, .callsign = "RNZ National", .name = "Radio New Zealand National", .country = "NZ", .lang = "en", .hash = "hash_rnz", .url = "https://radionz-ice.streamguys.com/national.mp3" },
    .{ .slot = 29, .callsign = "CPR News", .name = "Colorado Public Radio CPR", .country = "US", .lang = "en", .hash = "hash_cpr", .url = "https://cpr.streamguys1.com/cpr1_128" },
    .{ .slot = 30, .callsign = "TPR KSTX", .name = "Texas Public Radio KSTX", .country = "US", .lang = "en", .hash = "hash_tpr", .url = "https://tpr.streamguys1.com/tpr" },
    .{ .slot = 31, .callsign = "GPB News", .name = "Georgia Public Broadcasting GPB", .country = "US", .lang = "en", .hash = "hash_gpb", .url = "https://gpb.streamguys1.com/gpb-hd1" },
    .{ .slot = 32, .callsign = "WUOM 91.7", .name = "Michigan Radio WUOM", .country = "US", .lang = "en", .hash = "hash_wuom", .url = "https://wuom.streamguys1.com/wuom1" },
    .{ .slot = 33, .callsign = "KWMU 90.7", .name = "St. Louis Public Radio KWMU", .country = "US", .lang = "en", .hash = "hash_kwmu", .url = "https://stlpr.streamguys1.com/stlpr1" },
    .{ .slot = 34, .callsign = "KUT 90.5", .name = "KUT 90.5 Austin", .country = "US", .lang = "en", .hash = "hash_kut", .url = "https://kut1.streamguys1.com/kut1" },
    .{ .slot = 35, .callsign = "NHPR News", .name = "New Hampshire Public Radio NHPR", .country = "US", .lang = "en", .hash = "hash_nhpr", .url = "https://nhpr.streamguys1.com/nhpr1" },
    .{ .slot = 36, .callsign = "Maine Public", .name = "Maine Public Radio", .country = "US", .lang = "en", .hash = "hash_maine", .url = "https://mpbn.streamguys1.com/mpbn" },
    .{ .slot = 37, .callsign = "Boise State", .name = "Boise State Public Radio", .country = "US", .lang = "en", .hash = "hash_boise", .url = "https://bsu.streamguys1.com/kbsu" },
    .{ .slot = 38, .callsign = "Wyoming PR", .name = "Wyoming Public Radio", .country = "US", .lang = "en", .hash = "hash_wpr_wy", .url = "https://wpr.streamguys1.com/wpr" },
    .{ .slot = 39, .callsign = "KUFM 89.1", .name = "Montana Public Radio KUFM", .country = "US", .lang = "en", .hash = "hash_kufm", .url = "https://kufm.streamguys1.com/kufm" },
    .{ .slot = 40, .callsign = "KSKA 91.1", .name = "Alaska Public Media KSKA", .country = "US", .lang = "en", .hash = "hash_kska", .url = "https://kska.streamguys1.com/kska" },
    .{ .slot = 41, .callsign = "HPR Hawaii", .name = "Hawaii Public Radio HPR", .country = "US", .lang = "en", .hash = "hash_hpr", .url = "https://hpr.streamguys1.com/hpr1" },
    .{ .slot = 42, .callsign = "KPCC 89.3", .name = "KPCC 89.3 Pasadena", .country = "US", .lang = "en", .hash = "hash_kpcc", .url = "https://live.scpr.org/kpcclive" },
    .{ .slot = 43, .callsign = "KALW 91.7", .name = "KALW 91.7 San Francisco", .country = "US", .lang = "en", .hash = "hash_kalw", .url = "https://stream.kalw.org/kalw" },
    .{ .slot = 44, .callsign = "KNPR 88.9", .name = "Nevada Public Radio KNPR", .country = "US", .lang = "en", .hash = "hash_knpr", .url = "https://knpr.streamguys1.com/knpr" },
    .{ .slot = 45, .callsign = "KUAZ 89.1", .name = "Arizona Public Media KUAZ", .country = "US", .lang = "en", .hash = "hash_kuaz", .url = "https://kuaz.streamguys1.com/kuaz" },
    .{ .slot = 46, .callsign = "KUNM 89.9", .name = "New Mexico Public Radio KUNM", .country = "US", .lang = "en", .hash = "hash_kunm", .url = "https://kunm.streamguys1.com/kunm" },
    .{ .slot = 47, .callsign = "UPR Utah", .name = "Utah Public Radio UPR", .country = "US", .lang = "en", .hash = "hash_upr", .url = "https://upr.streamguys1.com/upr" },
    .{ .slot = 48, .callsign = "NPM Omaha", .name = "Nebraska Public Media", .country = "US", .lang = "en", .hash = "hash_npm", .url = "https://net.streamguys1.com/net" },
    .{ .slot = 49, .callsign = "SDPB Radio", .name = "South Dakota Public Broadcasting", .country = "US", .lang = "en", .hash = "hash_sdpb", .url = "https://sdpb.streamguys1.com/sdpb" },
    .{ .slot = 50, .callsign = "NDPR Prairie", .name = "North Dakota Public Radio Prairie", .country = "US", .lang = "en", .hash = "hash_ndpr", .url = "https://prairie.streamguys1.com/prairie" },
    .{ .slot = 51, .callsign = "KPR Kansas", .name = "Kansas Public Radio KPR", .country = "US", .lang = "en", .hash = "hash_kpr", .url = "https://kpr.streamguys1.com/kpr1" },
    .{ .slot = 52, .callsign = "KGOU 106.3", .name = "Oklahoma Public Media KGOU", .country = "US", .lang = "en", .hash = "hash_kgou", .url = "https://kgou.streamguys1.com/kgou" },
    .{ .slot = 53, .callsign = "KUAR 89.1", .name = "Arkansas Public Media KUAR", .country = "US", .lang = "en", .hash = "hash_kuar", .url = "https://kuar.streamguys1.com/kuar" },
    .{ .slot = 54, .callsign = "WRKF 89.3", .name = "Louisiana Public Broadcasting WRKF", .country = "US", .lang = "en", .hash = "hash_wrkf", .url = "https://wrkf.streamguys1.com/wrkf" },
    .{ .slot = 55, .callsign = "MPB News", .name = "Mississippi Public Broadcasting MPB", .country = "US", .lang = "en", .hash = "hash_mpb", .url = "https://mpb.streamguys1.com/mpb" },
    .{ .slot = 56, .callsign = "APR Alabama", .name = "Alabama Public Radio APR", .country = "US", .lang = "en", .hash = "hash_apr", .url = "https://apr.streamguys1.com/apr" },
    .{ .slot = 57, .callsign = "WPLN 90.3", .name = "Tennessee Public Radio WPLN", .country = "US", .lang = "en", .hash = "hash_wpln", .url = "https://wpln.streamguys1.com/wpln" },
    .{ .slot = 58, .callsign = "WFPL 89.3", .name = "Kentucky Public Radio WFPL", .country = "US", .lang = "en", .hash = "hash_wfpl", .url = "https://lpm.streamguys1.com/wfpl" },
    .{ .slot = 59, .callsign = "WFIU 103.7", .name = "Indiana Public Media WFIU", .country = "US", .lang = "en", .hash = "hash_wfiu", .url = "https://wfiu.streamguys1.com/wfiu" },
    .{ .slot = 60, .callsign = "WOSU 89.7", .name = "Ohio Public Radio WOSU", .country = "US", .lang = "en", .hash = "hash_wosu", .url = "https://wosu.streamguys1.com/wosu" },
    .{ .slot = 61, .callsign = "WVPB News", .name = "West Virginia Public Broadcasting", .country = "US", .lang = "en", .hash = "hash_wvpb", .url = "https://wvpb.streamguys1.com/wvpb" },
    .{ .slot = 62, .callsign = "WNPR 90.5", .name = "Connecticut Public Radio WNPR", .country = "US", .lang = "en", .hash = "hash_wnpr", .url = "https://wnpr.streamguys1.com/wnpr" },
    .{ .slot = 63, .callsign = "RIPBS 88.1", .name = "Rhode Island PBS Radio", .country = "US", .lang = "en", .hash = "hash_ripbs", .url = "https://ripbs.streamguys1.com/ripbs" },
};

pub fn main() !void {
    std.debug.print("FieldVox Station Binary Blob Generator (Zig 0.16)\n", .{});

    const station_count: u32 = @intCast(STATIONS.len);

    var records: [STATIONS.len]StationRecord = undefined;
    for (STATIONS, 0..) |s, i| {
        records[i] = StationRecord{
            .slot_id = s.slot,
            .callsign = padString(32, s.callsign),
            .name = padString(64, s.name),
            .country = padString(4, s.country),
            .lang = padString(4, s.lang),
            .url_hash = padString(32, s.hash),
            .url = padString(160, s.url),
        };
    }

    const records_slice = std.mem.sliceAsBytes(&records);

    // CRC32 computation
    var crc: u32 = 0xFFFFFFFF;
    for (records_slice) |byte| {
        crc ^= byte;
        for (0..8) |_| {
            const mask: u32 = if ((crc & 1) != 0) 0xEDB88320 else 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    const checksum = ~crc;

    const header = BlobHeader{
        .magic = 0x50425646, // "FVBP" in little endian
        .version = 1,
        .station_count = station_count,
        .checksum_crc32 = checksum,
    };
    const header_slice = std.mem.asBytes(&header);

    // 1. Resolve paths (support running from root or source/ directory)
    var bin_path: [:0]const u8 = "android/app/src/main/assets/fieldvox_station_plan.bin";
    var h_path: [:0]const u8 = "android/app/src/main/cpp/fv_station_blob_data.h";
    var assets_dir: [:0]const u8 = "android/app/src/main/assets";

    if (std.c.access(assets_dir.ptr, 0) != 0) {
        if (std.c.access("../android/app/src/main/assets", 0) == 0) {
            bin_path = "../android/app/src/main/assets/fieldvox_station_plan.bin";
            h_path = "../android/app/src/main/cpp/fv_station_blob_data.h";
            assets_dir = "../android/app/src/main/assets";
        }
    }

    _ = std.c.mkdir(assets_dir.ptr, 0o755);

    const maybe_bin = std.c.fopen(bin_path.ptr, "wb");
    if (maybe_bin == null) return error.BinOpenFailed;
    const bin_file = maybe_bin.?;
    defer _ = std.c.fclose(bin_file);

    _ = std.c.fwrite(header_slice.ptr, 1, header_slice.len, bin_file);
    _ = std.c.fwrite(records_slice.ptr, 1, records_slice.len, bin_file);
    std.debug.print("Wrote binary blob: {s} ({d} bytes)\n", .{ bin_path, header_slice.len + records_slice.len });

    // 2. Write C header with packed bytes for direct static zero-IO embedding in native C
    const maybe_h = std.c.fopen(h_path.ptr, "wb");
    if (maybe_h == null) return error.HeaderOpenFailed;
    const h_file = maybe_h.?;
    defer _ = std.c.fclose(h_file);

    var buf: [512]u8 = undefined;

    writeStr(h_file, "/* Auto-generated by station_blob_gen.zig - DO NOT EDIT MANUALLY */\n");
    writeStr(h_file, "#ifndef FV_STATION_BLOB_DATA_H\n#define FV_STATION_BLOB_DATA_H\n\n");
    writeStr(h_file, "#include <stdint.h>\n\n");
    writeStr(h_file, "#define FV_STATION_BLOB_MAGIC 0x50425646U\n");
    writeStr(h_file, "#define FV_STATION_BLOB_VERSION 1U\n");

    const line_count = try std.fmt.bufPrint(&buf, "#define FV_STATION_BLOB_COUNT {d}U\n", .{station_count});
    writeStr(h_file, line_count);

    const line_crc = try std.fmt.bufPrint(&buf, "#define FV_STATION_BLOB_CRC32 0x{X:0>8}U\n\n", .{checksum});
    writeStr(h_file, line_crc);

    writeStr(h_file, "typedef struct __attribute__((packed)) FvBlobStationEntry {\n");
    writeStr(h_file, "    uint32_t slot_id;\n");
    writeStr(h_file, "    char callsign[32];\n");
    writeStr(h_file, "    char name[64];\n");
    writeStr(h_file, "    char country[4];\n");
    writeStr(h_file, "    char lang[4];\n");
    writeStr(h_file, "    char url_hash[32];\n");
    writeStr(h_file, "    char url[160];\n");
    writeStr(h_file, "} FvBlobStationEntry;\n\n");

    writeStr(h_file, "static const uint8_t FV_STATION_PLAN_BINARY_DATA[] = {\n");

    var col: usize = 0;
    for (header_slice) |b| {
        const item = try std.fmt.bufPrint(&buf, "0x{X:0>2},", .{b});
        writeStr(h_file, item);
        col += 1;
        if (col % 16 == 0) writeStr(h_file, "\n");
    }
    for (records_slice) |b| {
        const item = try std.fmt.bufPrint(&buf, "0x{X:0>2},", .{b});
        writeStr(h_file, item);
        col += 1;
        if (col % 16 == 0) writeStr(h_file, "\n");
    }
    writeStr(h_file, "\n};\n\n#endif /* FV_STATION_BLOB_DATA_H */\n");

    std.debug.print("Wrote C packed blob header: {s}\n", .{h_path});
    std.debug.print("Success: Generated binary blob and C headers with {d} stations.\n", .{station_count});
}
