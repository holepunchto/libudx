const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const mode = b.standardReleaseOptions();

    const udx = b.addStaticLibrary("udx", null);
    udx.setTarget(target);
    udx.setBuildMode(mode);
    udx.linkLibC();
    udx.linkSystemLibrary("libuv");
    udx.addSystemIncludeDir("/usr/include");
    udx.addCSourceFiles(&.{
        "src/cirbuf.c",
        "src/fifo.c",
        "src/udx.c",
        "src/utils.c",
    }, &.{
        "-Wall",
    });
    udx.install();
}
