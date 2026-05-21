#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/crc32.h>

MODULE_LICENSE("GPL");

#define SIMPLEFS_MAGIC 0xDEADBEEF
#define SIMPLEFS_SECTOR_SIZE 512

// Arguments
static char *disk_name = "";
static int sb_offsets[2] = {0, 100};
static int num_sb_offsets = 2;
static int max_name_len = 20;
static int max_file_sectors = 1;

module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Target block device name");

module_param_array(sb_offsets, int, &num_sb_offsets, 0444);
MODULE_PARM_DESC(sb_offsets, "Superblock offsets in sectors (first and second copy)");

module_param(max_name_len, int, 0444);
MODULE_PARM_DESC(max_name_len, "Maximum length of a file name");

module_param(max_file_sectors, int, 0444);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors (M)");

// 512 bytes
struct simplefs_super_block {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_files;
    uint32_t sb_copy_sector;
    uint32_t checksum;
    uint8_t  padding[492];
};

struct simplefs_sb_info {
    uint32_t total_files;
};

struct simplefs_mapping_args {
    int fd;
    uint32_t max_sectors;
    uint32_t num_sectors;
    uint64_t *sectors;
};

#define SIMPLEFS_IOC_MAGIC 'f'
#define SIMPLEFS_IOC_RESET_FILES   _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_WIPE_FS       _IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOC_GET_HASHES    _IOR(SIMPLEFS_IOC_MAGIC, 3, char*)
#define SIMPLEFS_IOC_GET_MAPPING   _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_mapping_args)

static const struct super_operations simplefs_sops;
static const struct inode_operations simplefs_inode_ops;
static const struct file_operations simplefs_file_ops;
static const struct file_operations simplefs_dir_ops;

static int write_zero_sector(struct super_block *sb, sector_t blocknr)
{
    struct buffer_head *bh = sb_getblk(sb, blocknr);
    if (!bh) return -EIO;
    memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
    mark_buffer_dirty(bh);
    brelse(bh);
    return 0;
}

static uint32_t simplefs_calculate_crc(struct simplefs_super_block *sb)
{
    uint32_t crc = ~0U;
    size_t len = offsetof(struct simplefs_super_block, checksum);
    crc = crc32_le(crc, (unsigned char *)sb, len);
    return crc ^ ~0U;
}

static bool is_superblock_sector(sector_t sector)
{
    int i;
    for (i = 0; i < num_sb_offsets; i++) {
        if (sb_offsets[i] == sector)
            return true;
    }
    return false;
}

static sector_t sfs_bypass_superblocks(sector_t target_sector)
{
    while (is_superblock_sector(target_sector)) {
        target_sector++;
    }
    return target_sector;
}

static inline sector_t sfs_ino_to_base_sector(unsigned long ino)
{
    unsigned int file_idx = (unsigned int)(ino - 2);
    return (sector_t)(2 + (file_idx * max_file_sectors));
}

/* Маппинг Индекса файла (0..99) в логический номер inode для VFS */
static inline unsigned long sfs_index_to_ino(unsigned int file_idx) {
    return (unsigned long)(file_idx + 1000);
}

