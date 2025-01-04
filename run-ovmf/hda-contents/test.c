#include <sys/io.h>
#include <stdio.h>

#define APM_CNT_PORT 0xB2

int main() {
    if (ioperm(APM_CNT_PORT, 1, 1)) {
        perror("ioperm");
        return 1;
    }

    outb(0x01, APM_CNT_PORT);

    return 0;
}
