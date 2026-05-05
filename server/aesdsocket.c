#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/queue.h>

#define PORT 9000
#define BACKLOG 20
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_CHUNK 1024
#define TIMESTAMP_INTERVAL_SEC 10

static volatile sig_atomic_t exit_requested = 0;
static int listen_fd = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_node {
    pthread_t thread_id;
    int client_fd;
    bool thread_complete_success;
    char client_ip[INET_ADDRSTRLEN];
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list_head, thread_node);
static struct thread_list_head thread_head;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;

    if (listen_fd != -1) {
        shutdown(listen_fd, SHUT_RDWR);
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

static int send_file_to_client_locked(int client_fd, int file_fd)
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

    return 0;
}

static int append_packet_and_respond(int client_fd, const char *packet, size_t packet_len)
{
    int ret = -1;

    if (pthread_mutex_lock(&file_mutex) != 0) {
        return -1;
    }

    int data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
    if (data_fd == -1) {
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    size_t written_total = 0;
    while (written_total < packet_len) {
        ssize_t written = write(data_fd, packet + written_total, packet_len - written_total);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            close(data_fd);
            pthread_mutex_unlock(&file_mutex);
            return -1;
        }
        written_total += (size_t)written;
    }

    ret = send_file_to_client_locked(client_fd, data_fd);

    close(data_fd);
    pthread_mutex_unlock(&file_mutex);
    return ret;
}

#ifdef ENABLE_TIMESTAMP_THREAD
static void *timestamp_thread_func(void *arg)
{
    (void)arg;

    while (!exit_requested) {
        struct timespec ts;
        ts.tv_sec = TIMESTAMP_INTERVAL_SEC;
        ts.tv_nsec = 0;

        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
            if (exit_requested) {
                return NULL;
            }
        }

        if (exit_requested) {
            break;
        }

        time_t now = time(NULL);
        if (now == (time_t)-1) {
            continue;
        }

        struct tm tm_now;
        if (localtime_r(&now, &tm_now) == NULL) {
            continue;
        }

        char timebuf[128];
        if (strftime(timebuf, sizeof(timebuf), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm_now) == 0) {
            continue;
        }

        if (pthread_mutex_lock(&file_mutex) != 0) {
            continue;
        }

        int data_fd = open(DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (data_fd != -1) {
            size_t len = strlen(timebuf);
            size_t total = 0;
            while (total < len) {
                ssize_t written = write(data_fd, timebuf + total, len - total);
                if (written == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                total += (size_t)written;
            }
            close(data_fd);
        }

        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}
#endif

static void *client_thread_func(void *arg)
{
    struct thread_node *node = (struct thread_node *)arg;
    char *packet = NULL;
    size_t packet_size = 0;

    for (;;) {
        char recvbuf[RECV_CHUNK];
        ssize_t recvlen = recv(node->client_fd, recvbuf, sizeof(recvbuf), 0);

        if (recvlen == 0) {
            break;
        }

        if (recvlen == -1) {
            if (errno == EINTR) {
                if (exit_requested) {
                    break;
                }
                continue;
            }
            break;
        }

        char *newbuf = realloc(packet, packet_size + (size_t)recvlen + 1);
        if (newbuf == NULL) {
            syslog(LOG_ERR, "realloc failed");
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

            if (append_packet_and_respond(node->client_fd, start, chunk_len) == -1) {
                goto cleanup;
            }

            start = newline + 1;
        }

        size_t remaining = packet_size - (size_t)(start - packet);
        memmove(packet, start, remaining);
        packet_size = remaining;
        packet[packet_size] = '\0';
    }

cleanup:
    free(packet);

    if (node->client_fd != -1) {
        shutdown(node->client_fd, SHUT_RDWR);
        close(node->client_fd);
        node->client_fd = -1;
    }

    syslog(LOG_DEBUG, "Closed connection from %s", node->client_ip);
    node->thread_complete_success = true;
    return arg;
}

static void shutdown_all_client_fds(void)
{
    struct thread_node *node;

    pthread_mutex_lock(&list_mutex);
    SLIST_FOREACH(node, &thread_head, entries) {
        if (node->client_fd != -1) {
            shutdown(node->client_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&list_mutex);
}

static void join_completed_threads(bool join_all)
{
    struct thread_node *node;
    struct thread_node *tmp;

    pthread_mutex_lock(&list_mutex);
    node = SLIST_FIRST(&thread_head);

    while (node != NULL) {
        tmp = SLIST_NEXT(node, entries);

        if (join_all || node->thread_complete_success) {
            SLIST_REMOVE(&thread_head, node, thread_node, entries);

            pthread_mutex_unlock(&list_mutex);
            pthread_join(node->thread_id, NULL);
            free(node);
            pthread_mutex_lock(&list_mutex);

            node = SLIST_FIRST(&thread_head);
            continue;
        }

        node = tmp;
    }

    pthread_mutex_unlock(&list_mutex);
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

    SLIST_INIT(&thread_head);
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

#ifdef ENABLE_TIMESTAMP_THREAD
    pthread_t timestamp_thread;
    bool timestamp_thread_started = false;
    if (pthread_create(&timestamp_thread, NULL, timestamp_thread_func, NULL) != 0) {
        close(listen_fd);
        closelog();
        return -1;
    }
    timestamp_thread_started = true;
#endif

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            if (errno == EINTR && exit_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        struct thread_node *node = calloc(1, sizeof(struct thread_node));
        if (node == NULL) {
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->thread_complete_success = false;

        if (inet_ntop(AF_INET, &client_addr.sin_addr, node->client_ip, sizeof(node->client_ip)) == NULL) {
            strncpy(node->client_ip, "unknown", sizeof(node->client_ip) - 1);
            node->client_ip[sizeof(node->client_ip) - 1] = '\0';
        }

        syslog(LOG_DEBUG, "Accepted connection from %s", node->client_ip);

        pthread_mutex_lock(&list_mutex);
        SLIST_INSERT_HEAD(&thread_head, node, entries);
        pthread_mutex_unlock(&list_mutex);

        if (pthread_create(&node->thread_id, NULL, client_thread_func, node) != 0) {
            pthread_mutex_lock(&list_mutex);
            SLIST_REMOVE(&thread_head, node, thread_node, entries);
            pthread_mutex_unlock(&list_mutex);
            close(client_fd);
            free(node);
            continue;
        }

        join_completed_threads(false);
    }

    exit_requested = 1;

    if (listen_fd != -1) {
        close(listen_fd);
        listen_fd = -1;
    }

    shutdown_all_client_fds();

#ifdef ENABLE_TIMESTAMP_THREAD
    if (timestamp_thread_started) {
        pthread_join(timestamp_thread, NULL);
    }
#endif

    join_completed_threads(true);

    unlink(DATA_FILE);
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);
    closelog();
    return 0;
}