/* Маппинг логического inode в Индекс файла (0..99) */
static inline unsigned int sfs_ino_to_index(unsigned long ino) {
    return (unsigned int)(ino - 1000);
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh = NULL;
    struct simplefs_super_block *disk_sb;
    struct inode *root_inode;
    struct simplefs_sb_info *sbi;
    uint32_t calc_crc;
    bool needs_format = false;

    sb->s_blocksize = SIMPLEFS_SECTOR_SIZE;
    sb->s_blocksize_bits = 9;
    sb->s_magic = SIMPLEFS_MAGIC;
    sb->s_op = &simplefs_sops;

    bh = sb_bread(sb, sb_offsets[0]);
     if (!bh) {
        needs_format = true;
    } else {
        disk_sb = (struct simplefs_super_block *)bh->b_data;
        calc_crc = simplefs_calculate_crc(disk_sb);
        if (disk_sb->magic != SIMPLEFS_MAGIC || disk_sb->checksum != calc_crc) {
            brelse(bh);
            needs_format = true;
        }
    }

     if (needs_format) {
        pr_info("simplefs: Superblock signature mismatch. Auto-formatting drive...\n");

        bh = sb_getblk(sb, sb_offsets[0]);
        if (!bh) return -EIO;

        disk_sb = (struct simplefs_super_block *)bh->b_data;
        memset(disk_sb, 0, SIMPLEFS_SECTOR_SIZE);
        disk_sb->magic = SIMPLEFS_MAGIC;
        disk_sb->block_size = SIMPLEFS_SECTOR_SIZE;
        disk_sb->total_files = 100; 
        disk_sb->sb_copy_sector = sb_offsets[0];
        disk_sb->checksum = simplefs_calculate_crc(disk_sb);

        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh); 

        if (num_sb_offsets > 1) {
            int j;
            for (j = 1; j < num_sb_offsets; j++) {
                struct buffer_head *bh_backup = sb_getblk(sb, sb_offsets[j]);
                if (bh_backup) {
                    memcpy(bh_backup->b_data, bh->b_data, SIMPLEFS_SECTOR_SIZE);
                    mark_buffer_dirty(bh_backup);
                    sync_dirty_buffer(bh_backup);
                    brelse(bh_backup);
                }
            }
        }
        pr_info("simplefs: Auto-formatting complete.\n");
    }

    disk_sb = (struct simplefs_super_block *)bh->b_data;
    sbi = kmalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
    if (!sbi) {
        brelse(bh);
        return -ENOMEM;
    }
    sbi->total_files = disk_sb->total_files;
    sb->s_fs_info = sbi;
    brelse(bh);

    root_inode = new_inode(sb);
    if (!root_inode) {
        kfree(sbi);
        return -ENOMEM;
    }

    root_inode->i_ino = 1;
    root_inode->i_sb = sb;
    root_inode->i_op = &simplefs_inode_ops;
    root_inode->i_fop = &simplefs_dir_ops;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_uid = GLOBAL_ROOT_UID;
    root_inode->i_gid = GLOBAL_ROOT_GID;
    sb->s_root = d_make_root(root_inode);

    if (!sb->s_root) {
        kfree(sbi);
        return -ENOMEM;
    }

    return 0;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type, int flags,
                                    const char *dev_name, void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static struct file_system_type simplefs_fs_type = {
    .owner   = THIS_MODULE,
    .name    = "simplefs",
    .mount   = simplefs_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static ssize_t simplefs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    size_t max_size = max_file_sectors * SIMPLEFS_SECTOR_SIZE;

    if (*ppos >= max_size) return 0;
    if (*ppos + len > max_size) len = max_size - *ppos;

    unsigned int sector_offset_in_file = *ppos / SIMPLEFS_SECTOR_SIZE;
    unsigned int byte_offset_in_sector = *ppos % SIMPLEFS_SECTOR_SIZE;

    sector_t phys_sector = sfs_ino_to_base_sector(inode->i_ino);

    unsigned int i;
    for (i = 0; i <= sector_offset_in_file; i++) {
        phys_sector = sfs_bypass_superblocks(phys_sector);
        if (i < sector_offset_in_file) {
            phys_sector++;
        }
    }

    bh = sb_bread(sb, phys_sector);
    if (!bh) return -EIO;

    size_t chunk = SIMPLEFS_SECTOR_SIZE - byte_offset_in_sector;
    if (chunk > len) chunk = len;

    if (copy_to_user(buf, bh->b_data + byte_offset_in_sector, chunk)) {
        brelse(bh);
        return -EFAULT;
    }

    brelse(bh);
    *ppos += chunk;
    return chunk;
}

