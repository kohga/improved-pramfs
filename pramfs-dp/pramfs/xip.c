/*
 * BRIEF DESCRIPTION
 *
 * XIP operations.
 *
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "pram.h"
#include "xip.h"

//#define VM_SHARED	0x00000008

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * i_mutex.
 */
ssize_t pram_xip_file_read(struct file *filp, char __user *buf,
					size_t len, loff_t *ppos)
{
	//printk(KERN_DEBUG "pram_xip_file_read\n");
	ssize_t res;
	rcu_read_lock();
	res = xip_file_read(filp, buf, len, ppos);
	rcu_read_unlock();
	return res;
}

//static int pram_xip_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
int pram_xip_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	//printk(KERN_DEBUG "pram_xip_file_fault\n");
	//vma->vm_flags &= ~VM_SHARED;
	rcu_read_lock();
	ret = xip_file_fault(vma, vmf);
	rcu_read_unlock();
	return ret;
}

//static int pram_xip_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
int pram_xip_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret;
	//printk(KERN_DEBUG "pram__xip_mkwrite\n");
	ret = filemap_page_mkwrite(vma,vmf);
	return ret;
}

static const struct vm_operations_struct pram_xip_vm_ops = {
	.fault = pram_xip_file_fault,
	.page_mkwrite = pram_xip_mkwrite,
	.remap_pages = generic_file_remap_pages,
};

int pram_xip_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	//printk(KERN_DEBUG "pram_xip_file_mmap\n");

	BUG_ON(!file->f_mapping->a_ops->get_xip_mem);

	file_accessed(file);
	vma->vm_ops = &pram_xip_vm_ops;
	vma->vm_flags |= VM_MIXEDMAP;

	return 0;
}

static int pram_find_and_alloc_blocks(struct inode *inode, sector_t iblock,
				     sector_t *data_block, int create)
{
	//printk(KERN_DEBUG "pram_find_and_alloc_blocks\n");
	int err = -EIO;
	u64 block;

	block = pram_find_data_block(inode, iblock);
	//printk(KERN_DEBUG "xip;_00\n");

	if (!block) {
		if (!create) {
			err = -ENODATA;
			//printk(KERN_DEBUG "xip;_01\n");
			goto err;
		}

		err = pram_alloc_blocks(inode, iblock, 1);
		//printk(KERN_DEBUG "xip;_04a\n");
		if (err){
			//printk(KERN_DEBUG "xip;_02\n");
			goto err;
		}

		block = pram_find_data_block(inode, iblock);
		//printk(KERN_DEBUG "xip;_04b\n");
		if (!block) {
			err = -ENODATA;
			//printk(KERN_DEBUG "xip;_03\n");
			goto err;
		}
	}

	*data_block = block;
	//printk(KERN_DEBUG "xip;_04c\n");
	err = 0;

 err:
	return err;
}

static inline int __pram_get_block(struct inode *inode, pgoff_t pgoff,
				   int create, sector_t *result)
{
	//printk(KERN_DEBUG "__pram_get_block\n");
	int rc = 0;

	rc = pram_find_and_alloc_blocks(inode, (sector_t)pgoff, result, create);

	if (rc == -ENODATA){
		//printk(KERN_DEBUG "xip;01\n");
		BUG_ON(create);
	}

	return rc;
}

