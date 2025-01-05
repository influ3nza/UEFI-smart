#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>

#define PAGEMAP_ENTRY 8
#define GET_BIT(X,Y) ((X & ((uint64_t)1<<Y)) >> Y)
#define GET_PFN(X) (X & 0x7FFFFFFFFFFFFF)

const int __endian_bit = 1;
#define is_bigendian() ( (*(char*)&__endian_bit) == 0 )
#define VAR_PATH "/sys/firmware/efi/efivars/MyVariable-a3a56e56-1d23-06dc-24bf-1473ff54e629"

int i, c, pid, status;
unsigned long virt_addr;
int data_size;
char *data_content;
uint64_t read_val, file_offset, page_size;
char path_buf [0x100] = {};
FILE * f;
char *end;
uint64_t phys_addr;

enum CMD_TYPE {
    CMD_READ,
    CMD_WRITE
}cmd_type;

int read_pagemap(char * path_buf, unsigned long virt_addr);
int write_var(enum CMD_TYPE type);

int main(int argc, char ** argv){
    printf("%d\n", argc);
    if(argc != 6 && argc != 5){
        printf("Argument number is not correct! It must like:\n./VtoP PID VIRTUAL_ADDRESS METHOD(0:READ/1:WRITE) DATA_SIZE [DATA_CONTENT]\n");
        return -1;
    }
    if(!memcmp(argv[1], "self", sizeof("self"))){
        sprintf(path_buf, "/proc/self/pagemap");
        pid = -1;
    }
    else{
        pid = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || pid <= 0){
            printf("PID must be a positive number or 'self'\n");
            return -1;
        }
    }
    virt_addr = strtoll(argv[2], NULL, 16);
    if(pid != -1)
        sprintf(path_buf, "/proc/%u/pagemap", pid);

    cmd_type = strtol(argv[3], NULL, 10);

    data_size = strtol(argv[4], NULL, 10);

    data_content = argv[5];

    page_size = getpagesize();
    read_pagemap(path_buf, virt_addr);

    write_var(cmd_type);
    return 0;
}

int read_pagemap(char * path_buf, unsigned long virt_addr){
    //printf("Big endian? %d\n", is_bigendian());
    f = fopen(path_buf, "rb");
    if(!f){
        printf("Error! Cannot open %s\n", path_buf);
        return -1;
    }

    file_offset = virt_addr / page_size * PAGEMAP_ENTRY;
    printf("Vaddr: 0x%lx, Page_size: %lld, Entry_size: %d\n", virt_addr, page_size, PAGEMAP_ENTRY);
    printf("Reading %s at 0x%llx\n", path_buf, (unsigned long long) file_offset);
    status = fseek(f, file_offset, SEEK_SET);
    if(status){
        perror("Failed to do fseek!");
        return -1;
    }
    errno = 0;
    read_val = 0;
    unsigned char c_buf[PAGEMAP_ENTRY];
    for(i = 0; i < PAGEMAP_ENTRY; i++){
        c = getc(f);
        if(c == EOF){
            printf("\nReached end of the file\n");
            return 0;
        }
        if(is_bigendian()) c_buf[i] = c;
        else c_buf[PAGEMAP_ENTRY - i - 1] = c;
        printf("[%d]0x%x ", i, c);
    }
    for(i = 0; i < PAGEMAP_ENTRY; i++){
        read_val = (read_val << 8) + c_buf[i];
    }
    printf("\n");
    printf("Result: 0x%llx\n", (unsigned long long) read_val);
    
    if(GET_BIT(read_val, 63)) {
        uint64_t pfn = GET_PFN(read_val);
        phys_addr = pfn * page_size + virt_addr % page_size;
        printf("PFN: 0x%llx (0x%llx)\n", pfn, (unsigned long long) phys_addr);
    }
    else printf("Page not present\n");
    if(GET_BIT(read_val, 62)) printf("Page swapped\n");
    fclose(f);
    return 0;
}

void convertStringToUint8Array(const char *input, uint8_t *output, size_t *size) {
    char *token;
    char *inputCopy = strdup(input);
    size_t i = 0;

    token = strtok(inputCopy, " ");
    while (token != NULL) {
        output[i] = (uint8_t)strtol(token, NULL, 16);
        i++;
        token = strtok(NULL, " ");
    }

    *size = i;
    free(inputCopy);
}

int write_var(enum CMD_TYPE type) {
    printf("Ready to write in var\n\tparameterID: %d\n\ttarget phys addr: 0x%llx\n\tdata_size: %d\n", type, phys_addr, data_size);
    if (type == 1) {
        printf("Write data: %s\n", data_content);
    }
    
    uint8_t cc[4 + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t) + 128] = {
        0x07, 0x00, 0x00, 0x00
    };

    uint64_t type_64 = (uint64_t) type;
    memcpy(cc + 4, &type_64, sizeof(uint64_t));

    memcpy(cc + 4 + sizeof(uint64_t), &phys_addr, sizeof(uint64_t));

    uint64_t data_size_64 = (uint64_t) data_size;
    memcpy(cc + 4 + sizeof(uint64_t) + sizeof(uint64_t), &data_size_64, sizeof(uint64_t));

    int array_size = 0;
    if (type)
        convertStringToUint8Array(data_content, cc + 4 + 3*sizeof(uint64_t), &array_size);

    printf("cc array contents:\n");
    for (size_t i = 0; i < sizeof(cc); ++i) {
        printf("%02X ", cc[i]);
    }
    printf("\n");

    // 打开文件
    int fd = open(VAR_PATH, O_WRONLY | O_CREAT);
    if (fd == -1) {
        perror("Error opening file");
        return 1;
    }

    // 写入数字
    int need_write_size = type ? (4 + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t) + data_size) : (4 + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t));
    ssize_t bytes_written = write(fd, cc, need_write_size);
    printf("bytes_writter: %d\n", bytes_written);
    if (bytes_written != need_write_size) {
        perror("Error writing to file");
        close(fd);
        return 1;
    }

    // 关闭文件
    close(fd);

    printf("Successfully wrote %ld bytes to %s\n", bytes_written, VAR_PATH);

    return 0;
}

