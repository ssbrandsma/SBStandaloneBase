// SPDX-License-Identifier: GPL-2.0
/* Harmless loader/ABI probe. It registers no devices and touches no storage. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init sbdiag_init(void)
{
	printk(KERN_INFO "sbdiag: loaded (diagnostic only)\n");
	return 0;
}

static void __exit sbdiag_exit(void)
{
	printk(KERN_INFO "sbdiag: unloaded\n");
}

module_init(sbdiag_init);
module_exit(sbdiag_exit);

MODULE_DESCRIPTION("Squeezebox Radio kernel-module ABI probe");
MODULE_AUTHOR("SBStandaloneBase");
MODULE_LICENSE("GPL");

