/* jce_tcp.c -- minimal blocking TCP with millisecond deadlines.
 * Platform symbols are confined to this TU (winsock2 / BSD sockets). */
#include <jce/os/platform/jce_tcp.h>

#include <jce/os/core/jce_alloc.h>

#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET jce_sock_t;
#  define JCE_SOCK_INVALID INVALID_SOCKET
#  define jce_sock_close   closesocket
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int jce_sock_t;
#  define JCE_SOCK_INVALID (-1)
#  define jce_sock_close   close
#endif

struct JceTcp {
    jce_sock_t fd;
};

static int tcp_global_init(void)
{
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        done = 1;
    }
#endif
    return 1;
}

static int wait_fd(jce_sock_t fd, int for_write, uint32_t timeout_ms)
{
    fd_set         set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec  = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    return select((int)(fd + 1), for_write ? NULL : &set,
                  for_write ? &set : NULL, NULL, &tv) == 1;
}

static void set_nonblocking(jce_sock_t fd, int on)
{
#ifdef _WIN32
    u_long mode = on ? 1u : 0u;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

static JceTcp* wrap(jce_sock_t fd)
{
    JceTcp* t = (JceTcp*)jce_malloc(sizeof(JceTcp));
    if (!t) { jce_sock_close(fd); return NULL; }
    t->fd = fd;
    return t;
}

JceTcp* JCE_CALL
jce_tcp_connect(const char* host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo  hints;
    struct addrinfo* res = NULL;
    jce_sock_t       fd  = JCE_SOCK_INVALID;
    char             portstr[8];

    if (!host || !tcp_global_init()) return NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    /* simple itoa (no snprintf dependency in this hot-path-free TU) */
    {
        int      n = 0;
        unsigned v = port;
        char     tmp[8];
        if (!v) tmp[n++] = '0';
        while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
        {
            int i;
            for (i = 0; i < n; ++i) portstr[i] = tmp[n - 1 - i];
            portstr[n] = 0;
        }
    }

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return NULL;

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == JCE_SOCK_INVALID) { freeaddrinfo(res); return NULL; }

    set_nonblocking(fd, 1);
    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        /* in progress: wait for writability */
        if (!wait_fd(fd, 1, timeout_ms)) {
            jce_sock_close(fd);
            freeaddrinfo(res);
            return NULL;
        }
        /* confirm the connect actually succeeded */
        {
            int       err = 0;
#ifdef _WIN32
            int       len = (int)sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
            socklen_t len = (socklen_t)sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
            if (err != 0) {
                jce_sock_close(fd);
                freeaddrinfo(res);
                return NULL;
            }
        }
    }
    set_nonblocking(fd, 0);
    freeaddrinfo(res);
    return wrap(fd);
}

bool JCE_CALL
jce_tcp_send_all(JceTcp* t, const void* data, size_t n, uint32_t timeout_ms)
{
    const char* p = (const char*)data;
    if (!t) return false;
    while (n) {
        int sent;
        if (!wait_fd(t->fd, 1, timeout_ms)) return false;
        sent = (int)send(t->fd, p, (int)(n > 65536 ? 65536 : n), 0);
        if (sent <= 0) return false;
        p += sent;
        n -= (size_t)sent;
    }
    return true;
}

int JCE_CALL
jce_tcp_recv(JceTcp* t, void* buf, size_t cap, uint32_t timeout_ms)
{
    int n;
    if (!t || !cap) return -1;
    if (!wait_fd(t->fd, 0, timeout_ms)) return -1; /* timeout */
    n = (int)recv(t->fd, (char*)buf, (int)(cap > 65536 ? 65536 : cap), 0);
    if (n < 0) return -1;
    return n; /* 0 = closed */
}

void JCE_CALL jce_tcp_close(JceTcp* t)
{
    if (!t) return;
    jce_sock_close(t->fd);
    jce_free(t);
}

JceTcp* JCE_CALL jce_tcp_listen(uint16_t port)
{
    jce_sock_t         fd;
    struct sockaddr_in addr;

    if (!tcp_global_init()) return NULL;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == JCE_SOCK_INVALID) return NULL;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(0x7F000001u); /* 127.0.0.1 */
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        jce_sock_close(fd);
        return NULL;
    }
    return wrap(fd);
}

uint16_t JCE_CALL jce_tcp_bound_port(JceTcp* server)
{
    struct sockaddr_in addr;
#ifdef _WIN32
    int len = (int)sizeof(addr);
#else
    socklen_t len = (socklen_t)sizeof(addr);
#endif
    if (!server) return 0;
    if (getsockname(server->fd, (struct sockaddr*)&addr, &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

JceTcp* JCE_CALL jce_tcp_accept(JceTcp* server, uint32_t timeout_ms)
{
    jce_sock_t fd;
    if (!server) return NULL;
    if (!wait_fd(server->fd, 0, timeout_ms)) return NULL;
    fd = accept(server->fd, NULL, NULL);
    if (fd == JCE_SOCK_INVALID) return NULL;
    return wrap(fd);
}
