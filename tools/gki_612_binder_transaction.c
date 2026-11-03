/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <linux/android/binder.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAP_SIZE (1024U * 1024U)
#define TEST_CODE 0x53485031U
#define TEST_MAGIC UINT64_C(0x5348454e47495043)
#define PARENT_TOKEN UINT64_C(0x1234567812345678)
#define PARENT_FIRST UINT64_C(0x2345678923456789)
#define CHILD_TOKEN UINT64_C(0x3456789a3456789a)
#define TOKEN_GET _IOR('A', 1, unsigned long long)
#define TOKEN_SET _IOW('A', 2, unsigned long long)
#define FIRST_GET _IOR('A', 3, unsigned long long)
#define FIRST_SET _IOW('A', 4, unsigned long long)

struct endpoint {
    int fd;
    void *mapping;
};

struct payload {
    uint64_t magic;
    uint64_t inode;
    uint32_t sequence;
    uint32_t reserved;
    char text[32];
    struct binder_fd_object object;
};

_Static_assert(sizeof(binder_uintptr_t) == 8, "Binder protocol must be 64-bit");
_Static_assert(sizeof(struct binder_transaction_data) == 64, "Unexpected transaction ABI");
_Static_assert(sizeof(struct binder_fd_object) == 24, "Unexpected FD object ABI");
_Static_assert(sizeof(struct flat_binder_object) == 24, "Unexpected Binder object ABI");
_Static_assert(sizeof(struct payload) == 80, "Unexpected payload layout");
_Static_assert(offsetof(struct payload, object) == 56, "Unexpected object offset");
_Static_assert(BINDER_VERSION == 0xc0046209UL, "Unexpected Binder ioctl ABI");
_Static_assert(BINDER_WRITE_READ == 0xc0306201UL, "Unexpected Binder stream ABI");

static const char request_data[] = "sheng-fd-request\n";
static const char reply_data[] = "sheng-fd-reply\n";


static int error(const char *stage)
{
    fprintf(stderr, "SHENG_IPC_TRANSACTION_ERROR stage=%s errno=%d\n", stage, errno);
    return -1;
}


static int transfer(int fd, void *buffer, size_t length, int writing)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t count = writing ? write(fd, (char *)buffer + offset, length - offset) :
            read(fd, (char *)buffer + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return error("pipe-or-file-transfer");
        }
        offset += (size_t)count;
    }
    return 0;
}


/* 按字节拼接命令，避免结构体对齐插入额外数据 */
static int send_command(int fd, uint32_t command, const void *data, size_t length)
{
    unsigned char bytes[sizeof(command) + sizeof(struct binder_transaction_data)];
    struct binder_write_read operation = {0};

    if (length > sizeof(bytes) - sizeof(command)) {
        errno = EOVERFLOW;
        return error("command-size");
    }
    memcpy(bytes, &command, sizeof(command));
    if (length) {
        memcpy(bytes + sizeof(command), data, length);
    }
    operation.write_size = sizeof(command) + length;
    operation.write_buffer = (uintptr_t)bytes;
    if (ioctl(fd, BINDER_WRITE_READ, &operation) != 0 ||
        operation.write_consumed != operation.write_size) {
        return error("binder-write");
    }
    return 0;
}


static int open_endpoint(struct endpoint *endpoint)
{
    struct binder_version version = {0};
    uint32_t threads = 0;

    endpoint->fd = open("/dev/hmos_binder", O_RDWR | O_CLOEXEC);
    endpoint->mapping = MAP_FAILED;
    if (endpoint->fd < 0 || ioctl(endpoint->fd, BINDER_VERSION, &version) != 0 ||
        version.protocol_version != 8 ||
        ioctl(endpoint->fd, BINDER_SET_MAX_THREADS, &threads) != 0) {
        return error("binder-open-version");
    }
    endpoint->mapping = mmap(NULL, MAP_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE,
        endpoint->fd, 0);
    if (endpoint->mapping == MAP_FAILED) {
        return error("binder-mmap");
    }
    return 0;
}


static void close_endpoint(struct endpoint *endpoint)
{
    if (endpoint->mapping != MAP_FAILED) {
        munmap(endpoint->mapping, MAP_SIZE);
    }
    if (endpoint->fd >= 0) {
        close(endpoint->fd);
    }
}


