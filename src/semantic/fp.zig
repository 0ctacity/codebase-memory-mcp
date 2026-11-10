//! fp.zig — floating-point contraction that matches the C build.
//!
//! The C sources were compiled by clang with its default -ffp-contract=on:
//! where the target CPU has a fused multiply-add instruction, `a * b + c`
//! written in one expression becomes a single FMA with one rounding. Zig
//! never fuses on its own, so a port that writes `a * b + c` rounds twice and
//! produces scores that differ in the last bit. Those differences move
//! similarity scores across edge thresholds.
//!
//! mulAdd fuses exactly when clang would: on targets with FMA. Use it for
//! every expression the C code wrote as a product plus (or minus) a term in
//! one expression, including `x += a * b`.

const std = @import("std");
const builtin = @import("builtin");

/// True when the target has a fused multiply-add instruction, which is when
/// clang lowers llvm.fmuladd to FMA.
pub const has_fma = switch (builtin.cpu.arch) {
    .x86_64, .x86 => std.Target.x86.featureSetHas(builtin.cpu.features, .fma),
    .aarch64, .aarch64_be => true,
    else => false,
};

/// `a * b + c` the way clang compiles that C expression.
pub inline fn mulAdd(comptime T: type, a: T, b: T, c: T) T {
    return if (has_fma) @mulAdd(T, a, b, c) else a * b + c;
}