static ssize_t simplefs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    size_t max_size = max_file_sectors * SIMPLEFS_SECTOR_SIZE;

    if (*ppos >= max_size) return -ENOSPC;
    if (*ppos + len > max_size) len = max_size - *ppos;

    unsigned int sector_offset_in_file = *ppos / SIMPLEFS_SECTOR_SIZE;
    unsigned int byte_offset_in_sector = *ppos % SIMPLEFS_SECTOR_SIZE;

    sector_t phys_sector = sfs_ino_to_base_sector(inode->i_ino);

    unsigned int i;
    for (i = 0; i <= sector_offset_in_file; i++) {
        phys_sector = sfs_bypass_superblocks(phys_sector);
        if (i < sector_offset_in_file) {
            phys_sector++;
        }
    }

    bh = sb_bread(sb, phys_sector);
    if (!bh) return -EIO;

    size_t chunk = SIMPLEFS_SECTOR_SIZE - byte_offset_in_sector;
    if (chunk > len) chunk = len;

    if (copy_from_user(bh->b_data + byte_offset_in_sector, buf, chunk)) {
        brelse(bh);
        return -EFAULT;
    }

    mark_buffer_dirty(bh);
    brelse(bh);

    *ppos += chunk;
    if (*ppos > inode->i_size) {
        inode->i_size = *ppos;
    }

    return chunk;
}

static int simplefs_iterate(struct file *filp, struct dir_context *ctx)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    char *name_buf;
    uint32_t i;

    if (!dir_emit_dots(filp, ctx))
        return 0;

    if (ctx->pos >= sbi->total_files + 2)
        return 0;

    name_buf = kmalloc(max_name_len + 1, GFP_KERNEL);
    if (!name_buf)
        return -ENOMEM;

    for (i = ctx->pos - 2; i < sbi->total_files; i++) {
        int len = snprintf(name_buf, max_name_len + 1, "file_%u", i);
        if (!dir_emit(ctx, name_buf, len, i + 2, DT_REG))
            break;
            
        ctx->pos++;
    }

    kfree(name_buf);
    return 0;
}

static long simplefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    uint32_t total_files;
    sector_t i;
    int ret = 0;

    if (!sbi) return -EIO;
    total_files = sbi->total_files;

    switch(cmd) {
        case SIMPLEFS_IOC_RESET_FILES: {
            pr_info("simplefs ioctl: zeroing all data files block-by-block\n");
            
            for (i = 0; i < total_files; i++) {
                unsigned long target_ino = i + 2;
                sector_t phys_sector = sfs_ino_to_base_sector(target_ino);
                unsigned int s;

                for (s = 0; s < max_file_sectors; s++) {
                    phys_sector = sfs_bypass_superblocks(phys_sector);
                    
                    ret = write_zero_sector(sb, phys_sector);
                    if (ret) return ret;
                    
                    phys_sector++;
                }
            }
            pr_info("simplefs ioctl: all data files have been reset\n");
            return 0;
        }

        case SIMPLEFS_IOC_WIPE_FS: {
            pr_info("simplefs ioctl: wiping filesystem superblocks\n");
            
            ret = write_zero_sector(sb, sb_offsets[0]);
            if (ret) return ret;

            if (num_sb_offsets > 1) {
                int j;
                for (j = 1; j < num_sb_offsets; j++) {
                    ret = write_zero_sector(sb, sb_offsets[j]);
                    if (ret) return ret;
                }
            }
            pr_info("simplefs ioctl: superblocks wiped completely\n");
            return 0;
        }

        case SIMPLEFS_IOC_GET_HASHES: {
            uint32_t *user_hashes_ptr = (uint32_t *)arg;
            uint32_t *kernel_hashes = kmalloc_array(total_files, sizeof(uint32_t), GFP_KERNEL);
            if (!kernel_hashes) return -ENOMEM;

            pr_info("simplefs ioctl: calculating cumulative CRC32 for all files\n");

            for (i = 0; i < total_files; i++) {
                uint32_t file_crc = ~0U;
                unsigned long target_ino = i + 2;
                sector_t phys_sector = sfs_ino_to_base_sector(target_ino);
                unsigned int s;

                for (s = 0; s < max_file_sectors; s++) {
                    phys_sector = sfs_bypass_superblocks(phys_sector);
                    
                    struct buffer_head *bh = sb_bread(sb, phys_sector);
                    if (bh) {
                        file_crc = crc32_le(file_crc, (unsigned char *)bh->b_data, SIMPLEFS_SECTOR_SIZE);
                        brelse(bh);
                    }
                    phys_sector++;
                }
                kernel_hashes[i] = file_crc ^ ~0U;
            }

            if (copy_to_user(user_hashes_ptr, kernel_hashes, total_files * sizeof(uint32_t))) {
                kfree(kernel_hashes);
                return -EFAULT;
            }
            kfree(kernel_hashes);
            return 0;
        }

        case SIMPLEFS_IOC_GET_MAPPING: {
            struct simplefs_mapping_args uargs;
            struct fd f;
            struct inode *target_inode;
            uint64_t *k_sectors;
            uint32_t count;

            if (copy_from_user(&uargs, (void __user *)arg, sizeof(struct simplefs_mapping_args)))
                return -EFAULT;

            if (uargs.max_sectors == 0 || !uargs.sectors)
                return -EINVAL;

            f = fdget(uargs.fd);
            if (fd_empty(f)) return -EBADF;

            if (fd_file(f)->f_path.dentry->d_sb != sb) {
                fdput(f);
                return -EINVAL;
            }

            target_inode = file_inode(fd_file(f));
            count = max_file_sectors;
            if (count > uargs.max_sectors) count = uargs.max_sectors;

            k_sectors = kmalloc_array(count, sizeof(uint64_t), GFP_KERNEL);
            if (!k_sectors) {
                fdput(f);
                return -ENOMEM;
            }

            sector_t curr_sector = sfs_ino_to_base_sector(target_inode->i_ino);
            for (i = 0; i < count; i++) {
                curr_sector = sfs_bypass_superblocks(curr_sector);
                k_sectors[i] = (uint64_t)curr_sector;
                curr_sector++; 
            }

            uargs.num_sectors = count;

            if (copy_to_user(uargs.sectors, k_sectors, count * sizeof(uint64_t)) ||
                copy_to_user((void __user *)arg, &uargs, sizeof(struct simplefs_mapping_args))) {
                kfree(k_sectors);
                fdput(f);
                return -EFAULT;
            }

            kfree(k_sectors);
            fdput(f);
            return 0;
        }

        default:
            return -ENOTTY;
    }
}


