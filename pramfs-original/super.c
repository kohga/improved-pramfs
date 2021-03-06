/*
 * BRIEF DESCRIPTION
 *
 * Super block operations.
 *
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/parser.h>
#include <linux/vfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/mm.h>
#include <linux/ctype.h>
#include <linux/bitops.h>
#include <linux/magic.h>
#include <linux/exportfs.h>
#include <linux/random.h>
#include <linux/cred.h>
#include <linux/backing-dev.h>
#include <linux/ioport.h>
#include "xattr.h"
#include "pram.h"

static struct super_operations pram_sops;
static const struct export_operations pram_export_ops;
static struct kmem_cache *pram_inode_cachep;

#ifdef CONFIG_PRAMFS_TEST
static void *first_pram_super;

struct pram_super_block *get_pram_super(void)
{
	return (struct pram_super_block *)first_pram_super;
}
EXPORT_SYMBOL(get_pram_super);
#endif

void pram_error_mng(struct super_block *sb, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	printk(KERN_ERR "pramfs error: ");
	vprintk(fmt, args);
	printk("\n");
	va_end(args);

	if (test_opt(sb, ERRORS_PANIC))
		panic("pramfs: panic from previous error\n");
	if (test_opt(sb, ERRORS_RO)) {
		printk(KERN_CRIT "pramfs err: remounting filesystem read-only");
		sb->s_flags |= MS_RDONLY;
	}
}

static void pram_set_blocksize(struct super_block *sb, unsigned long size)
{
	int bits;

	/*
	 * We've already validated the user input and the value here must be
	 * between PRAM_MAX_BLOCK_SIZE and PRAM_MIN_BLOCK_SIZE
	 * and it must be a power of 2.
	 */
	bits = fls(size) - 1;
	sb->s_blocksize_bits = bits;
	sb->s_blocksize = (1<<bits);
}

static inline void *pram_ioremap(phys_addr_t phys_addr, ssize_t size,
				 bool protect)
{
	void *retval;

	/*
	 * NOTE: Userland may not map this resource, we will mark the region so
	 * /dev/mem and the sysfs MMIO access will not be allowed. This
	 * restriction depends on STRICT_DEVMEM option. If this option is
	 * disabled or not available we mark the region only as busy.
	 */
	retval = request_mem_region_exclusive(phys_addr, size, "pramfs");
	if (!retval)
		goto fail;

	if (protect) {
		retval = (__force void *)ioremap_nocache(phys_addr, size);
		if (!retval)
			goto fail;
		pram_writeable(retval, size, 0);
	} else
		retval = (__force void *)ioremap(phys_addr, size);
fail:
	return retval;
}

static loff_t pram_max_size(int bits)
{
	loff_t res;
	res = (1ULL << (3*bits - 6)) - 1;

	if (res > MAX_LFS_FILESIZE)
		res = MAX_LFS_FILESIZE;

	pram_info("max file size %llu bytes\n", res);
	return res;
}

enum {
	Opt_addr, Opt_bpi, Opt_size,
	Opt_num_inodes, Opt_mode, Opt_uid,
	Opt_gid, Opt_blocksize, Opt_user_xattr,
	Opt_nouser_xattr, Opt_noprotect,
	Opt_acl, Opt_noacl, Opt_xip,
	Opt_err_cont, Opt_err_panic, Opt_err_ro,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_bpi,		"physaddr=%x"},
	{Opt_bpi,		"bpi=%u"},
	{Opt_size,		"init=%s"},
	{Opt_num_inodes,	"N=%u"},
	{Opt_mode,		"mode=%o"},
	{Opt_uid,		"uid=%u"},
	{Opt_gid,		"gid=%u"},
	{Opt_blocksize,		"bs=%s"},
	{Opt_user_xattr,	"user_xattr"},
	{Opt_user_xattr,	"nouser_xattr"},
	{Opt_noprotect,		"noprotect"},
	{Opt_acl,		"acl"},
	{Opt_acl,		"noacl"},
	{Opt_xip,		"xip"},
	{Opt_err_cont,		"errors=continue"},
	{Opt_err_panic,		"errors=panic"},
	{Opt_err_ro,		"errors=remount-ro"},
	{Opt_err,		NULL},
};

