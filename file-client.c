#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <libgen.h>

#define CMD_LS       1
#define CMD_DOWNLOAD 2
#define CMD_UPLOAD   3
#define STATUS_OK    0
#define STATUS_ERR   1

static int read_all(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char*)buf + done, n - done);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}
static int write_all(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char*)buf + done, n - done);
        if (w <= 0) return -1;
        done += w;
    }
    return 0;
}
static int send_u8(int fd, uint8_t v)   { return write_all(fd, &v, 1); }
static int send_u32(int fd, uint32_t v) { v = htonl(v); return write_all(fd, &v, 4); }
static int send_u64(int fd, uint64_t v) { v = htobe64(v); return write_all(fd, &v, 8); }
static int recv_u8(int fd, uint8_t *v)  { return read_all(fd, v, 1); }
static int recv_u32(int fd, uint32_t *v) {
    if (read_all(fd, v, 4) < 0) return -1;
    *v = ntohl(*v); return 0;
}
static int recv_u64(int fd, uint64_t *v) {
    if (read_all(fd, v, 8) < 0) return -1;
    *v = be64toh(*v); return 0;
}

static int connect_to(const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", strerror(errno)); return -1;
    }
    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "connect: %s\n", strerror(errno));
    return fd;
}

static void recv_and_print_error(int fd) {
    uint32_t len;
    if (recv_u32(fd, &len) < 0) { fprintf(stderr, "server error (unreadable)\n"); return; }
    char *msg = malloc(len + 1);
    read_all(fd, msg, len);
    msg[len] = '\0';
    fprintf(stderr, "server error: %s\n", msg);
    free(msg);
}

static int do_ls(int fd) {
    send_u8(fd, CMD_LS);
    uint8_t status;
    if (recv_u8(fd, &status) < 0) { fprintf(stderr, "no response\n"); return 1; }
    if (status != STATUS_OK) { recv_and_print_error(fd); return 1; }
    uint32_t count;
    if (recv_u32(fd, &count) < 0) return 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nlen;
        if (recv_u32(fd, &nlen) < 0) return 1;
        char *name = malloc(nlen + 1);
        read_all(fd, name, nlen);
        name[nlen] = '\0';
        printf("%s\n", name);
        free(name);
    }
    return 0;
}

static int do_upload(int fd, const char *local_path, const char *remote_name) {
    int f = open(local_path, O_RDONLY);
    if (f < 0) { fprintf(stderr, "open %s: %s\n", local_path, strerror(errno)); return 1; }
    struct stat st;
    fstat(f, &st);
    uint64_t size = st.st_size;
    send_u8(fd, CMD_UPLOAD);
    uint32_t nlen = strlen(remote_name);
    send_u32(fd, nlen);
    write_all(fd, remote_name, nlen);
    send_u64(fd, size);
    char buf[65536];
    uint64_t left = size;
    while (left > 0) {
        ssize_t r = read(f, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (r <= 0) break;
        write_all(fd, buf, r);
        left -= r;
    }
    close(f);
    uint8_t status;
    if (recv_u8(fd, &status) < 0) { fprintf(stderr, "no response\n"); return 1; }
    if (status != STATUS_OK) { recv_and_print_error(fd); return 1; }
    return 0;
}

static int do_download(int fd, const char *remote_name, const char *local_path) {
    send_u8(fd, CMD_DOWNLOAD);
    uint32_t nlen = strlen(remote_name);
    send_u32(fd, nlen);
    write_all(fd, remote_name, nlen);
    uint8_t status;
    if (recv_u8(fd, &status) < 0) { fprintf(stderr, "no response\n"); return 1; }
    if (status != STATUS_OK) { recv_and_print_error(fd); return 1; }
    uint64_t size;
    if (recv_u64(fd, &size) < 0) { fprintf(stderr, "no size\n"); return 1; }
    int f = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) { fprintf(stderr, "open %s: %s\n", local_path, strerror(errno)); return 1; }
    char buf[65536];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left < sizeof(buf) ? left : sizeof(buf);
        ssize_t r = read(fd, buf, want);
        if (r <= 0) break;
        write_all(f, buf, r);
        left -= r;
    }
    close(f);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <host> <port> <ls|upload|download> ...\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *cmd  = argv[3];
    int fd = connect_to(host, port);
    if (fd < 0) return 1;
    int rc = 1;
    if (strcmp(cmd, "ls") == 0) {
        rc = do_ls(fd);
    } else if (strcmp(cmd, "upload") == 0) {
        if (argc < 5) { fprintf(stderr, "upload: missing local-path\n"); goto done; }
        const char *local_path = argv[4];
        char *tmp = strdup(local_path);
        const char *remote_name = (argc >= 6) ? argv[5] : basename(tmp);
        rc = do_upload(fd, local_path, remote_name);
        free(tmp);
    } else if (strcmp(cmd, "download") == 0) {
        if (argc < 5) { fprintf(stderr, "download: missing file-name\n"); goto done; }
        const char *remote_name = argv[4];
        const char *local_path  = (argc >= 6) ? argv[5] : remote_name;
        rc = do_download(fd, remote_name, local_path);
    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
    }
done:
    close(fd);
    return rc;
}