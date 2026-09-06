/* jtlabctl — send a command to a running jtlab bar over its control socket.
 *
 * Usage: jtlabctl <command>
 *   menu   toggle the app menu open/closed
 *   quit   exit the bar
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtlab-ctl.sock", rt);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> (menu|quit)\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("jtlabctl: socket");
        return 1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    ctl_path(addr.sun_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtlabctl: cannot connect to %s (is jtlab running?)\n",
                addr.sun_path);
        close(fd);
        return 1;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s\n", argv[1]);
    ssize_t n = write(fd, buf, strlen(buf));
    close(fd);
    return n < 0 ? 1 : 0;
}