static phys_addr_t get_phys_addr(void **data)
{
	phys_addr_t phys_addr;
	char *options = (char *) *data;
	unsigned long long ulltmp;
	char *end;
	char org_end;
	int err;

	if (!options || strncmp(options, "physaddr=", 9) != 0)
		return (phys_addr_t)ULLONG_MAX;
	options += 9;
	end = strchr(options, ',') ?: options + strlen(options);
	org_end = *end;
	*end = '\0';
	err = kstrtoull(options, 0, &ulltmp);
	*end = org_end;
	options = end;
	phys_addr = (phys_addr_t)ulltmp;
	if (err) {
		printk(KERN_ERR "Invalid phys addr specification: %s\n",
		       (char *) *data);
		return (phys_addr_t)ULLONG_MAX;
	}
	if (phys_addr & (PAGE_SIZE - 1)) {
		printk(KERN_ERR "physical address 0x%16llx for pramfs isn't "
			  "aligned to a page boundary\n",
			  (u64)phys_addr);
		return (phys_addr_t)ULLONG_MAX;
	}
	if (*options == ',')
		options++;
	*data = (void *) options;
	return phys_addr;
}

static int pram_parse_options(char *options, struct pram_sb_info *sbi,
			      bool remount)
{
	char *p, *rest;
	substring_t args[MAX_OPT_ARGS];
	int option;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_addr:
			if (remount)
				goto bad_opt;
			/* physaddr managed in get_phys_addr() */
			break;
		case Opt_bpi:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->bpi = option;
			break;
		case Opt_uid:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(sbi->uid))
				goto bad_val;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(sbi->gid))
				goto bad_val;
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				goto bad_val;
			sbi->mode = option & 01777U;
			break;
		case Opt_size:
			if (remount)
				goto bad_opt;
			/* memparse() will accept a K/M/G without a digit */
			if (!isdigit(*args[0].from))
				goto bad_val;
			sbi->initsize = memparse(args[0].from, &rest);
			break;
		case Opt_num_inodes:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
				sbi->num_inodes = option;
				break;
		case Opt_blocksize:
			if (remount)
				goto bad_opt;
			/* memparse() will accept a K/M/G without a digit */
			if (!isdigit(*args[0].from))
				goto bad_val;
			sbi->blocksize = memparse(args[0].from, &rest);
			if (sbi->blocksize < PRAM_MIN_BLOCK_SIZE ||
				sbi->blocksize > PRAM_MAX_BLOCK_SIZE ||
				!is_power_of_2(sbi->blocksize))
				goto bad_val;
			break;
		case Opt_err_panic:
			clear_opt(sbi->s_mount_opt, ERRORS_CONT);
			clear_opt(sbi->s_mount_opt, ERRORS_RO);
			set_opt(sbi->s_mount_opt, ERRORS_PANIC);
			break;
		case Opt_err_ro:
			clear_opt(sbi->s_mount_opt, ERRORS_CONT);
			clear_opt(sbi->s_mount_opt, ERRORS_PANIC);
			set_opt(sbi->s_mount_opt, ERRORS_RO);
			break;
		case Opt_err_cont:
			clear_opt(sbi->s_mount_opt, ERRORS_RO);
			clear_opt(sbi->s_mount_opt, ERRORS_PANIC);
			set_opt(sbi->s_mount_opt, ERRORS_CONT);
			break;
		case Opt_noprotect:
#ifdef CONFIG_PRAMFS_WRITE_PROTECT
			if (remount)
				goto bad_opt;
			clear_opt(sbi->s_mount_opt, PROTECT);
