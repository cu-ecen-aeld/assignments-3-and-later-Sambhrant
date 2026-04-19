#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static int write_all(int fd, const void *buffer, size_t count, const char *filename)
{
    const char *ptr = buffer;
    while (count > 0) {
        ssize_t written = write(fd, ptr, count);
        if (written < 0) {
            syslog(LOG_ERR, "Could not write to %s: %s", filename, strerror(errno));
            return -1;
        }
        count -= (size_t)written;
        ptr += written;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Error: Two arguments required: <writefile> <writestr>\n");
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    openlog("writer", LOG_PID, LOG_USER);

    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        syslog(LOG_ERR, "Could not open %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    size_t str_len = strlen(writestr);
    if (str_len > 0) {
        if (write_all(fd, writestr, str_len, writefile) < 0) {
            close(fd);
            closelog();
            return 1;
        }
    }

    const char newline = '\n';
    if (write_all(fd, &newline, 1, writefile) < 0) {
        close(fd);
        closelog();
        return 1;
    }

    if (close(fd) < 0) {
        syslog(LOG_ERR, "Could not close %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);
    closelog();
    return 0;
}
