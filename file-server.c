#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>

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
static void send_error(int fd, const char *msg) {
    send_u8(fd, STATUS_ERR);
    uint32_t len = strlen(msg);
    send_u32(fd, len);
    write_all(fd, msg, len);
}

static void handle_ls(int fd) {
    DIR *d = opendir(".");
    if (!d) { send_error(fd, strerror(errno)); return; }
    char **names = NULL;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        struct stat st;
        if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
            names = realloc(names, (count+1) * sizeof(*names));
            names[count++] = strdup(ent->d_name);
        }
    }
    closedir(d);
    send_u8(fd, STATUS_OK);
    send_u32(fd, count);
    for (int i = 0; i < count; i++) {
        uint32_t nlen = strlen(names[i]);
        send_u32(fd, nlen);
        write_all(fd, names[i], nlen);
        free(names[i]);
    }
    free(names);
}

static void handle_download(int fd) {
    uint32_t nlen;
    if (recv_u32(fd, &nlen) < 0) return;
    char *name = malloc(nlen + 1);
    if (read_all(fd, name, nlen) < 0) { free(name); return; }
    name[nlen] = '\0';
    if (strchr(name, '/') || strchr(name, '\\')) {
        send_error(fd, "invalid filename"); free(name); return;
    }
    int f = open(name, O_RDONLY);
    free(name);
    if (f < 0) { send_error(fd, strerror(errno)); return; }
    struct stat st;
    fstat(f, &st);
    uint64_t size = st.st_size;
    send_u8(fd, STATUS_OK);
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
}

static void handle_upload(int fd) {
    uint32_t nlen;
    if (recv_u32(fd, &nlen) < 0) return;
    char *name = malloc(nlen + 1);
    if (read_all(fd, name, nlen) < 0) { free(name); return; }
    name[nlen] = '\0';
    if (strchr(name, '/') || strchr(name, '\\')) {
        send_error(fd, "invalid filename"); free(name); return;
    }
    uint64_t size;
    if (recv_u64(fd, &size) < 0) { free(name); return; }
    int f = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    free(name);
    if (f < 0) { send_error(fd, strerror(errno)); return; }
    char buf[65536];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left < sizeof(buf) ? left : sizeof(buf);
        ssize_t r = read(fd, buf, want);
        if (r <= 0) { close(f); return; }
        write_all(f, buf, r);
        left -= r;
    }
    close(f);
    send_u8(fd, STATUS_OK);
}

static void *handle_client(void *arg) {
    int fd = *(int*)arg;
    free(arg);
    uint8_t cmd;
    while (recv_u8(fd, &cmd) == 0) {
        switch (cmd) {
            case CMD_LS:       handle_ls(fd);       break;
            case CMD_DOWNLOAD: handle_download(fd); break;
            case CMD_UPLOAD:   handle_upload(fd);   break;
            default: send_error(fd, "unknown command");
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <port>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { fprintf(stderr, "socket: %s\n", strerror(errno)); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind: %s\n", strerror(errno)); return 1;
    }
    if (listen(srv, 128) < 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno)); return 1;
    }
    printf("Listening on %d...\n", port);
    fflush(stdout);
    for (;;) {
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t;
        pthread_create(&t, NULL, handle_client, cfd);
        pthread_detach(t);
    }
}