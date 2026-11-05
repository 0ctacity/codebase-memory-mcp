//! archive.zig — in-memory extraction of the release binary from a .tar.gz
//! or .zip download (the `update` command). Implements
//! cbm_extract_binary_from_targz and cbm_extract_binary_from_zip from
//! src/cli/cli.h.
//!
//! Inflation uses std.compress.flate rather than zlib, so the Zig library
//! needs no zlib header on any target.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const flate = std.compress.flate;
const gpa = std.heap.c_allocator;

/// Decompression cap: 500 MB.
const DECOMPRESS_MAX_BYTES: usize = 500 * 1024 * 1024;

const TAR_BLOCK_SIZE: usize = 512;
const TAR_NAME_LEN = 100; // filename field: bytes 0-99
const TAR_SIZE_OFFSET = 124; // octal size field offset
const TAR_SIZE_LEN = 12; // octal size field: bytes 124-135
const TAR_TYPE_OFFSET = 156; // type flag byte
const BINARY_NAME = "codebase-memory-mcp";

/// Inflate input (gzip or raw deflate) into a gpa-owned buffer of at most
/// limit bytes. Null on corrupt or truncated input, or when the output would
/// exceed limit.
fn inflate(input: []const u8, container: flate.Container, limit: usize) ?[]u8 {
    var in: std.Io.Reader = .fixed(input);
    const window = gpa.alloc(u8, flate.max_window_len) catch return null;
    defer gpa.free(window);
    var dec: flate.Decompress = .init(&in, container, window);
    return dec.reader.allocRemaining(gpa, .limited(limit)) catch null;
}

/// A malloc'd copy of bytes for a C caller that frees with free().
fn mallocCopy(bytes: []const u8, out_len: *c_int) ?[*]u8 {
    const out = interop.mallocN(u8, @max(bytes.len, 1));
    if (out == null) return null;
    @memcpy(out[0..bytes.len], bytes);
    out_len.* = @intCast(bytes.len);
    return out;
}

fn basename(name: []const u8) []const u8 {
    return if (std.mem.lastIndexOfScalar(u8, name, '/')) |i| name[i + 1 ..] else name;
}

pub export fn cbm_extract_binary_from_targz(data: [*c]const u8, data_len: c_int, out_len: [*c]c_int) [*c]u8 {
    if (data == null or data_len <= 0 or out_len == null) return null;
    const archive = inflate(data[0..@intCast(data_len)], .gzip, DECOMPRESS_MAX_BYTES) orelse return null;
    defer gpa.free(archive);

    // Parse tar: find the entry whose basename starts with "codebase-memory-mcp".
    var pos: usize = 0;
    while (pos + TAR_BLOCK_SIZE <= archive.len) {
        const hdr = archive[pos..][0..TAR_BLOCK_SIZE];
        // An all-zero block marks the end of the archive.
        if (std.mem.allEqual(u8, hdr, 0)) break;

        const name = std.mem.sliceTo(hdr[0..TAR_NAME_LEN], 0);
        const size_field = std.mem.sliceTo(hdr[TAR_SIZE_OFFSET..][0..TAR_SIZE_LEN], 0);
        var size_z: [TAR_SIZE_LEN + 1]u8 = undefined;
        @memcpy(size_z[0..size_field.len], size_field);
        size_z[size_field.len] = 0;
        const file_size_l = c.strtol(&size_z, null, 8);
        if (file_size_l < 0) break;
        const file_size: usize = @intCast(file_size_l);
        const typeflag = hdr[TAR_TYPE_OFFSET];
        pos += TAR_BLOCK_SIZE;

        if ((typeflag == '0' or typeflag == 0) and std.mem.startsWith(u8, basename(name), BINARY_NAME)) {
            if (file_size > archive.len - pos) return null;
            return mallocCopy(archive[pos..][0..file_size], out_len);
        }
        const blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        pos +|= blocks * TAR_BLOCK_SIZE;
    }
    return null; // binary not found
}

// Zip local file header layout.
const ZIP_SIG = [4]u8{ 0x50, 0x4B, 0x03, 0x04 };
const ZIP_HDR_SZ = 30;
const ZIP_OFF_METHOD = 8;
const ZIP_OFF_COMP = 18;
const ZIP_OFF_UNCOMP = 22;
const ZIP_OFF_NAMELEN = 26;
const ZIP_OFF_EXTRALEN = 28;
const ZIP_STORED = 0;
const ZIP_DEFLATE = 8;
const ZIP_MAX_UNCOMP: usize = 500 * 1024 * 1024;
const ZIP_NAME_MAX = 511;

/// Decompress a single zip entry (stored or deflated) into a malloc'd buffer.
fn zipExtractEntry(file_data: []const u8, method: u16, uncomp_size: usize, out_len: *c_int) ?[*]u8 {
    switch (method) {
        ZIP_STORED => {
            if (file_data.len > ZIP_MAX_UNCOMP) return null;
            return mallocCopy(file_data, out_len);
        },
        ZIP_DEFLATE => {
            if (uncomp_size > ZIP_MAX_UNCOMP) return null;
            const out = inflate(file_data, .raw, uncomp_size) orelse return null;
            defer gpa.free(out);
            return mallocCopy(out, out_len);
        },
        else => return null, // unknown method
    }
}

pub export fn cbm_extract_binary_from_zip(data_c: [*c]const u8, data_len: c_int, out_len: [*c]c_int) [*c]u8 {
    if (data_c == null or data_len <= 0 or out_len == null) return null;
    out_len.* = 0;
    const data = data_c[0..@intCast(data_len)];

    var pos: usize = 0;
    while (pos + ZIP_HDR_SZ <= data.len) {
        const hdr = data[pos..][0..ZIP_HDR_SZ];
        if (!std.mem.eql(u8, hdr[0..4], &ZIP_SIG)) break;

        const method = std.mem.readInt(u16, hdr[ZIP_OFF_METHOD..][0..2], .little);
        const comp_size: usize = std.mem.readInt(u32, hdr[ZIP_OFF_COMP..][0..4], .little);
        const uncomp_size: usize = std.mem.readInt(u32, hdr[ZIP_OFF_UNCOMP..][0..4], .little);
        const name_len: usize = std.mem.readInt(u16, hdr[ZIP_OFF_NAMELEN..][0..2], .little);
        const extra_len: usize = std.mem.readInt(u16, hdr[ZIP_OFF_EXTRALEN..][0..2], .little);

        const header_end = pos + ZIP_HDR_SZ + name_len + extra_len;
        if (header_end > data.len or comp_size > data.len - header_end) break;

        // The name, truncated like the C fixed buffer and cut at an embedded NUL.
        const name = std.mem.sliceTo(data[pos + ZIP_HDR_SZ ..][0..@min(name_len, ZIP_NAME_MAX)], 0);
        const next = header_end + comp_size;
        if (std.mem.indexOf(u8, name, "..") != null) {
            pos = next;
            continue;
        }
        const base = basename(name);
        if (std.mem.eql(u8, base, BINARY_NAME) or std.mem.eql(u8, base, BINARY_NAME ++ ".exe")) {
            return zipExtractEntry(data[header_end..][0..comp_size], method, uncomp_size, out_len);
        }
        pos = next;
    }
    return null;
}
