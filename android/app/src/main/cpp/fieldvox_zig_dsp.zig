const std = @import("std");

// FIELDVOX PARANORMAL NATIVE ZIG DSP & TELEMETRY MODULE
// Zero Java. Pure Zig compiled directly into native relocatable machine code.

pub const ColdSpotTracker = extern struct {
    baseline: f32,
    last_pressure: f32,
    delta: f32,
    cold_spot_active: bool,
    spike_counter: u32,
};

var global_tracker: ColdSpotTracker = .{
    .baseline = 0.0,
    .last_pressure = 0.0,
    .delta = 0.0,
    .cold_spot_active = false,
    .spike_counter = 0,
};

/// Updates barometric pressure telemetry and detects sudden drops indicative of cold spots.
/// Returns 1 if cold spot drop detected (delta <= -0.10 hPa), 0 otherwise.
pub export fn zig_fv_update_barometer(hpa: f32, out_delta: ?*f32) callconv(.c) i32 {
    if (!std.math.isFinite(hpa) or hpa <= 0.0) {
        if (out_delta) |ptr| ptr.* = 0.0;
        return 0;
    }

    if (global_tracker.baseline <= 0.0) {
        global_tracker.baseline = hpa;
        global_tracker.last_pressure = hpa;
    }

    // Slow moving baseline adaptation
    global_tracker.baseline = (global_tracker.baseline * 0.992) + (hpa * 0.008);
    const delta = hpa - global_tracker.baseline;
    global_tracker.last_pressure = hpa;
    global_tracker.delta = delta;

    if (out_delta) |ptr| {
        ptr.* = delta;
    }

    // Paranormal cold spot drop trigger: rapid pressure drop <= -0.10 hPa
    if (delta <= -0.10) {
        global_tracker.cold_spot_active = true;
        global_tracker.spike_counter += 1;
        return 1;
    } else {
        global_tracker.cold_spot_active = false;
        return 0;
    }
}

pub export fn zig_fv_get_barometer_baseline() callconv(.c) f32 {
    return global_tracker.baseline;
}

pub export fn zig_fv_reset_barometer_baseline(hpa: f32) callconv(.c) void {
    if (!std.math.isFinite(hpa) or hpa <= 0.0) return;
    global_tracker.baseline = hpa;
    global_tracker.last_pressure = hpa;
    global_tracker.delta = 0.0;
    global_tracker.cold_spot_active = false;
}

// 3D Binaural Spatial Audio Panning (Equal-Power Law)
// In-place or output calculation: Left = cos(theta), Right = sin(theta)
pub export fn zig_fv_calc_spatial_pan_gains(azimuth_deg: f32, out_left_opt: ?*f32, out_right_opt: ?*f32) callconv(.c) void {
    const out_left = out_left_opt orelse return;
    const out_right = out_right_opt orelse return;

    if (!std.math.isFinite(azimuth_deg)) {
        out_left.* = 0.70710678;
        out_right.* = 0.70710678;
        return;
    }

    // Normalize azimuth [0, 360) -> [-1.0, 1.0] where 0.0 is center
    var norm_pan = azimuth_deg;
    while (norm_pan > 180.0) norm_pan -= 360.0;
    while (norm_pan < -180.0) norm_pan += 360.0;

    // Convert -180..+180 to -1.0..+1.0
    const pan_clamped = std.math.clamp(norm_pan / 90.0, -1.0, 1.0);
    const theta = (pan_clamped + 1.0) * (std.math.pi / 4.0);

    out_left.* = @cos(theta);
    out_right.* = @sin(theta);
}

// Acoustic REM-Pod Synthesizer: 4-Tier Stepped Tones (440, 880, 1320, 1760 Hz)
var rem_phase: f32 = 0.0;
var rem_pulse_counter: u32 = 0;

pub export fn zig_fv_rem_pod_generate(
    samples_opt: ?[*]i16,
    frame_count: usize,
    level: i32,
    sample_rate: u32,
) callconv(.c) void {
    const samples = samples_opt orelse return;
    if (level <= 0 or sample_rate == 0 or frame_count == 0) return;

    const freqs = [4]f32{ 440.0, 880.0, 1320.0, 1760.0 };
    const tier: usize = @intCast(std.math.clamp(level - 1, 0, 3));
    const target_freq = freqs[tier];
    const phase_inc = (target_freq * 2.0 * std.math.pi) / @as(f32, @floatFromInt(sample_rate));

    // Pulse rate increases with tier: tier 0: 6 Hz, tier 1: 10 Hz, tier 2: 16 Hz, tier 3: solid
    const pulse_rate: u32 = switch (tier) {
        0 => 6,
        1 => 10,
        2 => 16,
        else => 0, // continuous tone
    };

    const pulse_period: u32 = if (pulse_rate > 0) sample_rate / pulse_rate else 0;
    const pulse_on_len: u32 = if (pulse_period > 0) pulse_period / 2 else 0;

    var i: usize = 0;
    while (i < frame_count) : (i += 1) {
        var is_on = true;
        if (pulse_period > 0) {
            const phase_in_period = rem_pulse_counter % pulse_period;
            is_on = phase_in_period < pulse_on_len;
            rem_pulse_counter +%= 1;
        }

        if (is_on) {
            const s = @sin(rem_phase);
            const tone_amp: f32 = 4500.0 + @as(f32, @floatFromInt(tier)) * 2000.0;
            const tone_sample: i32 = @intFromFloat(s * tone_amp);
            const current: i32 = samples[i];
            const mixed: i32 = std.math.clamp(current + tone_sample, -32767, 32767);
            samples[i] = @intCast(mixed);
        }

        rem_phase += phase_inc;
        if (rem_phase >= 2.0 * std.math.pi) {
            rem_phase -= 2.0 * std.math.pi;
        }
    }
}

