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


static long Write(const char *text, unsigned long length)
{
    while (length) {
        long count = Call(64, 1, (long)text, (long)length, 0, 0, 0);
        if (count == -4) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        text += count;
        length -= (unsigned long)count;
    }
    return 0;
}


static long Drain(void)
{
    long result;

    do {
        result = Call(29, 1, 0x5409, 1, 0, 0, 0);
    } while (result == -4);
    return result;
}


/* 读取内核接口，避免依赖动态链接器及外部工具 */
static long Read(const char *path, char *buffer, unsigned long size)
{
    long fd = Call(56, -100, (long)path, 0, 0, 0, 0);
    long count;

    if (fd < 0) {
        return fd;
    }
    do {
        count = Call(63, fd, (long)buffer, (long)size, 0, 0, 0);
    } while (count == -4);
    Call(57, fd, 0, 0, 0, 0, 0);
    return count;
}


static void Result(long result)
{
    char buffer[24];
    unsigned long position = sizeof(buffer);
    unsigned long magnitude = result < 0 ? (unsigned long)-result :
        (unsigned long)result;

    buffer[--position] = '\n';
    do {
        buffer[--position] = '0' + magnitude % 10;
        magnitude /= 10;
    } while (magnitude);
    if (result < 0) {
        buffer[--position] = '-';
    }
    Write(buffer + position, sizeof(buffer) - position);
}


__attribute__((noreturn)) static void Fail(void)
{
    static const char failed[] = "\nSHENG_GKI_612_IPC_SMOKE_FAILED\n";
    static const long wait[2] = {1, 0};

    Write(failed, sizeof(failed) - 1);
    Drain();
    /* 保留失败现场，由虚拟机外部超时终止 */
    for (;;) {
        Call(101, (long)wait, 0, 0, 0, 0, 0);
    }
}


/* 仅验证签名模块加载及 Binder 版本查询 */
__attribute__((noreturn)) void _start(void)
{
    static const char generic[] = "SHENG_GKI_612_SMOKE_OK\n";
    static const char signature[] = "SHENG_GKI_612_SIG_ENFORCED\n";
    static const char load[] = "SHENG_GKI_612_IPC_LOAD_OK\n";
    static const char binder[] = "SHENG_GKI_612_BINDER_VERSION_8\n";
    static const char done[] = "SHENG_GKI_612_IPC_SMOKE_OK\n";
    static const char loading[] = "SHENG_GKI_612_FINIT_RESULT=";
    char buffer[512];
    long count;
    long fd;
    long result;
    int protocol = 0;

    if (Call(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0) != 0) {
        Fail();
    }
    count = Read("/proc/version", buffer, sizeof(buffer));
    if (count <= 0 || Write(buffer, (unsigned long)count) != 0 ||
        Write(generic, sizeof(generic) - 1) != 0 || Drain() != 0) {
        Fail();
    }
    if (Call(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0) != 0 ||
        Call(40, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0, 0) != 0) {
        Fail();
    }
    count = Read("/sys/module/module/parameters/sig_enforce", buffer, sizeof(buffer));
    if (count != 2 || buffer[0] != 'Y' || buffer[1] != '\n' ||
        Write(signature, sizeof(signature) - 1) != 0 || Drain() != 0) {
        Fail();
    }
    count = Read("/proc/sys/kernel/tainted", buffer, sizeof(buffer));
    if (count != 2 || buffer[0] != '0' || buffer[1] != '\n') {
        Fail();
    }

    fd = Call(56, -100, (long)"/probe/sheng_hmos_ipc.ko", 0, 0, 0, 0);
    if (fd < 0) {
        Fail();
    }
    /* 保留版本与签名校验，禁止忽略模块加载标志 */
    result = Call(273, fd, (long)"", 0, 0, 0, 0);
    Call(57, fd, 0, 0, 0, 0, 0);
    Write(loading, sizeof(loading) - 1);
    Result(result);
    if (result != 0 || Write(load, sizeof(load) - 1) != 0 || Drain() != 0) {
        Fail();
    }
    fd = Call(56, -100, (long)"/dev/hmos_binder", 2, 0, 0, 0);
    if (fd < 0) {
        Fail();
    }
    result = Call(29, fd, 0xc0046209, (long)&protocol, 0, 0, 0);
    Call(57, fd, 0, 0, 0, 0, 0);
    if (result != 0 || protocol != 8 ||
        Write(binder, sizeof(binder) - 1) != 0 || Drain() != 0) {
        Fail();
    }
    count = Read("/proc/sys/kernel/tainted", buffer, sizeof(buffer));
    if (count != 5 || buffer[0] != '4' || buffer[1] != '0' ||
        buffer[2] != '9' || buffer[3] != '6' || buffer[4] != '\n') {
        Fail();
    }
    if (Write(done, sizeof(done) - 1) != 0 || Drain() != 0) {
        Fail();
    }
    Call(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    Fail();
}
