// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/android/binder.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s errno=%d (%s)\n", \
        __LINE__, #x, errno, strerror(errno)); exit(1); \
} } while (0)

struct instruction {
    uint32_t code;
    pid_t sender_pid;
};

static void command(int fd, uint32_t code, const void *value, size_t size)
{
    unsigned char bytes[512];
    REQUIRE(size + sizeof(code) <= sizeof(bytes));
    memcpy(bytes, &code, sizeof(code));
    if (size) memcpy(bytes + sizeof(code), value, size);
    struct binder_write_read wr = {
        .write_size = sizeof(code) + size,
        .write_buffer = (uintptr_t)bytes,
    };
    REQUIRE(ioctl(fd, BINDER_WRITE_READ, &wr) == 0);
    REQUIRE(wr.write_consumed == wr.write_size);
}

static int open_binder(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    REQUIRE(fd >= 0);
    REQUIRE(mmap(NULL, 1024 * 1024, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED);
    return fd;
}

static uint32_t receive(int fd, struct binder_transaction_data *transaction)
{
    for (;;) {
        unsigned char bytes[1024];
        struct binder_write_read wr = {
            .read_size = sizeof(bytes),
            .read_buffer = (uintptr_t)bytes,
        };
        REQUIRE(ioctl(fd, BINDER_WRITE_READ, &wr) == 0);
        for (size_t offset = 0; offset + 4 <= wr.read_consumed;) {
            uint32_t code;
            memcpy(&code, bytes + offset, sizeof(code));
            offset += sizeof(code);
            REQUIRE(offset + _IOC_SIZE(code) <= wr.read_consumed);
            if (code == BR_TRANSACTION || code == BR_REPLY) {
                memcpy(transaction, bytes + offset, sizeof(*transaction));
                return code;
            }
            if (code == BR_INCREFS || code == BR_ACQUIRE) {
                struct binder_ptr_cookie value;
                memcpy(&value, bytes + offset, sizeof(value));
                command(fd, code == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE,
                    &value, sizeof(value));
            }
            REQUIRE(code != BR_DEAD_REPLY && code != BR_FAILED_REPLY && code != BR_ERROR);
            offset += _IOC_SIZE(code);
        }
    }
}

static void send_transaction(int fd, uint32_t code, bool oneway)
{
    struct binder_transaction_data transaction = {
        .target.handle = 0,
        .code = code,
        .flags = oneway ? TF_ONE_WAY : 0,
    };
    command(fd, BC_TRANSACTION, &transaction, sizeof(transaction));
}

static void wait_ok(pid_t child)
{
    int status;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void allow_receive(int fd, uint32_t code, pid_t sender_pid)
{
    struct instruction value = {code, sender_pid};
    REQUIRE(write(fd, &value, sizeof(value)) == sizeof(value));
}

static void wait_ack(int fd)
{
    char value;
    REQUIRE(read(fd, &value, 1) == 1 && value == 'A');
}

/* 仅使用独立测试节点，避免接管系统上下文管理器 */
static void verify_private_node(const char *path, const char *name)
{
    const char *base = strrchr(path, '/');
    struct stat state;
    REQUIRE(base && !strcmp(base + 1, name));
    REQUIRE(lstat(path, &state) == 0 && S_ISCHR(state.st_mode));
}

static void run_server(const char *path, int control, int ack, unsigned count)
{
    alarm(20);
    int fd = open_binder(path);
    REQUIRE(ioctl(fd, BINDER_SET_CONTEXT_MGR, 0) == 0);
    command(fd, BC_ENTER_LOOPER, NULL, 0);
    REQUIRE(write(ack, "A", 1) == 1);
    for (unsigned index = 0; index < count; ++index) {
        struct instruction expected;
        REQUIRE(read(control, &expected, sizeof(expected)) == sizeof(expected));
        struct binder_transaction_data transaction;
        REQUIRE(receive(fd, &transaction) == BR_TRANSACTION);
        REQUIRE(transaction.code == expected.code);
        REQUIRE(transaction.sender_pid == expected.sender_pid);
        binder_uintptr_t buffer = transaction.data.ptr.buffer;
        command(fd, BC_FREE_BUFFER, &buffer, sizeof(buffer));
        if (!(transaction.flags & TF_ONE_WAY)) {
            struct binder_transaction_data reply = {0};
            command(fd, BC_REPLY, &reply, sizeof(reply));
        }
        REQUIRE(write(ack, "A", 1) == 1);
    }
    close(fd);
}

static void *send_from_thread(void *value)
{
    int fd = *(int *)value;
    send_transaction(fd, 3, true);
    return NULL;
}

/* 发送线程退出后仍按进程标识解析身份 */
static void test_root_namespace(const char *path, bool enabled)
{
    int control[2], ack[2];
    REQUIRE(pipe(control) == 0 && pipe(ack) == 0);
    pid_t server = fork();
    REQUIRE(server >= 0);
    if (!server) {
        close(control[1]);
        close(ack[0]);
        run_server(path, control[0], ack[1], 4);
        _exit(0);
    }
    close(control[0]);
    close(ack[1]);
    wait_ack(ack[0]);
    int fd = open_binder(path);
    pid_t oneway_pid = enabled ? getpid() : 0;

    send_transaction(fd, 1, false);
    allow_receive(control[1], 1, getpid());
    struct binder_transaction_data reply;
    REQUIRE(receive(fd, &reply) == BR_REPLY && reply.sender_pid == 0);
    binder_uintptr_t buffer = reply.data.ptr.buffer;
    command(fd, BC_FREE_BUFFER, &buffer, sizeof(buffer));
    wait_ack(ack[0]);

    send_transaction(fd, 2, true);
    allow_receive(control[1], 2, oneway_pid);
    wait_ack(ack[0]);

    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, send_from_thread, &fd) == 0);
    REQUIRE(pthread_join(thread, NULL) == 0);
    allow_receive(control[1], 3, oneway_pid);
    wait_ack(ack[0]);

    /* 等待发送进程被回收后再消费排队事务 */
    pid_t sender = fork();
    REQUIRE(sender >= 0);
    if (!sender) {
        alarm(10);
        close(fd);
        int own_fd = open_binder(path);
        send_transaction(own_fd, 4, true);
        close(own_fd);
        _exit(0);
    }
    wait_ok(sender);
    allow_receive(control[1], 4, 0);
    wait_ack(ack[0]);
    close(fd);
    close(control[1]);
    close(ack[0]);
    wait_ok(server);
    puts("PASS sync, reply, live oneway, exited thread and reaped process");
}

