#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#define VAR_PATH "/sys/firmware/efi/efivars/MyVariable-a3a56e56-1d23-06dc-24bf-1473ff54e629"

int main() {
    // 要写入的8字节数字
    uint64_t number = 1234567890123456789ULL;  // 示例数字

    // 打开文件
    int fd = open(VAR_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Error opening file");
        return 1;
    }

    // 写入数字
    ssize_t bytes_written = write(fd, &number, sizeof(number));
    if (bytes_written != sizeof(number)) {
        perror("Error writing to file");
        close(fd);
        return 1;
    }

    // 关闭文件
    close(fd);

    printf("Successfully wrote %ld bytes to %s\n", bytes_written, VAR_PATH);
    return 0;
}
