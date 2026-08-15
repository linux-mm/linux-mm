// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/elf.h>
#include <linux/elf_plugins.h>
#include <linux/slab.h>

MODULE_DESCRIPTION("ELF Interpreter plugin for NixOS / $ORIGIN");
MODULE_AUTHOR("Farid Zakaria");
MODULE_LICENSE("GPL");

/* Mnemonic value for NixOS-specific program interpreter: 'N', 'I', 'X', 3 */
#define PT_INTERP_NIX  (PT_LOOS + 0x4e49583)

static struct file *nix_open_interpreter(struct linux_binprm *bprm,
					 struct elfhdr *elf_ex,
					 struct elf_phdr *elf_phdata)
{
	struct elf_phdr *elf_ppnt;
	struct file *interpreter = NULL;
	char *elf_interpreter = NULL;
	int i, retval;

	/* Find the custom Nix interpreter header */
	elf_ppnt = elf_phdata;
	for (i = 0; i < elf_ex->e_phnum; i++, elf_ppnt++) {
		if (elf_ppnt->p_type == PT_INTERP_NIX)
			break;
	}

	if (i == elf_ex->e_phnum)
		return NULL; /* Segment not present; fall back to others */

	/* Security check: refuse relative interp resolution on secure execution */
	if (bprm->secureexec) {
		pr_warn_once("binfmt_elf_nix: secureexec active, refusing custom interpreter lookup\n");
		return NULL; /* Fallback to standard PT_INTERP */
	}

	if (elf_ppnt->p_filesz > PATH_MAX || elf_ppnt->p_filesz < 2)
		return ERR_PTR(-ENOEXEC);

	elf_interpreter = kmalloc(elf_ppnt->p_filesz, GFP_KERNEL);
	if (!elf_interpreter)
		return ERR_PTR(-ENOMEM);

	/* Read the interpreter path from the executable file */
	retval = kernel_read(bprm->file, elf_interpreter, elf_ppnt->p_filesz, &elf_ppnt->p_offset);
	if (retval != elf_ppnt->p_filesz) {
		retval = (retval < 0) ? retval : -EIO;
		goto out_free;
	}

	if (elf_interpreter[elf_ppnt->p_filesz - 1] != '\0') {
		retval = -ENOEXEC;
		goto out_free;
	}

	/* Path Resolution: Absolute vs. $ORIGIN */
	if (elf_interpreter[0] == '/') {
		interpreter = open_exec(elf_interpreter);
	} else if (strncmp(elf_interpreter, "$ORIGIN/", 8) == 0 || strncmp(elf_interpreter, "${ORIGIN}/", 10) == 0) {
		const char *rel_path = (elf_interpreter[0] == '$') ? (elf_interpreter + 8) : (elf_interpreter + 10);
		struct path parent_path;

		/* Reference parent directory of the executed file safely */
		parent_path.mnt = mntget(bprm->file->f_path.mnt);
		parent_path.dentry = dget_parent(bprm->file->f_path.dentry);

		/* Open relative to parent directory */
		interpreter = file_open_root(&parent_path, rel_path, O_RDONLY, 0);

		path_put(&parent_path);
	} else {
		/* Naked relative paths are rejected for safety */
		retval = -ENOEXEC;
		goto out_free;
	}

	kfree(elf_interpreter);
	return interpreter;

out_free:
	kfree(elf_interpreter);
	return ERR_PTR(retval);
}

static struct elf_plugin nix_elf_plugin = {
	.owner = THIS_MODULE,
	.open_interpreter = nix_open_interpreter,
};

static int __init binfmt_elf_nix_init(void)
{
	return register_elf_plugin(&nix_elf_plugin);
}

static void __exit binfmt_elf_nix_exit(void)
{
	unregister_elf_plugin(&nix_elf_plugin);
}

module_init(binfmt_elf_nix_init);
module_exit(binfmt_elf_nix_exit);
