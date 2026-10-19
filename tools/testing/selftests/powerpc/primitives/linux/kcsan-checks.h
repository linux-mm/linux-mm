/* SPDX-License-Identifier: GPL-2.0 */
/* There is nothing for KCSAN to check in user space. */
#define kcsan_check_read(p, size)	((void)(p), (void)(size))
