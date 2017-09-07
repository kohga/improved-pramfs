/*
 * BRIEF DESCRIPTION
 *
 * Definitions for the PRAM filesystem.
 *
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _LINUX_PRAM_FS_H
#define _LINUX_PRAM_FS_H

//haga's pram_flags
#define PRAM_INIT		0x0001
#define PRAM_COMMIT		0x0002
#define PRAM_COW		0x0004
//#define PRAM_NEW		0x0008
//#define PRAM_OLD		0x0010

#include <uapi/linux/pram_fs.h>
#include <linux/mm.h>
/*
struct pram_cow {
	int pram_flags;
	struct pram_cow *prev,*next;
	void **kmem;
	unsigned long *pfn;
};
*/

extern void ck_pram_flags(void);
extern struct page *pram_page_prev;
extern int pram_flags;

//extern char *pram_xip_mem;
//extern struct page *pram_page_old;
//extern struct page *pram_page_new;
extern unsigned long pram_address;
//extern void cow_user_page(struct page *dst, struct page *src, unsigned long va, struct vm_area_struct *vma);
//extern int pram_cow_alloc_block_mem(struct address_space *mapping, pgoff_t pgoff, void **kmem, unsigned long *pfn);

//extern int pram_new_data_block(struct inode *inode, unsigned long *blocknr,int zero);
//extern void __pram_truncate_blocks(struct inode *inode, loff_t start,loff_t end);
//extern void pram_cow_user_page(struct page *dst, struct page *src, unsigned long va, struct vm_area_struct *vma);
extern int pram_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf);
extern int pram_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf);
extern int pram_remap(struct vm_area_struct *vma, unsigned long addr,
                             unsigned long size, pgoff_t pgoff);
extern int pram_xip_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf);
extern int pram_xip_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf);
/*
 * PRAM filesystem super-block data in memory
 */
struct pram_sb_info {
	/*
	 * base physical and virtual address of PRAMFS (which is also
	 * the pointer to the super block)
	 */
	phys_addr_t phys_addr;
	void *virt_addr;

	/* Mount options */
	unsigned long bpi;
	unsigned long num_inodes;
	unsigned long blocksize;
	unsigned long initsize;
	unsigned long s_mount_opt;
	kuid_t uid;		    /* Mount uid for root directory */
	kgid_t gid;		    /* Mount gid for root directory */
	umode_t mode;		    /* Mount mode for root directory */
	atomic_t next_generation;
#ifdef CONFIG_PRAMFS_XATTR
	struct rb_root desc_tree;
	spinlock_t desc_tree_lock;
#endif
	struct mutex s_lock;
};

#endif	/* _LINUX_PRAM_FS_H */
