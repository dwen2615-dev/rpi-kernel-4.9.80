#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <asm/unistd.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group05");
MODULE_DESCRIPTION("Replace sys_calc with modulo while loaded");

/* FIX 1: Use unsigned long * (array of entries), not unsigned long ** */
static unsigned long *syscall_table;
static asmlinkage long (*original_calc)(int, int, char, int __user *);

/* Function pointers for memory protection - FIX 5: Add typedef for clarity */
typedef void (*set_memory_func_t)(void);
static set_memory_func_t my_set_kernel_text_rw;
static set_memory_func_t my_set_kernel_text_ro;

#ifndef __NR_calc
#define __NR_calc 397
#endif

/* Our replacement that does modulo */
static asmlinkage long mod_calc(int a, int b, char op, int __user *out)
{
    int r;
    if (b == 0)
        return -1;
    r = a % b;
    return copy_to_user(out, &r, sizeof(r)) ? -1 : 0;
}

static int __init mod_calc_init(void)
{
    /* FIX 1: Cast to unsigned long *, not unsigned long ** */
    syscall_table = (unsigned long *)kallsyms_lookup_name("sys_call_table");
    if (!syscall_table) {
        pr_err("mod_calc: sys_call_table not found\n");
        return -ENOENT;
    }

    /* Lookup memory protection functions */
    my_set_kernel_text_rw = (set_memory_func_t)kallsyms_lookup_name("set_kernel_text_rw");
    my_set_kernel_text_ro = (set_memory_func_t)kallsyms_lookup_name("set_kernel_text_ro");

    /* FIX 4: Bail out if functions not found (when rodata is enforced) */
    if (!my_set_kernel_text_rw || !my_set_kernel_text_ro) {
        pr_err("mod_calc: set_kernel_text functions not found, cannot proceed safely\n");
        return -ENOENT;
    }

    /* FIX 2: Save as void *, cast when reading from table */
    original_calc = (void *)syscall_table[__NR_calc];

    pr_info("mod_calc: Found sys_call_table at %p\n", syscall_table);
    pr_info("mod_calc: Original sys_calc at %p\n", original_calc);

    /* FIX 3: Disable preemption during patch */
    preempt_disable();
    barrier();

    /* Make kernel text writable */
    my_set_kernel_text_rw();
    pr_info("mod_calc: Kernel text set to RW\n");

    /* FIX 2: Write machine word, not pointer-to-pointer */
    syscall_table[__NR_calc] = (unsigned long)mod_calc;

    /* Use barrier for ordering */
    barrier();

    /* Make kernel text read-only again */
    my_set_kernel_text_ro();
    pr_info("mod_calc: Kernel text set back to RO\n");

    /* FIX 3: Re-enable preemption */
    preempt_enable();

    pr_info("mod_calc: Hooked __NR_calc, modulo mode active\n");
    return 0;
}

static void __exit mod_calc_exit(void)
{
    if (!syscall_table || !original_calc || !my_set_kernel_text_rw || !my_set_kernel_text_ro) {
        pr_err("mod_calc: Cannot safely unhook\n");
        return;
    }

    /* FIX 3: Disable preemption during unpatch */
    preempt_disable();
    barrier();

    /* Make kernel text writable */
    my_set_kernel_text_rw();

    /* FIX 2: Restore original as machine word */
    syscall_table[__NR_calc] = (unsigned long)original_calc;

    barrier();

    /* Make kernel text read-only again */
    my_set_kernel_text_ro();

    /* FIX 3: Re-enable preemption */
    preempt_enable();

    pr_info("mod_calc: Unhooked, normal operation restored\n");
}

module_init(mod_calc_init);
module_exit(mod_calc_exit);
