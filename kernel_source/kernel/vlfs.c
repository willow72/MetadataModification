#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dax.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/radix-tree.h>
#include "../fs/ext4/ext4.h"
#include "../fs/ext4/ext4_extents.h"
#include "../fs/ext4/ext4_jbd2.h"
#include <linux/backing-dev.h>

static void print_extent_info(struct ext4_extent_header *eh)
{
	int i;
	struct ext4_extent *e = EXT_FIRST_EXTENT(eh);
	printk("printk_extent_info start\n");
	printk("eh_entries : %d\n", le16_to_cpu(eh->eh_entries));
	for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, e++){
		printk("ee_start_hi : %ld\t", le16_to_cpu(e->ee_start_hi));
		printk("ee_start_lo : %ld\n", le32_to_cpu(e->ee_start_lo));
		printk("ee_pblock : %ld\n", ext4_ext_pblock(e));
		printk("ee_lblock : %ld\n", le32_to_cpu(e->ee_block));
		printk("ee_length : %d\n", le16_to_cpu(e->ee_len));
	}
	printk("---------\n");


}
static void realignment_lblock(struct ext4_extent_header *eh, int *l)
{
	int i;
	struct ext4_extent *e = EXT_FIRST_EXTENT(eh);
	for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, e++){	
		//printk("e->ee_block : %d\t, l : %d\n", le32_to_cpu(e->ee_block), *l);
		//printk("l : %d\n", (*l) + e->ee_len);
		e->ee_block = cpu_to_le32(*l);
//		printk("e->ee_block : %d\n", le32_to_cpu(e->ee_block));
		(*l) = (*l) + le16_to_cpu(e->ee_len);
//		printk("index : %d\n", (*l));
		
	}
}
static int __ext4_block_zero_page_range(handle_t *handle,
		struct address_space *mapping, loff_t from, loff_t length)
{
	ext4_fsblk_t index = from >> PAGE_SHIFT;
	unsigned offset = from & (PAGE_SIZE-1);
	unsigned blocksize, pos;
	ext4_lblk_t iblock;
	struct inode *inode = mapping->host;
	struct buffer_head *bh;
	struct page *page;
	int err = 0;
	page = find_or_create_page(mapping, from >> PAGE_SHIFT,
				   mapping_gfp_constraint(mapping, ~__GFP_FS));
	if (!page)
		return -ENOMEM;
	blocksize = inode->i_sb->s_blocksize;
	iblock = index << (PAGE_SHIFT - inode->i_sb->s_blocksize_bits);
	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	/* Find the buffer that contains "offset" */
	bh = page_buffers(page);
	pos = blocksize;
	while (offset >= pos) {
		bh = bh->b_this_page;
		iblock++;
		pos += blocksize;
	}
	if (buffer_freed(bh)) {
		BUFFER_TRACE(bh, "freed: skip");
		goto unlock;
	}
	if (!buffer_mapped(bh)) {
		BUFFER_TRACE(bh, "unmapped");
		ext4_get_block(inode, iblock, bh, 0);
		/* unmapped? It's a hole - nothing to do */
		if (!buffer_mapped(bh)) {
			BUFFER_TRACE(bh, "still unmapped");
			goto unlock;
		}
	}
	/* Ok, it's mapped. Make sure it's up-to-date */
	if (PageUptodate(page))
		set_buffer_uptodate(bh);
	if (!buffer_uptodate(bh)) {
		err = -EIO;
		ll_rw_block(REQ_OP_READ, 0, 1, &bh);
		wait_on_buffer(bh);
		/* Uhhuh. Read error. Complain and punt. */
		if (!buffer_uptodate(bh))
			goto unlock;
		if (S_ISREG(inode->i_mode) &&
		    ext4_encrypted_inode(inode)) {
			/* We expect the key to be set. */
			BUG_ON(!fscrypt_has_encryption_key(inode));
			BUG_ON(blocksize != PAGE_SIZE);
			WARN_ON_ONCE(fscrypt_decrypt_page(page));
		}
	}
	if (ext4_should_journal_data(inode)) {
		BUFFER_TRACE(bh, "get write access");
		err = ext4_journal_get_write_access(handle, bh);
		if (err)
			goto unlock;
	}
	zero_user(page, offset, length);
	BUFFER_TRACE(bh, "zeroed end of block");
	if (ext4_should_journal_data(inode)) {
		err = ext4_handle_dirty_metadata(handle, inode, bh);
	} else {
		err = 0;
		mark_buffer_dirty(bh);
		if (ext4_should_order_data(inode))
			err = ext4_jbd2_inode_add_write(handle, inode);
	}
unlock:
	unlock_page(page);
	put_page(page);
	return err;
}




