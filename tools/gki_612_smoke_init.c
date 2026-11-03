/* SPDX-License-Identifier: GPL-2.0 */

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


static void Write(const char *text, unsigned long length)
{
    while (length) {
        long count = Call(64, 1, (long)text, (long)length, 0, 0, 0);
        if (count <= 0) {
            return;
        }
        text += count;
        length -= (unsigned long)count;
    }
}


/* 读取真实内核版本后关闭虚拟机 */
__attribute__((noreturn)) void _start(void)
{
    static const char marker[] = "SHENG_GKI_612_SMOKE_OK\n";
    static const char failed[] = "SHENG_GKI_612_SMOKE_FAILED\n";
    char version[512];
    long mounted = Call(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
    long fd = mounted == 0 ? Call(56, -100, (long)"/proc/version", 0, 0, 0, 0) : -1;
    long count = fd >= 0 ? Call(63, fd, (long)version, sizeof(version), 0, 0, 0) : -1;

    if (count > 0) {
        Write(version, (unsigned long)count);
        Write(marker, sizeof(marker) - 1);
    } else {
        Write(failed, sizeof(failed) - 1);
    }
    Call(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    for (;;) {
        Call(93, count > 0 ? 0 : 1, 0, 0, 0, 0, 0);
    }
}
