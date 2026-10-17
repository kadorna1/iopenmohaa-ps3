/* probe.c (v3) -- smallest possible "is my EBOOT running, and how far does it get?"
 *
 * Lesson from the first run: stdio (fopen) must NOT be used from a static
 * constructor. The constructor below runs before the C library has finished
 * setting itself up, so only the raw lv2 file API is safe there. Everything in
 * ctor and in the first half of main() therefore uses lv2 only; the stdio
 * check comes last, wrapped in markers, so a crash inside fopen is visible.
 *
 * Log files (append): <dir>/probe-lv2.log in /dev_hdd0/tmp, the probe's USRDIR,
 * and the first two USB ports. Expected lines, in order:
 *   ctor / main start / main after sleep / stdio: before fopen / stdio: ok
 *
 * Build:
 *   export PSL1GHT="${PSL1GHT:-$PS3DEV}"
 *   $PS3DEV/ppu/bin/ppu-gcc -O2 -I$PSL1GHT/ppu/include -L$PSL1GHT/ppu/lib \
 *       probe.c -o probe.elf -lrt -llv2
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ppu-types.h>
#include <sys/file.h>

static const char *const dirs[] = {
    "/dev_hdd0/tmp",
    "/dev_hdd0/game/OMOHPROBE/USRDIR",
    "/dev_usb000",
    "/dev_usb001",
    NULL
};

/* lv2 only: no stdio, no printf family, safe from a constructor */
static void beacon(const char *tag)
{
    char line[96], path[160];
    size_t n = strlen(tag);
    int i;

    if (n > sizeof(line) - 2)
        n = sizeof(line) - 2;
    memcpy(line, tag, n);
    line[n++] = '\n';

    for (i = 0; dirs[i]; i++) {
        s32 fd = -1;
        u64 written = 0;

        strcpy(path, dirs[i]);
        strcat(path, "/probe-lv2.log");
        if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) == 0) {
            sysLv2FsWrite(fd, line, (u64)n, &written);
            sysLv2FsClose(fd);
        }
    }
}

__attribute__((constructor(101)))
static void early(void)
{
    beacon("ctor");
}

int main(void)
{
    FILE *f;

    beacon("main start");
    sleep(10);
    beacon("main after sleep");

    beacon("stdio: before fopen");
    f = fopen("/dev_hdd0/tmp/probe-stdio.log", "a");
    if (f) {
        fputs("stdio works\n", f);
        fclose(f);
        beacon("stdio: ok");
    } else {
        beacon("stdio: fopen returned NULL");
    }
    return 0;
}