/*
 * ext4_block_zero_page_range() zeros out a mapping of length 'length'
 * starting from file offset 'from'.  The range to be zero'd must
 * be contained with in one block.  If the specified range exceeds
 * the end of the block it will be shortened to end of the block
 * that cooresponds to 'from'
 */
static int ext4_block_zero_page_range(handle_t *handle,
		struct address_space *mapping, loff_t from, loff_t length)
{
	struct inode *inode = mapping->host;
	unsigned offset = from & (PAGE_SIZE-1);
	unsigned blocksize = inode->i_sb->s_blocksize;
	unsigned max = blocksize - (offset & (blocksize - 1));
	/*
	 * correct length if it does not fall between
	 * 'from' and the end of the block
	 */
	if (length > max || length < 0)
		length = max;
	if (IS_DAX(inode))
		return dax_zero_page_range(inode, from, length, ext4_get_block);
	return __ext4_block_zero_page_range(handle, mapping, from, length);
}


static int ext4_block_truncate_page(handle_t *handle,
		struct address_space *mapping, loff_t from)
{
	unsigned offset = from & (PAGE_SIZE-1);
	unsigned length;
	unsigned blocksize;
	struct inode *inode = mapping->host;
	blocksize = inode->i_sb->s_blocksize;
	length = blocksize - (offset & (blocksize - 1));
	return ext4_block_zero_page_range(handle, mapping, from, length);
}
void vlfs_ext4_ext_truncate(handle_t *handle, struct inode *inode, unsigned long offset, size_t len){
	//printk("[vlfs_ext4_ext_trucnate] - start\n");

	struct super_block *sb = inode->i_sb;
	ext4_lblk_t last_block;
	ext4_lblk_t start_block;
	int err = 0;

	EXT4_I(inode)->i_disksize = inode->i_size;
	ext4_mark_inode_dirty(handle, inode);
	
	start_block = offset >> 12;
	last_block = (start_block + (len >> 12))-1;

//	printk("%d to %d\n", start_block, last_block);	
	//last_block = (inode->i_size + sb->s_blocksize -1)
	//	>> EXT4_BLOCK_SIZE_BITS(sb);
retry :
	//err = ext4_es_remove_extent(inode, start_bloc,
	//	EXT_MAX_BLOCKS - last_block);
	err = ext4_es_remove_extent(inode, start_block,
		len);

	if(err == -ENOMEM){
		cond_resched();
		congestion_wait(BLK_RW_ASYNC, HZ/50);
		goto retry;
	}
	err = ext4_ext_remove_space(inode, start_block, last_block);
	ext4_std_error(inode->i_sb, err);
}
static int tour_extent(
	struct inode *inode, struct ext4_extent_idx *ix)
{
	ext4_fsblk_t block;
	struct buffer_head *bh;
	struct ext4_extent_header *eh;
	int i;
	block = ext4_idx_pblock(ix);
	bh = sb_bread(inode->i_sb, block);

	if(!bh){
		printk("bh null\n");
		return -EIO;
	}