//sector_t NEW_block = 0;
//sector_t OLD_block = 0;
//struct address_space *cow_mapping = NULL;
//pgoff_t cow_pgoff = 0;
//loff_t cow_start = 0;
//loff_t cow_end = 0;
int pram_get_xip_mem(struct address_space *mapping, pgoff_t pgoff, int create,
		     void **kmem, unsigned long *pfn)
{
	//printk(KERN_DEBUG "*** pram_get_xip_mem ***\n");
	int rc;
	sector_t block = 0;
	//sector_t cow_new_block = 0;

	//ck_pram_flags();

	/* first, retrieve the block */
	rc = __pram_get_block(mapping->host, pgoff, create, &block);
	if (rc) {
		//printk(KERN_DEBUG "pram_get_xip_mem; goto exit\n");
		goto exit;
	}

	//1217add
#if 0

	if ( pram_flags & PRAM_COW ){
		printk(KERN_DEBUG "PRAMFS mmap Security Mode\n");
		printk(KERN_DEBUG "create copy...\n");

		OLD_block = block;

		/* copy process start*/
		struct inode *inode = mapping->host;
		int file_blocknr = (sector_t)pgoff;
		unsigned int num = 1;

		struct super_block *sb = inode->i_sb;
		struct pram_inode *pi = pram_get_inode(sb, inode->i_ino);
		int N = sb->s_blocksize >> 3; /* num block ptrs per block */
		int Nbits = sb->s_blocksize_bits - 3;
		int first_file_blocknr;
		int last_file_blocknr;
		int first_row_index, last_row_index;
		int i, j, errval;
		unsigned long blocknr;
		u64 *row;
		u64 *col;

		printk(KERN_DEBUG "cow;00\n");
		errval = pram_new_block(sb, &blocknr, 1);
		if (errval) {
			pram_dbg("failed to alloc 2nd order array block\n");
			goto fail_cow;
		}
		pram_memunlock_inode(sb, pi);
		pi->i_type.reg.row_block = cpu_to_be64(pram_get_block_off(sb,blocknr));
		pram_memlock_inode(sb, pi);

		row = pram_get_block(sb, be64_to_cpu(pi->i_type.reg.row_block));

		first_file_blocknr = file_blocknr;
		last_file_blocknr = file_blocknr + num - 1;

		first_row_index = first_file_blocknr >> Nbits;
		last_row_index  = last_file_blocknr >> Nbits;

		for (i = first_row_index; i <= last_row_index; i++) {
			printk(KERN_DEBUG "cow;01\n");

			int first_col_index, last_col_index;
			if (!row[i]) {
				printk(KERN_DEBUG "cow;02\n");
				errval = pram_new_block(sb, &blocknr, 1);
				if (errval) {
					pram_dbg("failed to alloc row block\n");
					goto fail_cow;
				}
				pram_memunlock_block(sb, row);
				row[i] = cpu_to_be64(pram_get_block_off(sb, blocknr));
				pram_memlock_block(sb, row);
			}

			printk(KERN_DEBUG "cow;03\n");
			col = pram_get_block(sb, be64_to_cpu(row[i]));

			first_col_index = (i == first_row_index) ?
				first_file_blocknr & (N-1) : 0;

			last_col_index = (i == last_row_index) ?
				last_file_blocknr & (N-1) : N-1;

			for (j = first_col_index; j <= last_col_index; j++) {
				printk(KERN_DEBUG "cow;04\n");

				if (!col[j]) {
					printk(KERN_DEBUG "cow;05\n");

					errval = pram_new_data_block(inode, &blocknr,1);
					if (errval) {
						pram_dbg("fail to alloc data block\n");
						if (j != first_col_index) {
							printk(KERN_DEBUG "cow;06\n");
							__pram_truncate_blocks(inode,
									inode->i_size,
									inode->i_size + ((j - first_col_index)
										<< inode->i_sb->s_blocksize_bits));
						}
						goto fail_cow;
					}
					cow_start = inode->i_size;
					cow_end = inode->i_size + ((j - first_col_index)<< inode->i_sb->s_blocksize_bits);

					pram_memunlock_block(sb, col);
					printk(KERN_DEBUG "cow;07\n");
					col[j] = cpu_to_be64(pram_get_block_off(sb,
								blocknr));
					pram_memlock_block(sb, col);
				}
			}
		}
		printk(KERN_DEBUG "cow;08\n");
		errval = 0;
fail_cow:
		printk(KERN_DEBUG "cow;09\n");
		if (errval){
			printk(KERN_DEBUG "cow;10\n");
			goto err_cow;
		}

		cow_new_block = pram_find_data_block(inode, file_blocknr);
		printk(KERN_DEBUG "cow;11\n");
		if (!cow_new_block) {
			errval = -ENODATA;
			printk(KERN_DEBUG "cow;12\n");
			goto err_cow;
		}

		//block = cow_new_block;
		NEW_block = cow_new_block;
		cow_mapping = mapping;
		cow_pgoff = pgoff;

		printk(KERN_DEBUG "cow;13\n");
		errval = 0;

		*kmem = pram_get_block(sb, NEW_block);
		*pfn =  pram_get_pfn(sb, NEW_block);

		return errval;

err_cow:
		printk(KERN_DEBUG "cow;14\n");
		/* copy process end*/
	}


	if ( (pram_flags & PRAM_INIT) && (pram_flags & PRAM_NEW) ){
		printk(KERN_DEBUG "Success!!\n");
		printk(KERN_DEBUG "delete OLD...\n");

		if ( (cow_mapping->host == mapping->host) && (cow_pgoff == pgoff) ){
			/* delete process */

			unsigned long del_blocknr;
			printk(KERN_DEBUG "ok mapping & pgoff...\n");
			*kmem = pram_get_block(mapping->host->i_sb, OLD_block);
			*pfn =  pram_get_pfn(mapping->host->i_sb, OLD_block);

			__pram_truncate_blocks(mapping->host,cow_start,cow_end);
			//del_blocknr = pram_get_blocknr((mapping->host)->i_sb,NEW_block);
			//pram_free_block( (mapping->host)->i_sb, del_blocknr);
			
			pram_flags &= ~PRAM_NEW;
			printk(KERN_DEBUG "ok mapping & pgoff...end\n");
			return rc;
		}
	} else if ( (pram_flags & PRAM_INIT) && (pram_flags & PRAM_OLD)) {
		printk(KERN_DEBUG "Not Commit! It's BUG by writing mmap!!\n");
		printk(KERN_DEBUG "delete New...\n");

		/* delete process */

		pram_flags &= ~PRAM_OLD;
	} else {
		/* nothing to do */
	}
	//1217
#endif

	*kmem = pram_get_block(mapping->host->i_sb, block);
	*pfn =  pram_get_pfn(mapping->host->i_sb, block);

exit:
	return rc;
}

