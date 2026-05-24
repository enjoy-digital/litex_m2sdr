/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int run_and_check(const char *cmd, const char *expect_text, int expect_exit)
{
    FILE *p;
    char buf[4096];
    size_t len = 0;
    int rc;

    p = popen(cmd, "r");
    if (!p)
        return -1;
    while (len < sizeof(buf) - 1 && fgets(buf + len, (int)(sizeof(buf) - len), p) != NULL)
        len = strlen(buf);
    buf[len] = '\0';

    rc = pclose(p);
    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != expect_exit) {
        fprintf(stderr, "unexpected exit for command: %s\n", cmd);
        return -1;
    }
    if (!strstr(buf, expect_text)) {
        fprintf(stderr, "missing expected text '%s' for command: %s\n", expect_text, cmd);
        fprintf(stderr, "output:\n%s\n", buf);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (run_and_check("./m2sdr_sata --help 2>&1", "workflow commands:", 0) != 0)
        return 1;
    if (run_and_check("./m2sdr_sata --help 2>&1", "diag read SECTOR NSECTORS FILE|-", 0) != 0)
        return 1;
    if (run_and_check("./m2sdr_sata --dry-run diag read 0x1000 2 /tmp/x 2>&1",
                      "diag read dry-run", 0) != 0)
        return 1;
    if (run_and_check("./m2sdr_sata --dry-run diag write /tmp/x 0x1000 2 2>&1",
                      "diag write dry-run", 0) != 0)
        return 1;
    if (run_and_check("./m2sdr_sata --dry-run diag pattern-check 0x1000 2 2>&1",
                      "diag pattern-check dry-run", 0) != 0)
        return 1;
    if (run_and_check("./m2sdr_sata status 2>&1", "Unknown command: status", 1) != 0)
        return 1;

    printf("test_m2sdr_sata_cli: ok\n");
    return 0;
}
