# MM CI GitHub Actions workflows

## Overview

This repository contains GitHub Actions workflow definitions, scripts and configuration files used by those workflows.

You can check the workflow runs on the [linux-mm/linux-mm actions page](https://github.com/linux-mm/linux-mm/actions).

**"MM CI"** refers to a continuous integration testing system targeting the memory management subsystem of the Linux Kernel.

MM CI consists of a number of components:
- [linux-mm/linux-mm](https://github.com/linux-mm/linux-mm) - a copy of Linux Kernel source repository tracking [upstream MM trees](https://git.kernel.org/pub/scm/linux/kernel/git/akpm/mm.git/)
- [Kernel Patches Daemon](https://github.com/kernel-patches/kernel-patches-daemon) (KPD) instance - a service connecting [Patchwork](https://patchwork.kernel.org/project/linux-mm/list/) with the GitHub repository
- [linux-mm/mm-ci](https://github.com/linux-mm/mm-ci) (this repository) - GitHub Actions workflows

Every patchset submitted to the linux-mm mailing list is added to the Linux MM project in Patchwork.

KPD monitors this patchwork project and generates pull requests in the linux-mm/linux-mm tree. A pull request contains workflow and scripts definitions from this repository and the submitted patches.

The patches are applied against one of the following branches in the MM tree:
- `mm-unstable`
- `mm-new`
- `linux`

If the patches apply to a branch, the other bases are not checked.

## Workflows and scripts

All the tests are driven by the `.github/workflows/mm-tests.yml` workflow.
It runs scripts from the `.github/scripts` directory that execute the actual tests.
The idea is to keep the workflow as simple as possible and to allow using the same test scripts locally.

There are two test categories: tests that run on the host and tests that run in a virtual machine.

### Host test

There are several MM-related test suites in the kernel that use the actual kernel source files and mocking to perform functional testing of a kernel subsystem in userspace.

Currently MM CI supports testing for memblock, VMA, and several important data structures: idr, maple and radix trees, and XArray.

### VM tests

The tests that require a kernel build are run in a virtual machine.

For simplicity, kernel build and VM execution are done with [virtme-ng](https://github.com/arighi/virtme-ng).

The files in `.github/kconfigs/` list the kernel configuration options required to run the tests with as much coverage as possible while still keeping the config small and the build fast.

MM CI runs the following tests in a virtual machine:
- MM selftests from the kernel tree
- KUnit tests for SLUB
