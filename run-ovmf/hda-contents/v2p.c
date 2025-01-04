#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define PAGEMAP_ENTRY 8
#define GET_BIT(X,Y) ((X & ((uint64_t)1<<Y)) >> Y)
#define GET_PFN(X) (X & 0x7FFFFFFFFFFFFF)

const int __endian_bit = 1;
#define is_bigendian() ( (*(char*)&__endian_bit) == 0 )

int i, c, pid, status;
unsigned long virt_addr;
uint64_t read_val, file_offset, page_size;
char path_buf [0x100] = {};
FILE * f;
char *end;

int read_pagemap(char * path_buf, unsigned long virt_addr);

int main(int argc, char ** argv){
    if(argc != 3){
        printf("Argument number is not correct! It must like:\n./VtoP PID VIRTUAL_ADDRESS\n");
        return -1;
    }
    if(!memcmp(argv[1], "self", sizeof("self"))){  //该VtoP进程自身
        sprintf(path_buf, "/proc/self/pagemap");
        pid = -1;
    }
    else{  //指定的进程
        pid = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || pid <= 0){
            printf("PID must be a positive number or 'self'\n");
            return -1;
        }
    }
    virt_addr = strtoll(argv[2], NULL, 16);
    if(pid != -1)
        sprintf(path_buf, "/proc/%u/pagemap", pid);

    page_size = getpagesize();  //获取页面大小
    read_pagemap(path_buf, virt_addr);  //读取页面映射内容
    return 0;
}

int read_pagemap(char * path_buf, unsigned long virt_addr){
    //printf("Big endian? %d\n", is_bigendian());
    f = fopen(path_buf, "rb");
    if(!f){
        printf("Error! Cannot open %s\n", path_buf);
        return -1;
    }

    /* 
     * 根据用户提供的虚拟内存地址计算该地址在文件中的偏移地址，公式为：
     * 文件中偏移地址 = virt_addr偏移的字节数 * pagemap文件中条目的大小
     */ 
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
    /*
     * 如果页面不存在，但在交换中，那么PFN包含交换文件编号的编码以及页面在交换中的偏移量。
     * 未映射的页返回空的PFN。这允许精确地确定映射（或交换）哪些页，并比较进程之间的映射页。
     */
    if(GET_BIT(read_val, 63)) {
        uint64_t pfn = GET_PFN(read_val);
        printf("PFN: 0x%llx (0x%llx)\n", pfn, pfn * page_size + virt_addr % page_size);
    }
    else printf("Page not present\n");
    if(GET_BIT(read_val, 62)) printf("Page swapped\n");
    fclose(f);
    return 0;
}

