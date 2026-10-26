// Split-screen IPC over a Unix SOCK_SEQPACKET socketpair (split_ipc.h).
#include "linux/split_ipc.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace split {

namespace {
struct Header {
    uint16_t type;
    uint16_t reserved;
    uint32_t size;
};
}  // namespace

bool Pair(int& compositor_end, int& player_end) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return false;
    compositor_end = sv[0];
    player_end = sv[1];
    // the player's end survives the exec that starts the player and every relaunch after
    fcntl(player_end, F_SETFD, 0);
    return true;
}

void SetNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

bool Send(int fd, const Message& m, const int* fds, int nfds) {
    if (m.body.size() > kMaxBody || nfds > kMaxFds) return false;
    Header h{uint16_t(m.type), 0, uint32_t(m.body.size())};
    iovec iov[2] = {{&h, sizeof(h)}, {const_cast<uint8_t*>(m.body.data()), m.body.size()}};
    msghdr mh{};
    mh.msg_iov = iov;
    mh.msg_iovlen = m.body.empty() ? 1 : 2;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
    if (nfds > 0) {
        mh.msg_control = control;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
        std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
    }
    for (;;) {
        ssize_t n = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (n >= 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

bool Receive(int fd, Message& m, bool* again) {
    if (again) *again = false;
    Header h{};
    uint8_t body[kMaxBody];
    iovec iov[2] = {{&h, sizeof(h)}, {body, sizeof(body)}};
    msghdr mh{};
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
    mh.msg_control = control;
    mh.msg_controllen = sizeof(control);
    ssize_t n;
    do {
        n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (again) *again = true;
        return false;
    }
    if (n <= 0 || size_t(n) < sizeof(h) || h.size > kMaxBody || size_t(n) != sizeof(h) + h.size) return false;
    m.type = Msg(h.type);
    m.body.assign(body, body + h.size);
    m.fds.clear();
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int f;
            std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            m.fds.push_back(f);
        }
    }
    return true;
}

}  // namespace split