/* 接收命名空间不可见的发送进程保持零值 */
static void test_hidden_sender(const char *path)
{
    int control[2], ack[2];
    REQUIRE(pipe(control) == 0 && pipe(ack) == 0);
    pid_t supervisor = fork();
    REQUIRE(supervisor >= 0);
    if (!supervisor) {
        close(control[1]);
        close(ack[0]);
        REQUIRE(unshare(CLONE_NEWPID) == 0);
        pid_t server = fork();
        REQUIRE(server >= 0);
        if (!server) {
            REQUIRE(getpid() == 1);
            run_server(path, control[0], ack[1], 1);
            _exit(0);
        }
        wait_ok(server);
        _exit(0);
    }
    close(control[0]);
    close(ack[1]);
    wait_ack(ack[0]);
    int fd = open_binder(path);
    send_transaction(fd, 5, true);
    allow_receive(control[1], 5, 0);
    wait_ack(ack[0]);
    close(fd);
    close(control[1]);
    close(ack[0]);
    wait_ok(supervisor);
    puts("PASS sender outside receiver PID namespace stays invisible");
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 4 && (!strcmp(argv[3], "0") || !strcmp(argv[3], "1")));
    setbuf(stdout, NULL);
    alarm(45);
    verify_private_node(argv[1], "hmos_pidtest");
    verify_private_node(argv[2], "hmos_pidtest_ns");
    struct stat first, second;
    REQUIRE(stat(argv[1], &first) == 0 && stat(argv[2], &second) == 0);
    REQUIRE(first.st_rdev != second.st_rdev);
    test_root_namespace(argv[1], argv[3][0] == '1');
    test_hidden_sender(argv[2]);
    puts("PASS Binder caller PID contract");
    return 0;
}
