#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

SYSCALL_DEFINE4(calc, int, param1, int, param2, char, operation, int __user *, result)
{
    int calc_result;
    
    // Validate operation
    if (operation != '+' && operation != '-' && operation != '*' && operation != '/') {
        return -1;
    }
    
    // Perform calculation
    switch (operation) {
        case '+':
            calc_result = param1 + param2;
            break;
        case '-':
            calc_result = param1 - param2;
            break;
        case '*':
            calc_result = param1 * param2;
            break;
        case '/':
            if (param2 == 0) {
                return -1;  // Division by zero
            }
            calc_result = param1 / param2;
            break;
        default:
            return -1;
    }
    
    // Copy result to user space
    if (copy_to_user(result, &calc_result, sizeof(int))) {
        return -1;
    }
    
    return 0;
}