#endif
			break;
#ifdef CONFIG_PRAMFS_XATTR
		case Opt_user_xattr:
			set_opt(sbi->s_mount_opt, XATTR_USER);
			break;
		case Opt_nouser_xattr:
			clear_opt(sbi->s_mount_opt, XATTR_USER);
			break;
#else
		case Opt_user_xattr:
		case Opt_nouser_xattr:
			pram_info("(no)user_xattr options not supported\n");
			break;
#endif
#ifdef CONFIG_PRAMFS_POSIX_ACL
		case Opt_acl:
			set_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
		case Opt_noacl:
			clear_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
#else
		case Opt_acl:
		case Opt_noacl:
			pram_info("(no)acl options not supported\n");
			break;
#endif
		case Opt_xip:
#ifdef CONFIG_PRAMFS_XIP
			if (remount)
				goto bad_opt;
			set_opt(sbi->s_mount_opt, XIP);
			break;
#else
			pram_info("xip option not supported\n");
			break;
#endif
		default: {
			goto bad_opt;
		}
		}
	}

	return 0;

bad_val:
	printk(KERN_ERR "Bad value '%s' for mount option '%s'\n", args[0].from,
	       p);
	return -EINVAL;
bad_opt:
	printk(KERN_ERR "Bad mount option: \"%s\"\n", p);
	return -EINVAL;
}

static struct pram_inode *pram_init(struct super_block *sb, unsigned long size)
{
	unsigned long bpi, num_inodes, bitmap_size, blocksize, num_blocks;
	u64 bitmap_start;
	struct pram_inode *root_i;
	struct pram_super_block *super;
	struct pram_sb_info *sbi = PRAM_SB(sb);

	pram_info("creating an empty pramfs of size %lu\n", size);
	sbi->virt_addr = pram_ioremap(sbi->phys_addr, size,
							pram_is_protected(sb));

	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pramfs image failed\n");
		return ERR_PTR(-EINVAL);
	}

#ifdef CONFIG_PRAMFS_TEST
	if (!first_pram_super)
		first_pram_super = sbi->virt_addr;
