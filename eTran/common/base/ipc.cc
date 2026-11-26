#include "ipc.h"

/**
 * @brief this function ensures that all data in the buffer is written to the file descriptor
 */
ssize_t write_all(int fd, const void *buffer, size_t count)
{
    const char *buf = reinterpret_cast<const char *>(buffer);
    ssize_t total_written = 0;

    while (count > 0) {
        ssize_t written = write(fd, buf, count);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }

        count -= written;
        buf += written;
        total_written += written;
    }

    return total_written;
}

/**
 * @brief this function ensures that all data in the buffer is read from the file descriptor
 */
ssize_t read_all(int fd, void *buffer, size_t count) 
{
    char *buf = reinterpret_cast<char *>(buffer);
    ssize_t total_read = 0;

    while (count > 0) {
        ssize_t num_read = read(fd, buf, count);
        if (num_read == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }
        if (num_read == 0) {
            break;  // EOF reached
        }

        count -= num_read;
        buf += num_read;
        total_read += num_read;
    }

    return total_read;
}

/**
 * @brief send fd to another process through sockfd
 *
 * @param sockfd
 * @param fd
 * @return int
 */
int transfer_fd(int sockfd, int fd)
{
    return transfer_fds(sockfd, &fd, 1);
}

int transfer_fds(int sockfd, const int *fds, int fd_count)
{
    char payload = 0;
    iovec iov{&payload, sizeof(payload)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * fd_count)]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    auto *header = CMSG_FIRSTHDR(&msg);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(header), fds, sizeof(int) * fd_count);

    // SCM_RIGHTS is attached to one byte, so a successful write commits the whole batch.
    const ssize_t size = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
    return size < 0 ? -errno : size == 1 ? 0 : -EIO;
}

int receive_fd(int sockfd, int *fd)
{
    return receive_fds(sockfd, fd, 1);
}

int receive_fds(int sockfd, int *fds, int fd_count)
{
    char payload;
    iovec iov{&payload, sizeof(payload)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * fd_count)]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t size;
    do {
        size = recvmsg(sockfd, &msg, MSG_CMSG_CLOEXEC);
    } while (size < 0 && errno == EINTR);
    if (size < 0)
        return -errno;

    auto *header = CMSG_FIRSTHDR(&msg);
    if (size != 1 || !header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
        return -EPROTO;

    const size_t bytes = header->cmsg_len - CMSG_LEN(0);
    if ((msg.msg_flags & MSG_CTRUNC) || bytes != sizeof(int) * fd_count) {
        for (size_t i = 0; i < bytes / sizeof(int); ++i) {
            int fd;
            memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
            close(fd);
        }
        return -EPROTO;
    }
    memcpy(fds, CMSG_DATA(header), bytes);
    return 0;
}