void ck_pram_flags(void){
	printk(KERN_DEBUG "---------Now pram_flags---------\n");
	if (pram_flags & PRAM_INIT )
		printk(KERN_DEBUG "INIT\n");
	if (pram_flags & PRAM_COMMIT )
		printk(KERN_DEBUG "COMMIT\n");
	if (pram_flags & PRAM_COW )
		printk(KERN_DEBUG "COW\n");
	//if (pram_flags & PRAM_NEW )
	//	printk(KERN_DEBUG "NEW\n");
	//if (pram_flags & PRAM_OLD )
	//	printk(KERN_DEBUG "OLD\n");
	printk(KERN_DEBUG "pram_flags = %lx\n",pram_flags);
	printk(KERN_DEBUG "--------------------------------\n");
}

#if 0
int pram_cow_alloc_block_mem(struct address_space *mapping, pgoff_t pgoff, void **kmem, unsigned long *pfn){
	printk(KERN_DEBUG "** pram_cow_alloc_block_mem **\n");
	printk(KERN_DEBUG "PRAMFS mmap Security Mode\n");
	printk(KERN_DEBUG "create copy...\n");

	/* copy process start*/
	struct inode *inode = mapping->host;
	int file_blocknr = (sector_t)pgoff;
	unsigned int num = 1;

	struct super_block *sb = inode->i_sb;
	struct pram_inode *pi = pram_get_inode(sb, inode->i_ino);
	int N = sb->s_blocksize >> 3; /* num block ptrs per block */
	int Nbits = sb->s_blocksize_bits - 3;
	int first_file_blocknr;
	int last_file_blocknr;
	int first_row_index, last_row_index;
	int i, j, errval;
	unsigned long blocknr;
	u64 *row;
	u64 *col;
	sector_t block = 0;

	printk(KERN_DEBUG "cow;00\n");
	errval = pram_new_block(sb, &blocknr, 1);
	if (errval) {
		pram_dbg("failed to alloc 2nd order array block\n");
		goto fail_cow;
	}
	pram_memunlock_inode(sb, pi);
	pi->i_type.reg.row_block = cpu_to_be64(pram_get_block_off(sb,blocknr));
	pram_memlock_inode(sb, pi);

	row = pram_get_block(sb, be64_to_cpu(pi->i_type.reg.row_block));

	first_file_blocknr = file_blocknr;
	last_file_blocknr = file_blocknr + num - 1;

	first_row_index = first_file_blocknr >> Nbits;
	last_row_index  = last_file_blocknr >> Nbits;

	for (i = first_row_index; i <= last_row_index; i++) {
		printk(KERN_DEBUG "cow;01\n");

		int first_col_index, last_col_index;
		if (!row[i]) {
			printk(KERN_DEBUG "cow;02\n");
			errval = pram_new_block(sb, &blocknr, 1);
			if (errval) {
				pram_dbg("failed to alloc row block\n");
				goto fail_cow;
			}
			pram_memunlock_block(sb, row);
			row[i] = cpu_to_be64(pram_get_block_off(sb, blocknr));
			pram_memlock_block(sb, row);
		}

		printk(KERN_DEBUG "cow;03\n");
		col = pram_get_block(sb, be64_to_cpu(row[i]));

		first_col_index = (i == first_row_index) ?
			first_file_blocknr & (N-1) : 0;

		last_col_index = (i == last_row_index) ?
			last_file_blocknr & (N-1) : N-1;

		for (j = first_col_index; j <= last_col_index; j++) {
			printk(KERN_DEBUG "cow;04\n");

			if (!col[j]) {
				printk(KERN_DEBUG "cow;05\n");

				errval = pram_new_data_block(inode, &blocknr,1);
				if (errval) {
					pram_dbg("fail to alloc data block\n");
					if (j != first_col_index) {
						printk(KERN_DEBUG "cow;06\n");
						__pram_truncate_blocks(inode,
								inode->i_size,
								inode->i_size + ((j - first_col_index)
									<< inode->i_sb->s_blocksize_bits));
					}
					goto fail_cow;
				}

				pram_memunlock_block(sb, col);
				printk(KERN_DEBUG "cow;07\n");
				col[j] = cpu_to_be64(pram_get_block_off(sb,
							blocknr));
				pram_memlock_block(sb, col);
			}
		}
	}
	printk(KERN_DEBUG "cow;08\n");
	errval = 0;
fail_cow:
	printk(KERN_DEBUG "cow;09\n");
	if (errval){
		printk(KERN_DEBUG "cow;10\n");
		goto err_cow;
	}

	block = pram_find_data_block(inode, file_blocknr);
	printk(KERN_DEBUG "cow;11\n");
	if (!block) {
		errval = -ENODATA;
		printk(KERN_DEBUG "cow;12\n");
		goto err_cow;
	}

	printk(KERN_DEBUG "cow;13\n");
	errval = 0;

	*kmem = pram_get_block(sb, block);
	*pfn =  pram_get_pfn(sb, block);

	return errval;

err_cow:
	printk(KERN_DEBUG "cow;14\n");
	return errval;
}
#endif
