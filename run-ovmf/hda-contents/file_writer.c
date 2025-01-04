#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#define VAR_PATH "/sys/firmware/efi/efivars/MyVariable-a3a56e56-1d23-06dc-24bf-1473ff54e629"

int main() {
    // 要写入的8字节数字
    uint8_t cc[] = {0x07, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0xc6, 0xe8, 0x10, 0x00, 0x00, 0x00, 0x80, 0x81,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    // 打开文件
    int fd = open(VAR_PATH, O_WRONLY | O_CREAT);
    if (fd == -1) {
        perror("Error opening file");
        return 1;
    }

    // 写入数字
    ssize_t bytes_written = write(fd, cc, sizeof(cc));
    if (bytes_written != sizeof(cc)) {
        perror("Error writing to file");
        close(fd);
        return 1;
    }

    // 关闭文件
    close(fd);

    printf("Successfully wrote %ld bytes to %s\n", bytes_written, VAR_PATH);
    return 0;
}
