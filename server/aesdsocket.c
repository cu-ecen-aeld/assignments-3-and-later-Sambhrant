#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#define PORT 9000
#define BACKLOG 10
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_CHUNK 1024

static volatile sig_atomic_t exit_requested = 0;
static int listen_fd = -1;
static int conn_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;

    if (listen_fd != -1) {
        shutdown(listen_fd, SHUT_RDWR);
    }
    if (conn_fd != -1) {
        shutdown(conn_fd, SHUT_RDWR);
    }
}

static int daemonize_process(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        return -1;
    }

    if (chdir("/") == -1) {
        return -1;
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        return -1;
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        close(fd);
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        close(fd);
        return -1;
    }
    if (dup2(fd, STDERR_FILENO) == -1) {
        close(fd);
        return -1;
    }

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t sent = send(fd, buf + total, len - total, 0);
        if (sent == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)sent;
    }

    return 0;
}

static int send_file_to_client(int client_fd, int file_fd)
{
    if (lseek(file_fd, 0, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    char buffer[RECV_CHUNK];
    for (;;) {
        ssize_t bytes_read = read(file_fd, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (send_all(client_fd, buffer, (size_t)bytes_read) == -1) {
            return -1;
        }
    }

    if (lseek(file_fd, 0, SEEK_END) == (off_t)-1) {
        return -1;
    }

    return 0;
}

static int process_packet(int client_fd, int file_fd, const char *packet, size_t packet_len)
{
    size_t written_total = 0;

    while (written_total < packet_len) {
        ssize_t written = write(file_fd, packet + written_total, packet_len - written_total);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written_total += (size_t)written;
    }

    if (send_file_to_client(client_fd, file_fd) == -1) {
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    bool run_as_daemon = false;

    if (argc == 2) {
        if (strcmp(argv[1], "-d") == 0) {
            run_as_daemon = true;
        } else {
            return -1;
        }
    } else if (argc > 2) {
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        closelog();
        return -1;
    }

    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        close(listen_fd);
        closelog();
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(listen_fd);
        closelog();
        return -1;
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        close(listen_fd);
        closelog();
        return -1;
    }

    unlink(DATA_FILE);

    if (run_as_daemon) {
        if (daemonize_process() == -1) {
            close(listen_fd);
            closelog();
            return -1;
        }
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd == -1) {
            if (errno == EINTR) {
                if (exit_requested) {
                    break;
                }
                continue;
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            strncpy(client_ip, "unknown", sizeof(client_ip) - 1);
            client_ip[sizeof(client_ip) - 1] = '\0';
        }

        syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);

        int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
        if (data_fd == -1) {
            syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
            close(conn_fd);
            conn_fd = -1;
            syslog(LOG_DEBUG, "Closed connection from %s", client_ip);
            continue;
        }

        char *packet = NULL;
        size_t packet_size = 0;
        bool connection_active = true;

        while (connection_active) {
            char recvbuf[RECV_CHUNK];
            ssize_t recvlen = recv(conn_fd, recvbuf, sizeof(recvbuf), 0);

            if (recvlen == 0) {
                break;
            }

            if (recvlen == -1) {
                if (errno == EINTR) {
                    if (exit_requested) {
                        connection_active = false;
                        break;
                    }
                    continue;
                }
                connection_active = false;
                break;
            }

            char *newbuf = realloc(packet, packet_size + (size_t)recvlen + 1);
            if (newbuf == NULL) {
                syslog(LOG_ERR, "realloc failed");
                connection_active = false;
                break;
            }

            packet = newbuf;
            memcpy(packet + packet_size, recvbuf, (size_t)recvlen);
            packet_size += (size_t)recvlen;
            packet[packet_size] = '\0';

            char *start = packet;
            char *newline = NULL;

            while ((newline = strchr(start, '\n')) != NULL) {
                size_t chunk_len = (size_t)(newline - start + 1);

                if (process_packet(conn_fd, data_fd, start, chunk_len) == -1) {
                    connection_active = false;
                    break;
                }

                start = newline + 1;
            }

            if (!connection_active) {
                break;
            }

            size_t remaining = packet_size - (size_t)(start - packet);
            memmove(packet, start, remaining);
            packet_size = remaining;
            packet[packet_size] = '\0';
        }

        free(packet);
        close(data_fd);
        close(conn_fd);
        conn_fd = -1;

        syslog(LOG_DEBUG, "Closed connection from %s", client_ip);
    }

    if (conn_fd != -1) {
        close(conn_fd);
        conn_fd = -1;
    }

    if (listen_fd != -1) {
        close(listen_fd);
        listen_fd = -1;
    }

    unlink(DATA_FILE);
    closelog();
    return 0;
}