/* 处理引用确认及返回消息，拒绝未知或截断的协议记录 */
static int receive_transaction(struct endpoint *endpoint, uint32_t expected,
    struct binder_transaction_data *transaction)
{
    for (;;) {
        unsigned char bytes[4096];
        struct binder_write_read operation = {0};
        size_t offset = 0;
        int found = 0;

        operation.read_size = sizeof(bytes);
        operation.read_buffer = (uintptr_t)bytes;
        if (ioctl(endpoint->fd, BINDER_WRITE_READ, &operation) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return error("binder-read");
        }
        if (!operation.read_consumed || operation.read_consumed > sizeof(bytes)) {
            errno = EPROTO;
            return error("binder-read-length");
        }
        while (offset < operation.read_consumed) {
            uint32_t command;
            size_t length;

            if (operation.read_consumed - offset < sizeof(command)) {
                errno = EPROTO;
                return error("binder-command-truncated");
            }
            memcpy(&command, bytes + offset, sizeof(command));
            offset += sizeof(command);
            length = _IOC_SIZE(command);
            if (length > operation.read_consumed - offset) {
                errno = EPROTO;
                return error("binder-payload-truncated");
            }
            if (command == expected && !found) {
                memcpy(transaction, bytes + offset, sizeof(*transaction));
                found = 1;
            } else if (command == BR_INCREFS || command == BR_ACQUIRE) {
                uint32_t reply = command == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE;
                if (send_command(endpoint->fd, reply, bytes + offset, length) != 0) {
                    return -1;
                }
            } else if (command != BR_NOOP && command != BR_TRANSACTION_COMPLETE &&
                command != BR_RELEASE && command != BR_DECREFS) {
                errno = EPROTO;
                return error("binder-unexpected-command");
            }
            offset += length;
        }
        if (found) {
            return 0;
        }
    }
}


static int mapped_range(struct endpoint *endpoint, binder_uintptr_t address, size_t size)
{
    uintptr_t start = (uintptr_t)endpoint->mapping;
    uintptr_t pointer = (uintptr_t)address;

    return pointer >= start && pointer - start <= MAP_SIZE &&
        size <= MAP_SIZE - (pointer - start);
}


/* 核对只读接收区内的载荷、对象偏移及新文件描述符 */
static int unpack(struct endpoint *endpoint, struct binder_transaction_data *transaction,
    uint32_t sequence, uint64_t inode, struct payload *packet)
{
    binder_size_t object_offset;
    struct stat state;
    int flags;
    const char *text = sequence == 1 ? "request-payload" : "reply-payload";

    if (transaction->code != TEST_CODE || transaction->data_size != sizeof(*packet) ||
        transaction->offsets_size != sizeof(object_offset) ||
        (transaction->flags & TF_STATUS_CODE) ||
        !mapped_range(endpoint, transaction->data.ptr.buffer, sizeof(*packet)) ||
        !mapped_range(endpoint, transaction->data.ptr.offsets, sizeof(object_offset))) {
        errno = EPROTO;
        return error("transaction-mapped-range");
    }
    memcpy(packet, (void *)(uintptr_t)transaction->data.ptr.buffer, sizeof(*packet));
    memcpy(&object_offset, (void *)(uintptr_t)transaction->data.ptr.offsets,
        sizeof(object_offset));
    if (object_offset != offsetof(struct payload, object) || packet->magic != TEST_MAGIC ||
        packet->sequence != sequence || packet->reserved != 0 ||
        memcmp(packet->text, text, strlen(text) + 1) != 0 ||
        packet->object.hdr.type != BINDER_TYPE_FD ||
        packet->object.fd > 0x7fffffffU || packet->inode != inode) {
        errno = EPROTO;
        return error("transaction-payload-or-object");
    }
    flags = fcntl((int)packet->object.fd, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0 ||
        fstat((int)packet->object.fd, &state) != 0 || !S_ISREG(state.st_mode) ||
        (uint64_t)state.st_ino != inode) {
        return error("transferred-fd-identity");
    }
    return 0;
}


static void make_payload(struct payload *packet, uint32_t sequence, uint64_t inode, int fd)
{
    const char *text = sequence == 1 ? "request-payload" : "reply-payload";

    memset(packet, 0, sizeof(*packet));
    packet->magic = TEST_MAGIC;
    packet->inode = inode;
    packet->sequence = sequence;
    memcpy(packet->text, text, strlen(text) + 1);
    packet->object.hdr.type = BINDER_TYPE_FD;
    packet->object.fd = (uint32_t)fd;
}