#endif

	if (!sbi->blocksize)
		blocksize = PRAM_DEF_BLOCK_SIZE;
	else
		blocksize = sbi->blocksize;

	pram_set_blocksize(sb, blocksize);
	blocksize = sb->s_blocksize;

	if (sbi->blocksize && sbi->blocksize != blocksize)
		sbi->blocksize = blocksize;

	if (size < blocksize) {
		printk(KERN_ERR "size smaller then block size\n");
		return ERR_PTR(-EINVAL);
	}

	if (!sbi->bpi)
		/*
		 * default is that 5% of the filesystem is
		 * devoted to the inode table
		 */
		bpi = 20 * PRAM_INODE_SIZE;
	else
		bpi = sbi->bpi;

	if (!sbi->num_inodes)
		num_inodes = size / bpi;
	else
		num_inodes = sbi->num_inodes;

	/*
	 * up num_inodes such that the end of the inode table
	 * (and start of bitmap) is on a block boundary
	 */
	bitmap_start = (PRAM_SB_SIZE*2) + (num_inodes<<PRAM_INODE_BITS);
	if (bitmap_start & (blocksize - 1))
		bitmap_start = (bitmap_start + blocksize) &
			~(blocksize-1);
	num_inodes = (bitmap_start - (PRAM_SB_SIZE*2)) >> PRAM_INODE_BITS;

	if (sbi->num_inodes && num_inodes != sbi->num_inodes)
		sbi->num_inodes = num_inodes;

	num_blocks = (size - bitmap_start) >> sb->s_blocksize_bits;

	if (!num_blocks) {
		printk(KERN_ERR "num blocks equals to zero\n");
		return ERR_PTR(-EINVAL);
	}

	/* calc the data blocks in-use bitmap size in bytes */
	if (num_blocks & 7)
		bitmap_size = ((num_blocks + 8) & ~7) >> 3;
	else
		bitmap_size = num_blocks >> 3;
	/* round it up to the nearest blocksize boundary */
	if (bitmap_size & (blocksize - 1))
		bitmap_size = (bitmap_size + blocksize) & ~(blocksize-1);

	pram_info("blocksize %lu, num inodes %lu, num blocks %lu\n",
		  blocksize, num_inodes, num_blocks);
	pram_dbg("bitmap start 0x%08x, bitmap size %lu\n",
		 (unsigned int)bitmap_start, bitmap_size);
	pram_dbg("max name length %d\n", (unsigned int)PRAM_NAME_LEN);

	super = pram_get_super(sb);
	pram_memunlock_range(sb, super, bitmap_start + bitmap_size);

	/* clear out super-block and inode table */
	memset(super, 0, bitmap_start);
	super->s_size = cpu_to_be64(size);
	super->s_blocksize = cpu_to_be32(blocksize);
	super->s_inodes_count = cpu_to_be32(num_inodes);
	super->s_blocks_count = cpu_to_be32(num_blocks);
	super->s_free_inodes_count = cpu_to_be32(num_inodes - 1);
	super->s_bitmap_blocks = cpu_to_be32(bitmap_size >>
							  sb->s_blocksize_bits);
	super->s_free_blocks_count =
		cpu_to_be32(num_blocks - be32_to_cpu(super->s_bitmap_blocks));
	super->s_free_inode_hint = cpu_to_be32(1);
	super->s_bitmap_start = cpu_to_be64(bitmap_start);
	super->s_magic = cpu_to_be16(PRAM_SUPER_MAGIC);
	pram_sync_super(super);

	root_i = pram_get_inode(sb, PRAM_ROOT_INO);

	root_i->i_mode = cpu_to_be16(sbi->mode | S_IFDIR);
	root_i->i_uid = cpu_to_be32(sbi->uid);
	root_i->i_gid = cpu_to_be32(sbi->gid);
	root_i->i_links_count = cpu_to_be16(2);
	root_i->i_d.d_parent = cpu_to_be64(PRAM_ROOT_INO);
	pram_sync_inode(root_i);

	pram_init_bitmap(sb);

	pram_memlock_range(sb, super, bitmap_start + bitmap_size);

	return root_i;
}

static inline void set_default_opts(struct pram_sb_info *sbi)
{
#ifdef CONFIG_PRAMFS_WRITE_PROTECT
	set_opt(sbi->s_mount_opt, PROTECT);
#endif
	set_opt(sbi->s_mount_opt, ERRORS_CONT);
}

static void pram_root_check(struct super_block *sb, struct pram_inode *root_pi)
{
	pram_memunlock_inode(sb, root_pi);

	if (root_pi->i_d.d_next) {
		pram_warn("root->next not NULL, trying to fix\n");
		goto fail1;
	}

	if (!S_ISDIR(be16_to_cpu(root_pi->i_mode))) {
		pram_warn("root is not a directory, trying to fix\n");
		goto fail2;
	}
	
	if (pram_calc_checksum((u8 *)root_pi, PRAM_INODE_SIZE)) {
		pram_warn("checksum error in root inode, trying to fix\n");
		goto fail3;
	}
 fail1:
	root_pi->i_d.d_next = 0;
 fail2:
	root_pi->i_mode = cpu_to_be16(S_IRWXUGO|S_ISVTX|S_IFDIR);
 fail3:
	root_pi->i_d.d_parent = cpu_to_be64(PRAM_ROOT_INO);
	pram_memlock_inode(sb, root_pi);
}

