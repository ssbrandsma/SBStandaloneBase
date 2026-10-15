// SPDX-License-Identifier: GPL-2.0
/*
 * Diagnostic kernel-module loader and ABI compatibility probe.
 *
 * Used by Standalone Base storage research tooling to verify that a module
 * can load on the Radio kernel before considering storage module support.
 * Registers no devices, touches no storage and is not a runtime service.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

/**
 * @brief Initialize sbdiag.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int __init sbdiag_init(void)
{
	printk(KERN_INFO "sbdiag: loaded (diagnostic only)\n");
	return 0;
}

/**
 * @brief Handle sbdiag exit.
 */
static void __exit sbdiag_exit(void)
{
	printk(KERN_INFO "sbdiag: unloaded\n");
}

module_init(sbdiag_init);
module_exit(sbdiag_exit);

MODULE_DESCRIPTION("Squeezebox Radio kernel-module ABI probe");
MODULE_AUTHOR("SBStandaloneBase");
MODULE_LICENSE("GPL");