static int send_payload(struct endpoint *endpoint, uint32_t command, struct payload *packet)
{
    binder_size_t object_offset = offsetof(struct payload, object);
    struct binder_transaction_data transaction = {0};

    transaction.code = TEST_CODE;
    transaction.flags = TF_ACCEPT_FDS;
    transaction.data_size = sizeof(*packet);
    transaction.offsets_size = sizeof(object_offset);
    transaction.data.ptr.buffer = (uintptr_t)packet;
    transaction.data.ptr.offsets = (uintptr_t)&object_offset;
    return send_command(endpoint->fd, command, &transaction, sizeof(transaction));
}


static int release_buffer(struct endpoint *endpoint, struct binder_transaction_data *transaction)
{
    binder_uintptr_t buffer = transaction->data.ptr.buffer;

    return send_command(endpoint->fd, BC_FREE_BUFFER, &buffer, sizeof(buffer));
}


static int check_tokens(int fd, uint64_t expected, uint64_t first)
{
    uint64_t token = 0;
    uint64_t first_token = 0;

    if (ioctl(fd, TOKEN_GET, &token) != 0 || ioctl(fd, FIRST_GET, &first_token) != 0 ||
        token != expected || first_token != first) {
        errno = EPROTO;
        return error("task-token-inheritance");
    }
    return 0;
}


/* 客户端在发送后关闭原描述符，验证内核保存的跨进程引用 */
static int client(int token_fd, int ready_fd, int done_fd)
{
    struct endpoint endpoint = {.fd = -1, .mapping = MAP_FAILED};
    struct binder_transaction_data reply;
    struct payload packet;
    struct stat state;
    uint64_t token = CHILD_TOKEN;
    char bytes[64];
    char signal;
    int memory = -1;
    int received = -1;
    int result = -1;

    if (check_tokens(token_fd, PARENT_TOKEN, 0) != 0 ||
        ioctl(token_fd, TOKEN_SET, &token) != 0 ||
        transfer(ready_fd, &signal, 1, 0) != 0 || signal != 'R' ||
        open_endpoint(&endpoint) != 0) {
        goto done;
    }
    memory = memfd_create("sheng-ipc-roundtrip", MFD_CLOEXEC);
    if (memory < 0 || transfer(memory, (void *)request_data, sizeof(request_data) - 1, 1) != 0 ||
        lseek(memory, 0, SEEK_SET) != 0 || fstat(memory, &state) != 0) {
        error("client-memfd");
        goto done;
    }
    make_payload(&packet, 1, (uint64_t)state.st_ino, memory);
    if (send_payload(&endpoint, BC_TRANSACTION, &packet) != 0) {
        goto done;
    }
    close(memory);
    memory = -1;
    if (receive_transaction(&endpoint, BR_REPLY, &reply) != 0 ||
        unpack(&endpoint, &reply, 2, (uint64_t)state.st_ino, &packet) != 0) {
        goto done;
    }
    received = (int)packet.object.fd;
    if (lseek(received, 0, SEEK_CUR) != 3 ||
        pread(received, bytes, sizeof(reply_data) - 1, 0) != sizeof(reply_data) - 1 ||
        memcmp(bytes, reply_data, sizeof(reply_data) - 1) != 0 ||
        transfer(received, bytes, sizeof(reply_data) - 4, 0) != 0 ||
        memcmp(bytes, reply_data + 3, sizeof(reply_data) - 4) != 0 ||
        release_buffer(&endpoint, &reply) != 0 ||
        check_tokens(token_fd, CHILD_TOKEN, 0) != 0) {
        error("client-returned-file");
        goto done;
    }
    signal = 'D';
    if (transfer(done_fd, &signal, 1, 1) != 0) {
        goto done;
    }
    result = 0;
done:
    if (received >= 0) {
        close(received);
    }
    if (memory >= 0) {
        close(memory);
    }
    close_endpoint(&endpoint);
    return result;
}


