#include <sys/io.h>
#include <stdio.h>
#define APM_CNT_PORT 0xb2

int main() {
    if (ioperm(APM_CNT_PORT, 1, 1)) {
        perror("ioperm error");
        return 1;
    }
    outb(0x01, APM_CNT_PORT);
    return 0;
}