// Binary Station Blob Validator in Zig
pub export fn zig_fv_verify_blob_checksum(blob_bytes_opt: ?[*]const u8, blob_size: usize) callconv(.c) i32 {
    const blob_bytes = blob_bytes_opt orelse return 0;
    if (blob_size < 16) return 0;

    const magic = @as(u32, blob_bytes[0]) |
        (@as(u32, blob_bytes[1]) << 8) |
        (@as(u32, blob_bytes[2]) << 16) |
        (@as(u32, blob_bytes[3]) << 24);

    if (magic != 0x50425646) return 0; // "FVBP" (FieldVox Binary Plan)

    const count = @as(u32, blob_bytes[8]) |
        (@as(u32, blob_bytes[9]) << 8) |
        (@as(u32, blob_bytes[10]) << 16) |
        (@as(u32, blob_bytes[11]) << 24);

    if (count == 0 or count > 4096) return 0;

    const expected_crc = @as(u32, blob_bytes[12]) |
        (@as(u32, blob_bytes[13]) << 8) |
        (@as(u32, blob_bytes[14]) << 16) |
        (@as(u32, blob_bytes[15]) << 24);

    const record_bytes = blob_bytes + 16;
    const record_len: usize = @as(usize, count) * 300;
    if (16 + record_len > blob_size) return 0;

    var crc: u32 = ~@as(u32, 0);
    var idx: usize = 0;
    while (idx < record_len) : (idx += 1) {
        crc ^= record_bytes[idx];
        var j: usize = 0;
        while (j < 8) : (j += 1) {
            const mask: u32 = if ((crc & 1) != 0) 0xEDB88320 else 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    crc = ~crc;

    return if (crc == expected_crc) 1 else 0;
}

test "barometer initializes from the first real reading and detects a drop" {
    global_tracker.baseline = 0.0;
    var delta: f32 = 99.0;
    try std.testing.expectEqual(@as(i32, 0), zig_fv_update_barometer(1005.0, &delta));
    try std.testing.expectApproxEqAbs(@as(f32, 0.0), delta, 0.001);
    try std.testing.expectEqual(@as(i32, 1), zig_fv_update_barometer(1004.7, &delta));
    try std.testing.expect(delta < -0.29);
    try std.testing.expectEqual(@as(i32, 0), zig_fv_update_barometer(std.math.nan(f32), &delta));
    try std.testing.expectEqual(@as(i32, 0), zig_fv_update_barometer(-50.0, &delta));
}

test "spatial pan uses equal-power center gains" {
    var left: f32 = 0.0;
    var right: f32 = 0.0;
    zig_fv_calc_spatial_pan_gains(0.0, &left, &right);
    try std.testing.expectApproxEqAbs(@as(f32, 0.70710677), left, 0.0001);
    try std.testing.expectApproxEqAbs(left, right, 0.0001);

    // Non-finite input falls back safely without hanging
    zig_fv_calc_spatial_pan_gains(std.math.nan(f32), &left, &right);
    try std.testing.expectApproxEqAbs(@as(f32, 0.70710677), left, 0.0001);
    zig_fv_calc_spatial_pan_gains(std.math.inf(f32), &left, &right);
    try std.testing.expectApproxEqAbs(@as(f32, 0.70710677), left, 0.0001);

    // Null outputs are ignored without crashing
    zig_fv_calc_spatial_pan_gains(90.0, null, null);
}

test "blob validator rejects truncated and invalid data" {
    try std.testing.expectEqual(@as(i32, 0), zig_fv_verify_blob_checksum(null, 100));
    const too_short = [8]u8{ 0, 0, 0, 0, 0, 0, 0, 0 };
    try std.testing.expectEqual(@as(i32, 0), zig_fv_verify_blob_checksum(&too_short, too_short.len));
    var header = [16]u8{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    header[0] = 0x46;
    header[1] = 0x56;
    header[2] = 0x42;
    header[3] = 0x50;
    header[8] = 64;
    try std.testing.expectEqual(@as(i32, 0), zig_fv_verify_blob_checksum(&header, header.len));

    // Valid blob synthesis: 1 slot (300 zero bytes) with matching CRC32
    var valid_blob: [16 + 300]u8 = [_]u8{0} ** (16 + 300);
    valid_blob[0] = 0x46; // 'F'
    valid_blob[1] = 0x56; // 'V'
    valid_blob[2] = 0x42; // 'B'
    valid_blob[3] = 0x50; // 'P'
    valid_blob[4] = 1; // version 1
    valid_blob[8] = 1; // count = 1

    var crc: u32 = ~@as(u32, 0);
    for (valid_blob[16..]) |b| {
        crc ^= b;
        for (0..8) |_| {
            const mask: u32 = if ((crc & 1) != 0) 0xEDB88320 else 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    crc = ~crc;
    valid_blob[12] = @truncate(crc);
    valid_blob[13] = @truncate(crc >> 8);
    valid_blob[14] = @truncate(crc >> 16);
    valid_blob[15] = @truncate(crc >> 24);

    try std.testing.expectEqual(@as(i32, 1), zig_fv_verify_blob_checksum(&valid_blob, valid_blob.len));
    valid_blob[20] ^= 0xFF; // corrupt one payload byte
    try std.testing.expectEqual(@as(i32, 0), zig_fv_verify_blob_checksum(&valid_blob, valid_blob.len));
}
