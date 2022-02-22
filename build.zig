const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const mode = b.standardReleaseOptions();

    const udx = b.addStaticLibrary("udx", null);
    udx.setTarget(target);
    udx.setBuildMode(mode);
    udx.linkLibC();
    udx.linkSystemLibrary("libuv");

    udx.addCSourceFiles(&.{ "src/cirbuf.c", "src/fifo.c", "src/udx.c" }, &.{"-Wall"});

    if (target.isLinux() or target.isDarwin()) {
        udx.addCSourceFiles(&.{"src/io_posix.c"}, &.{"-Wall"});
    }

    udx.install();
}