static int pram_fill_super(struct super_block *sb, void *data, int silent)
{
	struct pram_super_block *super, *super_redund;
	struct pram_inode *root_pi;
	struct pram_sb_info *sbi = NULL;
	struct inode *root_i = NULL;
	unsigned long blocksize, initsize = 0;
	u32 random = 0;
	int retval = -EINVAL;

	BUILD_BUG_ON(sizeof(struct pram_super_block) > PRAM_SB_SIZE);
	BUILD_BUG_ON(sizeof(struct pram_inode) > PRAM_INODE_SIZE);

	sbi = kzalloc(sizeof(struct pram_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	set_default_opts(sbi);

	mutex_init(&sbi->s_lock);
#ifdef CONFIG_PRAMFS_XATTR
	spin_lock_init(&sbi->desc_tree_lock);
	sbi->desc_tree.rb_node = NULL;
#endif

	sbi->phys_addr = get_phys_addr(&data);
	if (sbi->phys_addr == (phys_addr_t)ULLONG_MAX)
		goto out;

	get_random_bytes(&random, sizeof(u32));
	atomic_set(&sbi->next_generation, random);

	/* Init with default values */
	sbi->mode = (S_IRWXUGO | S_ISVTX);
	sbi->uid = current_fsuid();
	sbi->gid = current_fsgid();

	if (pram_parse_options(data, sbi, 0))
		goto out;

	if (test_opt(sb, XIP) && test_opt(sb, PROTECT)) {
		printk(KERN_ERR "xip and protect options both enabled\n");
		goto out;
	}

	if (test_opt(sb, XIP) && sbi->blocksize != PAGE_SIZE) {
		printk(KERN_ERR "blocksize not equal to page size "
							 "and xip enabled\n");
		goto out;
	}

	initsize = sbi->initsize;

	/* Init a new pramfs instance */
	if (initsize) {
		root_pi = pram_init(sb, initsize);

		if (IS_ERR(root_pi))
			goto out;

		super = pram_get_super(sb);

		goto setup_sb;
	}

	pram_dbg("checking physical address 0x%016llx for pramfs image\n",
		   (u64)sbi->phys_addr);

	/* Map only one page for now. Will remap it when fs size is known. */
	initsize = PAGE_SIZE;
	sbi->virt_addr = pram_ioremap(sbi->phys_addr, initsize,
							pram_is_protected(sb));
	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pramfs image failed\n");
		goto out;
	}

	super = pram_get_super(sb);
	super_redund = pram_get_redund_super(sb);

	/* Do sanity checks on the superblock */
	if (be16_to_cpu(super->s_magic) != PRAM_SUPER_MAGIC) {
		if (be16_to_cpu(super_redund->s_magic) != PRAM_SUPER_MAGIC) {
			if (!silent)
				printk(KERN_ERR "Can't find a valid pramfs "
								"partition\n");
			goto out;
		} else {
			pram_warn("Error in super block: try to repair it with "
							  "the redundant copy");
			/* Try to auto-recover the super block */
			pram_memunlock_super(sb, super);
			memcpy(super, super_redund, PRAM_SB_SIZE);
			pram_memlock_super(sb, super);
		}
	}

	/* Read the superblock */
	if (pram_calc_checksum((u8 *)super, PRAM_SB_SIZE)) {
		if (pram_calc_checksum((u8 *)super_redund, PRAM_SB_SIZE)) {
			printk(KERN_ERR "checksum error in super block\n");
			goto out;
		} else {
			pram_warn("Error in super block: try to repair it with "
							  "the redundant copy");
			/* Try to auto-recover the super block */
			pram_memunlock_super(sb, super);
			memcpy(super, super_redund, PRAM_SB_SIZE);
			pram_memlock_super(sb, super);
		}
	}

	blocksize = be32_to_cpu(super->s_blocksize);
	pram_set_blocksize(sb, blocksize);

	initsize = be64_to_cpu(super->s_size);
	pram_info("pramfs image appears to be %lu KB in size\n", initsize>>10);
	pram_info("blocksize %lu\n", blocksize);

	/* Read the root inode */
	root_pi = pram_get_inode(sb, PRAM_ROOT_INO);

	/* Check that the root inode is in a sane state */
	pram_root_check(sb, root_pi);

	/* Remap the whole filesystem now */
	if (pram_is_protected(sb))
		pram_writeable(sbi->virt_addr, PAGE_SIZE, 1);
	iounmap((void __iomem *)sbi->virt_addr);
	release_mem_region(sbi->phys_addr, PAGE_SIZE);
	sbi->virt_addr = pram_ioremap(sbi->phys_addr, initsize,
							pram_is_protected(sb));
	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pramfs image failed\n");
		goto out;
	}
	super = pram_get_super(sb);

#ifdef CONFIG_PRAMFS_TEST
	if (!first_pram_super)
		first_pram_super = sbi->virt_addr;
#endif

	/* Set it all up.. */
 setup_sb:
	sb->s_magic = be16_to_cpu(super->s_magic);
	sb->s_op = &pram_sops;
	sb->s_maxbytes = pram_max_size(sb->s_blocksize_bits);
	sb->s_max_links = PRAM_LINK_MAX;
	sb->s_export_op = &pram_export_ops;
	sb->s_xattr = pram_xattr_handlers;
#ifdef	CONFIG_PRAMFS_POSIX_ACL
	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		(sbi->s_mount_opt & PRAM_MOUNT_POSIX_ACL) ?
		 MS_POSIXACL : 0;
#endif
	sb->s_flags |= MS_NOSEC;
	root_i = pram_iget(sb, PRAM_ROOT_INO);
	if (IS_ERR(root_i)) {
		retval = PTR_ERR(root_i);
		goto out;
	}
	
	sb->s_root = d_make_root(root_i);
	if (!sb->s_root) {
		printk(KERN_ERR "get pramfs root inode failed\n");
		retval = -ENOMEM;
		goto out;
	}

	retval = 0;
	return retval;
 out:
	if (sbi->virt_addr) {
		if (pram_is_protected(sb))
			pram_writeable(sbi->virt_addr, initsize, 1);
		iounmap((void __iomem *)sbi->virt_addr);
		release_mem_region(sbi->phys_addr, initsize);
	}

	kfree(sbi);
	return retval;
}

