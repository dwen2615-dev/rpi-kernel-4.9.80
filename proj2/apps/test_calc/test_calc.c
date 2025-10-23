#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define __NR_calc 397

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <num1> <operation> <num2>\n", argv[0]);
        return 1;
    }
    
    int param1 = atoi(argv[1]);
    char operation = argv[2][0];
    int param2 = atoi(argv[3]);
    int result;
    
    long ret = syscall(__NR_calc, param1, param2, operation, &result);
    
    if (ret == -1) {
        printf("NaN\n");
    } else {
        printf("%d\n", result);
    }
    
    return 0;
}
