#include <cstdio>
#include <cstdlib>

using namespace std;

int main() {
    // 动态分配内存
    int* p = (int *)malloc(10 * sizeof(int)); // 分配一个包含 10 个整数的数组
    // 初始化分配的内存
    for (int i = 0; i < 10; ++i) {
        p[i] = i;
    }

    printf("Pointer p: %p\n", (void*)p);

    int d;
    scanf("%d", &d);

    for (int i = 0; i < 10; ++i) {
        printf("%d ", p[i]);
    }
    printf("\n");

    // 清理动态分配的内存
    free(p);

    return 0;
}