int pram_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	struct pram_super_block *ps = pram_get_super(sb);

	buf->f_type = PRAM_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = be32_to_cpu(ps->s_blocks_count);
	buf->f_bfree = buf->f_bavail = pram_count_free_blocks(sb);
	buf->f_files = be32_to_cpu(ps->s_inodes_count);
	buf->f_ffree = be32_to_cpu(ps->s_free_inodes_count);
	buf->f_namelen = PRAM_NAME_LEN;
	return 0;
}

static int pram_show_options(struct seq_file *seq, struct dentry *root)
{
	struct pram_sb_info *sbi = PRAM_SB(root->d_sb);

	seq_printf(seq, ",physaddr=0x%016llx", (u64)sbi->phys_addr);
	if (sbi->initsize)
		seq_printf(seq, ",init=%luk", sbi->initsize >> 10);
	if (sbi->blocksize)
		seq_printf(seq, ",bs=%lu", sbi->blocksize);
	if (sbi->bpi)
		seq_printf(seq, ",bpi=%lu", sbi->bpi);
	if (sbi->num_inodes)
		seq_printf(seq, ",N=%lu", sbi->num_inodes);
	if (sbi->mode != (S_IRWXUGO | S_ISVTX))
		seq_printf(seq, ",mode=%03o", sbi->mode);
	if (!uid_eq(sbi->uid, GLOBAL_ROOT_UID))
		seq_printf(seq, ",uid=%u",
				from_kuid_munged(&init_user_ns, sbi->uid));
	if (!gid_eq(sbi->gid, GLOBAL_ROOT_GID))
		seq_printf(seq, ",gid=%u",
				from_kgid_munged(&init_user_ns, sbi->gid));
	if (test_opt(root->d_sb, ERRORS_RO))
		seq_puts(seq, ",errors=remount-ro");
	if (test_opt(root->d_sb, ERRORS_PANIC))
		seq_puts(seq, ",errors=panic");
#ifdef CONFIG_PRAMFS_WRITE_PROTECT
	/* memory protection enabled by default */
	if (!test_opt(root->d_sb, PROTECT))
		seq_puts(seq, ",noprotect");
#else
	/*
	 * If it's not compiled say to the user that there
	 * isn't the protection.
	 */
	seq_puts(seq, ",noprotect");
#endif

#ifdef CONFIG_PRAMFS_XATTR
	/* user xattr not enabled by default */
	if (test_opt(root->d_sb, XATTR_USER))
		seq_puts(seq, ",user_xattr");
#endif

#ifdef CONFIG_PRAMFS_POSIX_ACL
	/* acl not enabled by default */
	if (test_opt(root->d_sb, POSIX_ACL))
		seq_puts(seq, ",acl");
#endif

#ifdef CONFIG_PRAMFS_XIP
	/* xip not enabled by default */
	if (test_opt(root->d_sb, XIP))
		seq_puts(seq, ",xip");
#endif

	return 0;
}

