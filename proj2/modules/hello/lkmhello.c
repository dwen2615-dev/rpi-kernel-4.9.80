#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group05");
MODULE_DESCRIPTION("Hello World LKM");

static int __init lkmhello_init(void) {
    printk(KERN_INFO "Hello world! Group05 in kernel space\n");
    return 0;
}

static void __exit lkmhello_exit(void) {
    printk(KERN_INFO "Goodbye from Group05\n");
}

module_init(lkmhello_init);
module_exit(lkmhello_exit);
