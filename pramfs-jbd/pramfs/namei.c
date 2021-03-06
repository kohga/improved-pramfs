/*
 * BRIEF DESCRIPTION
 *
 * Inode operations for directories.
 *
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "pram.h"
#include "acl.h"
#include "xattr.h"
#include "xip.h"

/*
 * Couple of helper functions - make the code slightly cleaner.
 */

static inline void pram_inc_count(struct inode *inode)
{
	inc_nlink(inode);
	pram_write_inode(inode, NULL);
}

static inline void pram_dec_count(struct inode *inode)
{
	if (inode->i_nlink) {
		drop_nlink(inode);
		pram_write_inode(inode, NULL);
	}
}

static inline int pram_add_nondir(struct inode *dir,
				   struct dentry *dentry,
				   struct inode *inode)
{
	int err = pram_add_link(dentry, inode);
	if (!err) {
		unlock_new_inode(inode);
		d_instantiate(dentry, inode);
		return 0;
	}
	pram_dec_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

/*
 * Methods themselves.
 */

static ino_t pram_inode_by_name(struct inode *dir, struct dentry *dentry)
{
	struct pram_inode *pi;
	ino_t ino;
	int namelen;

	pi = pram_get_inode(dir->i_sb, dir->i_ino);
	ino = be64_to_cpu(pi->i_type.dir.head);

	mutex_lock(&PRAM_I(dir)->i_link_mutex);
	while (ino) {
		pi = pram_get_inode(dir->i_sb, ino);

		if (pi->i_links_count) {
			namelen = strlen(pi->i_d.d_name);

			if (namelen == dentry->d_name.len &&
			    !memcmp(dentry->d_name.name,
				    pi->i_d.d_name, namelen))
				break;
		}

		ino = be64_to_cpu(pi->i_d.d_next);
	}
	mutex_unlock(&PRAM_I(dir)->i_link_mutex);
	return ino;
}

static struct dentry *pram_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	if (dentry->d_name.len > PRAM_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = pram_inode_by_name(dir, dentry);
	if (ino) {
		inode = pram_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE)) {
			pram_err(dir->i_sb,
				"deleted inode referenced: %lu",
				(unsigned long) ino);
			return ERR_PTR(-EIO);
		}
	}

	return d_splice_alias(inode, dentry);
}


/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int pram_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool flags)
{
	struct inode *inode = pram_new_inode(dir, mode, &dentry->d_name);
	int err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &pram_file_inode_operations;
		if (pram_use_xip(inode->i_sb)) {
			inode->i_mapping->a_ops = &pram_aops_xip;
			inode->i_fop = &pram_xip_file_operations;
		} else {
			inode->i_fop = &pram_file_operations;
			inode->i_mapping->a_ops = &pram_aops;
		}
		err = pram_add_nondir(dir, dentry, inode);
	}
	return err;
}

static int pram_tmpfile(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode = pram_new_inode(dir, mode, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &pram_file_inode_operations;
	if (pram_use_xip(inode->i_sb)) {
		inode->i_mapping->a_ops = &pram_aops_xip;
		inode->i_fop = &pram_xip_file_operations;
	} else {
		inode->i_mapping->a_ops = &pram_aops;
		inode->i_fop = &pram_file_operations;
	}
	d_tmpfile(dentry, inode);
	unlock_new_inode(inode);
	return 0;
}

static int pram_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		      dev_t rdev)
{
	struct inode *inode = pram_new_inode(dir, mode, &dentry->d_name);
	int err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, mode, rdev);
		inode->i_op = &pram_special_inode_operations;
		pram_write_inode(inode, NULL); /* update rdev */
		err = pram_add_nondir(dir, dentry, inode);
	}
	return err;
}

static int pram_symlink(struct inode *dir, struct dentry *dentry,
			const char *symname)
{
	struct super_block *sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned len = strlen(symname);
	struct inode *inode;

	if (len+1 > sb->s_blocksize)
		goto out;

	inode = pram_new_inode(dir, S_IFLNK | S_IRWXUGO, &dentry->d_name);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out;

	inode->i_op = &pram_symlink_inode_operations;
	inode->i_mapping->a_ops = &pram_aops;

	err = pram_block_symlink(inode, symname, len);
	if (err)
		goto out_fail;

	inode->i_size = len;
	pram_write_inode(inode, NULL);

	err = pram_add_nondir(dir, dentry, inode);
out:
	return err;

out_fail:
	pram_dec_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	goto out;
}

