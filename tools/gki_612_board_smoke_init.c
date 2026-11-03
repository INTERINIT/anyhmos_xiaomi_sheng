/* SPDX-License-Identifier: GPL-2.0 */

enum {
    SYS_OPENAT = 56,
    SYS_CLOSE = 57,
    SYS_READ = 63,
    SYS_WRITE = 64,
    SYS_NANOSLEEP = 101,
    SYS_MOUNT = 40,
    SYS_REBOOT = 142,
    AT_FDCWD = -100,
    OPEN_NONBLOCK = 04000,
    OPEN_NOCTTY = 0400,
};

struct TimeSpec {
    long seconds;
    long nanoseconds;
};


static long Call(long number, long first, long second, long third,
    long fourth, long fifth, long sixth)
{
    register long x0 __asm__("x0") = first;
    register long x1 __asm__("x1") = second;
    register long x2 __asm__("x2") = third;
    register long x3 __asm__("x3") = fourth;
    register long x4 __asm__("x4") = fifth;
    register long x5 __asm__("x5") = sixth;
    register long x8 __asm__("x8") = number;

    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
        "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}


static void Write(long fd, const char *data, unsigned long length)
{
    while (fd >= 0 && length) {
        long count = Call(SYS_WRITE, fd, (long)data, (long)length, 0, 0, 0);

        if (count <= 0) {
            return;
        }
        data += count;
        length -= (unsigned long)count;
    }
}


static long Open(const char *path, long flags)
{
    return Call(SYS_OPENAT, AT_FDCWD, (long)path, flags, 0, 0, 0);
}


static void Close(long fd)
{
    if (fd >= 0) {
        Call(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
}


static void CopyFile(long output, const char *path)
{
    char data[1024];
    long input = Open(path, OPEN_NONBLOCK);
    long count = input >= 0 ? Call(SYS_READ, input, (long)data,
        sizeof(data), 0, 0, 0) : -1;

    if (count > 0) {
        Write(output, data, (unsigned long)count);
    }
    Close(input);
}


/* 仅挂载内存及内核接口文件系统 */
static void MountInterfaces(void)
{
    Call(SYS_MOUNT, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
    Call(SYS_MOUNT, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
    Call(SYS_MOUNT, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0, 0);
}


/* 在有限窗口内向 USB 串口转发内核日志 */
__attribute__((noreturn)) void _start(void)
{
    static const char started[] = "SHENG_GKI_612_BOARD_INIT\n";
    static const char restarting[] = "SHENG_GKI_612_BOARD_RESTART\n";
    static const char restartFailed[] = "SHENG_GKI_612_BOARD_RESTART_FAILED\n";
    const struct TimeSpec delay = { 0, 100000000 };
    char data[2048];
    long serial = -1;
    long kernelLog;
    long writer;
    unsigned int tick;

    MountInterfaces();
    writer = Open("/dev/kmsg", 1 | OPEN_NONBLOCK);
    Write(writer, started, sizeof(started) - 1);
    Close(writer);
    kernelLog = Open("/dev/kmsg", OPEN_NONBLOCK);

    for (tick = 0; tick < 300; ++tick) {
        if (serial < 0) {
            serial = Open("/dev/ttyGS0", 2 | OPEN_NONBLOCK | OPEN_NOCTTY);
            if (serial >= 0) {
                Write(serial, started, sizeof(started) - 1);
                CopyFile(serial, "/proc/version");
                CopyFile(serial, "/proc/cmdline");
                CopyFile(serial, "/proc/self/mounts");
            }
        }
        if (serial >= 0 && kernelLog >= 0) {
            unsigned int record;

            for (record = 0; record < 20; ++record) {
                long count = Call(SYS_READ, kernelLog, (long)data,
                    sizeof(data), 0, 0, 0);

                if (count <= 0) {
                    break;
                }
                Write(serial, data, (unsigned long)count);
            }
        }
        Call(SYS_NANOSLEEP, (long)&delay, 0, 0, 0, 0, 0);
    }

    Write(serial, restarting, sizeof(restarting) - 1);
    Close(kernelLog);
    Call(SYS_REBOOT, 0xfee1dead, 672274793, 0x01234567, 0, 0, 0);
    Write(serial, restartFailed, sizeof(restartFailed) - 1);
    for (;;) {
        Call(SYS_NANOSLEEP, (long)&delay, 0, 0, 0, 0, 0);
    }
}
