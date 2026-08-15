/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_ELF_PLUGINS_H
#define _LINUX_ELF_PLUGINS_H

#include <linux/binfmts.h>
#include <linux/elf.h>
#include <linux/list.h>

struct elf_plugin {
	struct list_head list;
	struct module *owner;
	struct file *(*open_interpreter)(struct linux_binprm *bprm,
					 struct elfhdr *elf_ex,
					 struct elf_phdr *elf_phdata);
};

#if IS_ENABLED(CONFIG_BINFMT_ELF_PLUGINS)
int register_elf_plugin(struct elf_plugin *plugin);
void unregister_elf_plugin(struct elf_plugin *plugin);
struct file *elf_plugin_open_interpreter(struct linux_binprm *bprm,
					 struct elfhdr *elf_ex,
					 struct elf_phdr *elf_phdata);
#else
static inline int register_elf_plugin(struct elf_plugin *plugin)
{
	return 0;
}
static inline void unregister_elf_plugin(struct elf_plugin *plugin)
{
}
static inline struct file *elf_plugin_open_interpreter(struct linux_binprm *bprm,
						       struct elfhdr *elf_ex,
						       struct elf_phdr *elf_phdata)
{
	return NULL;
}
#endif

#endif /* _LINUX_ELF_PLUGINS_H */
