#include <iostream>
#include <cstdio>
#include <cstdlib>

using namespace std;

int main() {
    // 动态分配内存
    int* p = new int[10]; // 分配一个包含 10 个整数的数组
    // 初始化分配的内存
    for (int i = 0; i < 10; ++i) {
        p[i] = i;
    }

    printf("Pointer p: %p\n", (void*)p);

    long long x;
    // 进入无限循环
    while (true) {
        cout << "Enter a hex address (e.g., 11de1c) or type 'exit' to quit: ";
        
        string input;
        cin >> input;

        // 输入 'exit' 时退出程序
        if (input == "exit") {
            break;
        }

        // 将输入转换为十六进制整数
        char* end;
        x = strtoll(input.c_str(), &end, 16);
        
        // 检查转换是否成功
        if (*end != '\0') {
            cout << "Invalid hex address." << endl;
            continue;
        }

        cout << "Input address: " << hex << x << endl;

        // 注意：使用解引用时要非常小心
        // 如果地址不在有效范围内，可能导致崩溃
        int* addr = (int*)x;
        try {
            cout << "Value at address: " << *addr << endl;
        } catch (...) {
            cout << "Error: Cannot access memory at address " << hex << x << endl;
        }
    }

    // 清理动态分配的内存
    delete[] p;

    return 0;
}