int pram_remount(struct super_block *sb, int *mntflags, char *data)
{
	unsigned long old_sb_flags;
	unsigned long old_mount_opt;
	struct pram_super_block *ps;
	struct pram_sb_info *sbi = PRAM_SB(sb);
	int ret = -EINVAL;

	/* Store the old options */
	old_sb_flags = sb->s_flags;
	old_mount_opt = sbi->s_mount_opt;

	if (pram_parse_options(data, sbi, 1))
		goto restore_opt;

	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		((sbi->s_mount_opt & PRAM_MOUNT_POSIX_ACL) ? MS_POSIXACL : 0);

	if ((*mntflags & MS_RDONLY) != (sb->s_flags & MS_RDONLY)) {
		mutex_lock(&PRAM_SB(sb)->s_lock);
		ps = pram_get_super(sb);
		pram_memunlock_super(sb, ps);
		/* update mount time */
		ps->s_mtime = cpu_to_be32(get_seconds());
		pram_memlock_super(sb, ps);
		mutex_unlock(&PRAM_SB(sb)->s_lock);
	}

	ret = 0;
	return ret;

 restore_opt:
	sb->s_flags = old_sb_flags;
	sbi->s_mount_opt = old_mount_opt;
	return ret;
}

static void pram_put_super(struct super_block *sb)
{
	struct pram_sb_info *sbi = PRAM_SB(sb);
	struct pram_super_block *ps = pram_get_super(sb);
	u64 size = be64_to_cpu(ps->s_size);

#ifdef CONFIG_PRAMFS_TEST
	if (first_pram_super == sbi->virt_addr)
		first_pram_super = NULL;
#endif

	pram_xattr_put_super(sb);
	/* It's unmount time, so unmap the pramfs memory */
	if (sbi->virt_addr) {
		if (pram_is_protected(sb))
			pram_writeable(sbi->virt_addr, size, 1);
		iounmap(sbi->virt_addr);
		sbi->virt_addr = NULL;
		release_mem_region(sbi->phys_addr, size);
	}

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static struct inode *pram_alloc_inode(struct super_block *sb)
{
	struct pram_inode_vfs *vi = (struct pram_inode_vfs *)
				kmem_cache_alloc(pram_inode_cachep, GFP_NOFS);
	if (!vi)
		return NULL;
	vi->vfs_inode.i_version = 1;
	return &vi->vfs_inode;
}

static void pram_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(pram_inode_cachep, PRAM_I(inode));
}

static void pram_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, pram_i_callback);
}