	eh=(struct ext4_extent_header *) bh->b_data;
	if(le16_to_cpu(eh->eh_depth) != 0){
		ix = EXT_FIRST_INDEX(eh);
		for(i = 0; i<le16_to_cpu(eh->eh_entries); i++ ,ix++){
			printk("index node %lf\n",
				(unsigned long)(le16_to_cpu(ix->ei_leaf_hi)<<32) |
					le32_to_cpu(ix->ei_leaf_lo));
		}
	}else{
//		printk("leaf node\n");
		//print_extent_info(eh);
	}

}
static int realignment_tour(struct inode *inode, struct ext4_extent_idx *ix, int *l)
{
	ext4_fsblk_t block;
	struct buffer_head *bh;
	struct ext4_extent_header *eh;
	int i;
	
	block = ext4_idx_pblock(ix);
	bh = sb_bread(inode->i_sb, block);

	if(!bh){
		printk("bh null\n");
		return -EIO;
	}
	eh = (struct ext4_extent_header *)bh->b_data;
	if(le16_to_cpu(eh->eh_depth) != 0){
		ix = EXT_FIRST_INDEX(eh);
		for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, ix++){
			printk("index node\n");
		}
	}else{
		realignment_lblock(eh, l);
	}
}
static int realignment(struct inode *inode)
{
	printk("realignment function call\n");


	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_extent_header *eh = (struct ext4_extent_header *)ei->i_data;
	struct ext4_extent *e;
	struct ext4_extent_idx *ix;
	int index = 0;
	if(le16_to_cpu(eh->eh_depth) == 0)
//		print_extent_info(eh, &i);
		realignment_lblock(eh, &index);
	else{
		int i;
		ix = EXT_FIRST_INDEX(eh);
		for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, ix++){
			realignment_tour(inode, ix, &index);
		}
	}
	printk("realignment function finish\n");

}
/*static int __sync_inode (struct inode *inode, int datasync)
{
	int err;
	int ret;

	ret = sync_mapping_buffer(inode->i_mapping);
	if(!(inode->i_state & I_DIRTY))
		return ret;
	if(datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return ret;

	err = sync_inode_metadata(inode, 1);
	if(ret == 0)
		ret err;
	return ret;

}*/
static int ext4_releasepage(struct page *page, gfp_t wait)
{
	journal_t *journal = EXT4_JOURNAL(page->mapping->host);

//	trace_ext4_releasepage(page);
	if(PageChecked(page))
		return 0;
	if(journal)
		return jbd2_journal_try_to_free_buffers(journal, page, wait);
	else
		return try_to_free_buffers(page);
}
asmlinkage long sys_vlfs_modify(int fd, unsigned long offset, size_t len)
{
	struct file *file= fget(fd);
	struct inode *inode= file_inode(file);
	struct ext4_inode_info *ei = EXT4_I(inode);
	handle_t *handle;
	struct address_space *mapping = file->f_mapping;
	struct page *page;
	int credits;


	struct ext4_extent_header *eh = (struct ext4_extent_header *)ei->i_data;
	struct ext4_extent *e;
	struct ext4_extent_idx *ix;

	int tmp_offset = 0;

	ext4_fsblk_t blcok;
	
	printk("[sys_vlfs_modify] - start fd : %d\n", fd);

//	file = fget(fd);
//	inode = file_inode(file);
	inode->i_vb_count = 1;
	printk("[sys_vlfs_modify] - i_vb_count - %d\n", inode->i_vb_count);
	printk("[sys_vlfs_modify] - depth : %d\n", le16_to_cpu(eh->eh_depth));
/*	if(le16_to_cpu(eh->eh_depth) == 0)
		print_extent_info(eh);
	else{
		int i;
		ix = EXT_FIRST_INDEX(eh);
		for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, ix++){
			tour_extent(inode, ix);
		}
	}*/
	
	if(ext4_has_inline_data(inode)){
		int has_inline = 1;
	//	printk("[sys_vlfs_modify] - ext4_has_inline_data\n");
		ext4_inline_data_truncate(inode, &has_inline);
		if(has_inline)
			return;
	}

//	if(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		credits = ext4_writepage_trans_blocks(inode);
//	else
//		credits = ext4_blocks_for_truncate(inode);
//	printk("credits : %d\n", credits);
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);
	if(IS_ERR(handle)){
		ext4_std_error(inode->i_sb, PTR_ERR(handle));
		return 0;
	}

	if(inode->i_size & (inode->i_sb->s_blocksize -1)){
	//	printk("ext4_block_truncate_page\n");
		ext4_block_truncate_page(handle, mapping, inode->i_size);
	}

	if(ext4_orphan_add(handle, inode))
		goto out_stop;

	down_write(&EXT4_I(inode)->i_data_sem);

	ext4_discard_preallocations(inode);
//	printk("ext4_discard_preallocation\n");
	if(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
	vlfs_ext4_ext_truncate(handle, inode,offset, len);
//	vm_munmap(offset, len);
	printk("len : %d\n", len);
	inode->i_size -= len;
	while(tmp_offset < inode->i_size){
	//	printk("tmp_offset : %d\n", tmp_offset);
		page = NULL;
		page = find_get_page(mapping, tmp_offset>>12);
/*		if(page == NULL)
			printk("(1)page null\n");
		else
			printk("(1)page !null\n");
*/
		//ext4_releasepage(page, __GFP_HIGH);
		put_page(page);
		radix_tree_delete(&mapping->page_tree, tmp_offset>>12);
/*		page = NULL;
		page = find_get_page(mapping, tmp_offset>>12);
		if(page == NULL)
			printk("(2)page null\n");
		else
			printk("(2)page !null\n");*/
		tmp_offset += 4096;
	}
	realignment(inode);
	up_write(&ei->i_data_sem);

	if(inode->i_link)
		ext4_orphan_del(handle, inode);

	//ext4_sync_file(file, 0, LLONG_MAX, 0);
	if(le16_to_cpu(eh->eh_depth) == 0)
		print_extent_info(eh);
	else{
		int i;
		ix = EXT_FIRST_INDEX(eh);
		for(i = 0; i<le16_to_cpu(eh->eh_entries); i++, ix++){
			tour_extent(inode, ix);
		}
	}
out_stop:
	inode->i_mtime = inode->i_ctime = ext4_current_time(inode);
	ext4_mark_inode_dirty(handle, inode);
	ext4_journal_stop(handle);

	//__sync_inode(inode, 1);
}
