#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>

int main() {
    Dl_info info;

    if (dladdr((void *)pthread_mutex_lock, &info)) {
        printf("Function: %s\n", info.dli_sname);       // 实际符号名
        printf("Library: %s\n", info.dli_fname);        // 所在共享库路径
        printf("Address: %p\n", info.dli_fbase);       // 库基地址
    } else {
        printf("dladdr failed\n");
    }

    return 0;
}