static void init_once(void *foo)
{
	struct pram_inode_vfs *vi = (struct pram_inode_vfs *) foo;

#ifdef CONFIG_PRAMFS_XATTR
	init_rwsem(&vi->xattr_sem);
#endif
	mutex_init(&vi->i_meta_mutex);
	mutex_init(&vi->i_link_mutex);
	inode_init_once(&vi->vfs_inode);
}

static int __init init_inodecache(void)
{
	pram_inode_cachep = kmem_cache_create("pram_inode_cache",
					     sizeof(struct pram_inode_vfs),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (pram_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	/*
         * Make sure all delayed rcu free inodes are flushed before we
         * destroy cache.
         */
	rcu_barrier();
	kmem_cache_destroy(pram_inode_cachep);
}

/*
 * the super block writes are all done "on the fly", so the
 * super block is never in a "dirty" state, so there's no need
 * for write_super.
 */
static struct super_operations pram_sops = {
	.alloc_inode	= pram_alloc_inode,
	.destroy_inode	= pram_destroy_inode,
	.write_inode	= pram_write_inode,
	.dirty_inode	= pram_dirty_inode,
	.evict_inode	= pram_evict_inode,
	.put_super	= pram_put_super,
	.freeze_fs 	= pram_freeze_fs,
	.unfreeze_fs 	= pram_unfreeze_fs,
	.statfs		= pram_statfs,
	.remount_fs	= pram_remount,
	.show_options	= pram_show_options,
};

static struct dentry *pram_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, pram_fill_super);
}

static struct file_system_type pram_fs_type = {
	.owner          = THIS_MODULE,
	.name           = "pramfs",
	.mount          = pram_mount,
	.kill_sb        = kill_anon_super,
};
MODULE_ALIAS_FS("pramfs");

static struct inode *pram_nfs_get_inode(struct super_block *sb,
		u64 ino, u32 generation)
{
	struct pram_super_block *ps = pram_get_super(sb);
	struct inode *inode;

	if (ino < PRAM_ROOT_INO)
		return ERR_PTR(-ESTALE);
	if (((ino - PRAM_ROOT_INO) >> PRAM_INODE_BITS) >
	  be32_to_cpu(ps->s_inodes_count))
		return ERR_PTR(-ESTALE);

	inode = pram_iget(sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	if (generation && inode->i_generation != generation) {
		/* we didn't find the right inode.. */
		iput(inode);
		return ERR_PTR(-ESTALE);
	}
	return inode;
}

static struct dentry *
pram_fh_to_dentry(struct super_block *sb, struct fid *fid, int fh_len,
		   int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    pram_nfs_get_inode);
}

static struct dentry *
pram_fh_to_parent(struct super_block *sb, struct fid *fid, int fh_len,
		   int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    pram_nfs_get_inode);
}

static const struct export_operations pram_export_ops = {
	.fh_to_dentry = pram_fh_to_dentry,
	.fh_to_parent = pram_fh_to_parent,
	.get_parent = pram_get_parent,
};

static int __init init_pram_fs(void)
{
	int rc = 0;

	rc = init_pram_xattr();
	if (rc)
		return rc;

	rc = init_inodecache();
	if (rc)
		goto out1;

	rc = bdi_init(&pram_backing_dev_info);
	if (rc)
		goto out2;

	rc = register_filesystem(&pram_fs_type);
	if (rc)
		goto out3;

	return 0;

out3:
	bdi_destroy(&pram_backing_dev_info);
out2:
	destroy_inodecache();
out1:
	exit_pram_xattr();
	return rc;
}

static void __exit exit_pram_fs(void)
{
	unregister_filesystem(&pram_fs_type);
	bdi_destroy(&pram_backing_dev_info);
	destroy_inodecache();
	exit_pram_xattr();
}

MODULE_AUTHOR("Marco Stornelli <marco.stornelli@gmail.com>");
MODULE_DESCRIPTION("Protected/Persistent RAM Filesystem");
MODULE_LICENSE("GPL");

module_init(init_pram_fs)
module_exit(exit_pram_fs)