static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode = NULL;
    long idx;

    if (dentry->d_name.len > max_name_len)
        return ERR_PTR(-ENAMETOOLONG);

    if (sscanf(dentry->d_name.name, "file_%lu", &idx) == 1) {
        if (idx >= 0 && idx < sbi->total_files) {
            inode = new_inode(sb);
            if (!inode) return ERR_PTR(-ENOMEM);

            inode->i_ino = idx + 2; 
            inode->i_sb = sb;
            inode->i_mode = S_IFREG | 0644;
            inode->i_fop = &simplefs_file_ops;
            inode->i_size = max_file_sectors * SIMPLEFS_SECTOR_SIZE;
            
            inode->i_uid = GLOBAL_ROOT_UID;
            inode->i_gid = GLOBAL_ROOT_GID;
            insert_inode_hash(inode);
        }
    }
    return d_splice_alias(inode, dentry);
}

static const struct file_operations simplefs_file_ops = {
    .read           = simplefs_read,
    .write          = simplefs_write,
    .unlocked_ioctl = simplefs_ioctl,
};

static const struct file_operations simplefs_dir_ops = {
    .iterate_shared = simplefs_iterate,
    .read           = generic_read_dir,
    .unlocked_ioctl = simplefs_ioctl,
};

static const struct inode_operations simplefs_inode_ops = {
    .lookup = simplefs_lookup,
};

static void simplefs_put_super(struct super_block *sb)
{
    if (sb->s_fs_info) {
        kfree(sb->s_fs_info);
        sb->s_fs_info = NULL;
    }
}

static const struct super_operations simplefs_sops = {
    .drop_inode     = generic_delete_inode,
    .put_super      = simplefs_put_super,
};

static int __init simplefs_init(void)
{
    return register_filesystem(&simplefs_fs_type);
}

static void __exit simplefs_exit(void)
{
    unregister_filesystem(&simplefs_fs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);
