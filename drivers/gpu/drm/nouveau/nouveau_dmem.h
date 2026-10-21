/*
 * Copyright 2018 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef __NOUVEAU_DMEM_H__
#define __NOUVEAU_DMEM_H__
#include <nvif/os.h>
struct drm_device;
struct drm_file;
struct nouveau_drm;
struct nouveau_svmm;
struct hmm_range;
struct mmu_interval_notifier;

#if IS_ENABLED(CONFIG_DRM_NOUVEAU_SVM)
void nouveau_dmem_init(struct nouveau_drm *);
void nouveau_dmem_fini(struct nouveau_drm *);
void nouveau_dmem_suspend(struct nouveau_drm *);
void nouveau_dmem_resume(struct nouveau_drm *);

int nouveau_dmem_migrate_vma(struct nouveau_drm *drm,
			     struct nouveau_svmm *svmm,
			     struct vm_area_struct *vma,
			     unsigned long start,
			     unsigned long end);
int nouveau_dmem_migrate_fault(struct nouveau_drm *drm,
			       struct nouveau_svmm *svmm,
			       struct mmu_interval_notifier *notifier,
			       unsigned long start,
			       unsigned long end,
			       unsigned long hmm_flags,
			       u64 *out_pfn0,
			       unsigned long *out_addr,
			       unsigned int *out_shift);
unsigned long nouveau_dmem_page_addr(struct page *page);
u64 nouveau_dmem_hmm_pfn_to_map(unsigned long hmm_pfn, unsigned long start,
				unsigned long *map_addr, unsigned int *page_shift);

#else /* IS_ENABLED(CONFIG_DRM_NOUVEAU_SVM) */
static inline void nouveau_dmem_init(struct nouveau_drm *drm) {}
static inline void nouveau_dmem_fini(struct nouveau_drm *drm) {}
static inline void nouveau_dmem_suspend(struct nouveau_drm *drm) {}
static inline void nouveau_dmem_resume(struct nouveau_drm *drm) {}
#endif /* IS_ENABLED(CONFIG_DRM_NOUVEAU_SVM) */
#endif
