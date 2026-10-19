/* SPDX-License-Identifier: GPL-2.0 */
/* There is nothing for KASAN to check in user space. */
#define kasan_check_read(p, size)	((void)(p), (void)(size))