/* 服务端回送同一文件对象，核对请求方身份及载荷内容 */
static int server(pid_t child, int ready_fd, int done_fd)
{
    struct endpoint endpoint = {.fd = -1, .mapping = MAP_FAILED};
    struct flat_binder_object manager = {0};
    struct binder_transaction_data request;
    struct payload packet;
    char bytes[64];
    char signal = 'R';
    int received = -1;
    int result = -1;

    manager.hdr.type = BINDER_TYPE_BINDER;
    manager.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS;
    if (open_endpoint(&endpoint) != 0 ||
        ioctl(endpoint.fd, BINDER_SET_CONTEXT_MGR_EXT, &manager) != 0 ||
        send_command(endpoint.fd, BC_ENTER_LOOPER, NULL, 0) != 0 ||
        transfer(ready_fd, &signal, 1, 1) != 0 ||
        receive_transaction(&endpoint, BR_TRANSACTION, &request) != 0) {
        goto done;
    }
    if (!mapped_range(&endpoint, request.data.ptr.buffer, sizeof(packet))) {
        errno = EPROTO;
        error("server-buffer-range");
        goto done;
    }
    memcpy(&packet, (void *)(uintptr_t)request.data.ptr.buffer, sizeof(packet));
    if (request.sender_pid != child || request.sender_euid != 0 ||
        unpack(&endpoint, &request, 1, packet.inode, &packet) != 0) {
        goto done;
    }
    received = (int)packet.object.fd;
    if (lseek(received, 0, SEEK_CUR) != 0 ||
        transfer(received, bytes, sizeof(request_data) - 1, 0) != 0 ||
        memcmp(bytes, request_data, sizeof(request_data) - 1) != 0 ||
        pwrite(received, reply_data, sizeof(reply_data) - 1, 0) != sizeof(reply_data) - 1 ||
        ftruncate(received, sizeof(reply_data) - 1) != 0 ||
        lseek(received, 3, SEEK_SET) != 3) {
        error("server-file-content");
        goto done;
    }
    make_payload(&packet, 2, packet.inode, received);
    if (send_payload(&endpoint, BC_REPLY, &packet) != 0) {
        goto done;
    }
    close(received);
    received = -1;
    if (release_buffer(&endpoint, &request) != 0 ||
        transfer(done_fd, &signal, 1, 0) != 0 || signal != 'D') {
        goto done;
    }
    result = 0;
done:
    if (received >= 0) {
        close(received);
    }
    close_endpoint(&endpoint);
    return result;
}


/* 独立子进程验证事务、文件描述符往返及任务令牌继承 */
int main(void)
{
    uint64_t token = PARENT_TOKEN;
    uint64_t first = PARENT_FIRST;
    int ready[2];
    int done[2];
    int token_fd;
    int status = 0;
    int result;
    pid_t child;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);
    token_fd = open("/dev/access_token_id", O_RDWR | O_CLOEXEC);
    if (token_fd < 0 || ioctl(token_fd, TOKEN_SET, &token) != 0 ||
        ioctl(token_fd, FIRST_SET, &first) != 0 ||
        pipe2(ready, O_CLOEXEC) != 0 || pipe2(done, O_CLOEXEC) != 0) {
        error("tokens-and-pipes");
        return 1;
    }
    child = fork();
    if (child < 0) {
        error("fork");
        return 1;
    }
    if (child == 0) {
        close(ready[1]);
        close(done[0]);
        result = client(token_fd, ready[0], done[1]);
        close(ready[0]);
        close(done[1]);
        close(token_fd);
        _exit(result == 0 ? 0 : 1);
    }
    close(ready[0]);
    close(done[1]);
    result = server(child, ready[1], done[0]);
    close(ready[1]);
    close(done[0]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        check_tokens(token_fd, PARENT_TOKEN, PARENT_FIRST) != 0) {
        result = -1;
    }
    token = 0;
    first = 0;
    if (ioctl(token_fd, TOKEN_SET, &token) != 0 || ioctl(token_fd, FIRST_SET, &first) != 0) {
        result = -1;
    }
    close(token_fd);
    if (result != 0) {
        puts("SHENG_GKI_612_IPC_TRANSACTION_FAILED");
        return 1;
    }
    puts("SHENG_GKI_612_IPC_FORK_TOKENS_OK");
    puts("SHENG_GKI_612_IPC_TRANSACTION_FD_ROUNDTRIP_OK");
    return 0;
}