static int pram_link(struct dentry *dest_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	pram_dbg("hard links not supported\n");
	return -EOPNOTSUPP;
}

static int pram_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	inode->i_ctime = dir->i_ctime;
	pram_dec_count(inode);
	return 0;
}

static int pram_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	struct pram_inode *pi;
	int err = 0;

	pram_inc_count(dir);

	inode = pram_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;

	inode->i_op = &pram_dir_inode_operations;
	inode->i_fop = &pram_dir_operations;
	inode->i_mapping->a_ops = &pram_aops;

	pram_inc_count(inode);

	/* make the new directory empty */
	pi = pram_get_inode(dir->i_sb, inode->i_ino);
	pram_memunlock_inode(dir->i_sb, pi);
	pi->i_type.dir.head = pi->i_type.dir.tail = 0;
	pram_memlock_inode(dir->i_sb, pi);

	err = pram_add_link(dentry, inode);
	if (err)
		goto out_fail;

	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
out:
	return err;

out_fail:
	pram_dec_count(inode);
	pram_dec_count(inode);
	unlock_new_inode(inode);
	iput(inode);
out_dir:
	pram_dec_count(dir);
	goto out;
}

static int pram_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct pram_inode *pi;
	int err = -ENOTEMPTY;

	if (!inode)
		return -ENOENT;

	pi = pram_get_inode(dir->i_sb, inode->i_ino);

	/* directory to delete is empty? */
	if (pi->i_type.dir.tail == 0) {
		inode->i_ctime = dir->i_ctime;
		inode->i_size = 0;
		clear_nlink(inode);
		pram_write_inode(inode, NULL);
		pram_dec_count(dir);
		err = 0;
	} else {
		pram_dbg("dir not empty\n");
	}

	return err;
}

static int pram_rename(struct inode  *old_dir,
			struct dentry *old_dentry,
			struct inode  *new_dir,
			struct dentry *new_dentry)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct pram_inode *pi_new;
	int err = -ENOENT;

	if (new_inode) {
		err = -ENOTEMPTY;
		pi_new = pram_get_inode(new_dir->i_sb, new_inode->i_ino);
		if (S_ISDIR(old_inode->i_mode)) {
			if (pi_new->i_type.dir.tail != 0)
				goto out;
			if (new_inode->i_nlink)
				drop_nlink(new_inode);
		}

		new_inode->i_ctime = CURRENT_TIME;
		pram_dec_count(new_inode);
	} else {
		if (S_ISDIR(old_inode->i_mode)) {
			pram_dec_count(old_dir);
			pram_inc_count(new_dir);
		}
	}

	/* unlink the inode from the old directory ... */
	err = pram_remove_link(old_inode);
	if (err)
		goto out;

	/* and link it into the new directory. */
	err = pram_add_link(new_dentry, old_inode);
	if (err)
		goto out;

	err = 0;
 out:
	return err;
}

struct dentry *pram_get_parent(struct dentry *child)
{
	struct inode *inode;
	struct pram_inode *pi, *piparent;
	ino_t ino;

	pi = pram_get_inode(child->d_inode->i_sb, child->d_inode->i_ino);
	if (!pi)
		return ERR_PTR(-EACCES);

	piparent = pram_get_inode(child->d_inode->i_sb,
				  be64_to_cpu(pi->i_d.d_parent));
	if (!pi)
		return ERR_PTR(-ENOENT);

	ino = pram_get_inodenr(child->d_inode->i_sb, piparent);
	if (ino)
		inode = pram_iget(child->d_inode->i_sb, ino);
	else
		return ERR_PTR(-ENOENT);

	return d_obtain_alias(inode);
}

const struct inode_operations pram_dir_inode_operations = {
	.create		= pram_create,
	.lookup		= pram_lookup,
	.link		= pram_link,
	.unlink		= pram_unlink,
	.symlink	= pram_symlink,
	.mkdir		= pram_mkdir,
	.rmdir		= pram_rmdir,
	.mknod		= pram_mknod,
	.rename		= pram_rename,
#ifdef CONFIG_PRAMFS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= pram_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.setattr	= pram_notify_change,
	.get_acl	= pram_get_acl,
	.tmpfile	= pram_tmpfile,
};

const struct inode_operations pram_special_inode_operations = {
#ifdef CONFIG_PRAMFS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= pram_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.setattr	= pram_notify_change,
	.get_acl	= pram_get_acl,
};
