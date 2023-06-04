// SPDX-License-Identifier: GPL-2.0
/*
 * High-level sync()-related operations
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/namei.h>
#include <linux/sched/xacct.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/backing-dev.h>
#include <linux/version.h>
#include "internal.h"

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)

static inline int sec_sys_sync(void) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
	ksys_sync();
	return 0;
#else
	return sys_sync();
#endif
}

/* Interruptible sync for Samsung Mobile Device */
/* @fs.sec -- 30cbf83784121f91517b701d9706bccd -- */
#ifdef CONFIG_INTERRUPTIBLE_SYNC

#include <linux/workqueue.h>
#include <linux/suspend.h>
#include <linux/delay.h>

//#define CONFIG_INTR_SYNC_DEBUG

#ifdef CONFIG_INTR_SYNC_DEBUG
#define dbg_print	printk
#else
#define dbg_print(...)
#endif

enum {
	INTR_SYNC_STATE_IDLE = 0,
	INTR_SYNC_STATE_QUEUED,
	INTR_SYNC_STATE_RUNNING,
	INTR_SYNC_STATE_MAX
};

struct interruptible_sync_work {
	int id;
	int ret;
	unsigned int waiter;
	unsigned int state;
	unsigned long version;
	spinlock_t lock;
	struct completion done;
	struct work_struct work;
};

/* Initially, intr_sync_work has zero pending */
static struct interruptible_sync_work intr_sync_work[2];

/* Last work start time */
static atomic_t running_work_idx;

/* intr_sync_wq will be created when intr_sync() is called at first time.
 * And it is alive till system shutdown */
static struct workqueue_struct *intr_sync_wq;

/* It prevents double allocation of intr_sync_wq */
static DEFINE_MUTEX(intr_sync_wq_lock);

static inline struct interruptible_sync_work *INTR_SYNC_WORK(struct work_struct *work)
{
	return container_of(work, struct interruptible_sync_work, work);
}

static void do_intr_sync(struct work_struct *work)
{
	struct interruptible_sync_work *sync_work = INTR_SYNC_WORK(work);
	int ret = 0;
	unsigned int waiter;

	spin_lock(&sync_work->lock);
	atomic_set(&running_work_idx, sync_work->id);
	sync_work->state = INTR_SYNC_STATE_RUNNING;
	waiter = sync_work->waiter;
	spin_unlock(&sync_work->lock);

	dbg_print("\nintr_sync: %s: call sys_sync on work[%d]-%ld\n",
			__func__, sync_work->id, sync_work->version);

	/* if no one waits, do not call sync() */
	if (waiter) {
		ret = sec_sys_sync();
		dbg_print("\nintr_sync: %s: done sys_sync on work[%d]-%ld\n",
			__func__, sync_work->id, sync_work->version);
	} else {
		dbg_print("\nintr_sync: %s: cancel,no_wait on work[%d]-%ld\n",
			__func__, sync_work->id, sync_work->version);
	}

	spin_lock(&sync_work->lock);
	sync_work->version++;
	sync_work->ret = ret;
	sync_work->state = INTR_SYNC_STATE_IDLE;
	complete_all(&sync_work->done);
	spin_unlock(&sync_work->lock);
}

/* wakeup functions that depend on PM facilities
 *
 * struct intr_wakeup_data  : wrapper structure for variables for PM
 *			      each thread has own instance of it
 * __prepare_wakeup_event() : prepare and check intr_wakeup_data
 * __check_wakeup_event()   : check wakeup-event with intr_wakeup_data
 */
struct intr_wakeup_data {
	unsigned int cnt;
};

static inline int __prepare_wakeup_event(struct intr_wakeup_data *wd)
{
	if (pm_get_wakeup_count(&wd->cnt, false))
		return 0;

	pr_info("intr_sync: detected wakeup events before sync\n");
	pm_print_active_wakeup_sources();
	return -EBUSY;
}

static inline  int __check_wakeup_event(struct intr_wakeup_data *wd)
{
	unsigned int cnt, no_inpr;

	no_inpr = pm_get_wakeup_count(&cnt, false);
	if (no_inpr && (cnt == wd->cnt))
		return 0;

	pr_info("intr_sync: detected wakeup events(no_inpr: %u cnt: %u->%u)\n",
		no_inpr, wd->cnt, cnt);
	pm_print_active_wakeup_sources();
	return -EBUSY;
}

/* Interruptible Sync
 *
 * intr_sync() is same function as sys_sync() except that it can wakeup.
 * It's possible because of inter_syncd workqueue.
 *
 * If system gets wakeup event while sync_work is running,
 * just return -EBUSY, otherwise 0.
 *
 * If intr_sync() is called again while sync_work is running, it will enqueue
 * idle sync_work to work_queue and wait the completion of it.
 * If there is not idle sync_work but queued one, it just increases waiter by 1,
 * and waits the completion of queued sync_work.
 *
 * If you want to know returned value of sys_sync(),
 * you can get it from the argument, sync_ret
 */

int intr_sync(int *sync_ret)
{
	int ret;
enqueue_sync_wait:
	/* If the workqueue exists, try to enqueue work and wait */
	if (likely(intr_sync_wq)) {
		struct interruptible_sync_work *sync_work;
		struct intr_wakeup_data wd;
		int work_idx;
		int work_ver;
find_idle:
		work_idx = !atomic_read(&running_work_idx);
		sync_work = &intr_sync_work[work_idx];

		/* Prepare intr_wakeup_data and check wakeup event:
		 * If a wakeup-event is detected, wake up right now
		 */
		if (__prepare_wakeup_event(&wd)) {
			dbg_print("intr_sync: detect wakeup event "
				"before waiting work[%d]\n", work_idx);
			return -EBUSY;
		}

		dbg_print("\nintr_sync: try to wait work[%d]\n", work_idx);

		spin_lock(&sync_work->lock);
		work_ver = sync_work->version;
		if (sync_work->state == INTR_SYNC_STATE_RUNNING) {
			spin_unlock(&sync_work->lock);
			dbg_print("intr_sync: work[%d] is already running, "
				"find idle work\n", work_idx);
			goto find_idle;
		}

		sync_work->waiter++;
		if (sync_work->state == INTR_SYNC_STATE_IDLE) {
			dbg_print("intr_sync: enqueue work[%d]\n", work_idx);
			sync_work->state = INTR_SYNC_STATE_QUEUED;
			reinit_completion(&sync_work->done);
			queue_work(intr_sync_wq, &sync_work->work);
		}
		spin_unlock(&sync_work->lock);

		do {
			/* Check wakeup event first before waiting:
			 * If a wakeup-event is detected, wake up right now
			 */
			if  (__check_wakeup_event(&wd)) {
				spin_lock(&sync_work->lock);
				sync_work->waiter--;
				spin_unlock(&sync_work->lock);
				dbg_print("intr_sync: detect wakeup event "
					"while waiting work[%d]\n", work_idx);
				return -EBUSY;
			}

//			dbg_print("intr_sync: waiting work[%d]\n", work_idx);
			/* Return 0 if timed out, or positive if completed. */
			ret = wait_for_completion_io_timeout(
					&sync_work->done, HZ/10);
			/* A work that we are waiting for has done. */
			if ((ret > 0) || (sync_work->version != work_ver))
				break;
//			dbg_print("intr_sync: timeout work[%d]\n", work_idx);
		} while (1);

		spin_lock(&sync_work->lock);
		sync_work->waiter--;
		if (sync_ret)
			*sync_ret = sync_work->ret;
		spin_unlock(&sync_work->lock);
		dbg_print("intr_sync: sync work[%d] is done with ret(%d)\n",
				work_idx, sync_work->ret);
		return 0;
	}

	/* check whether a workqueue exists or not under locked state.
	 * Create new one if a workqueue is not created yet.
	 */
	mutex_lock(&intr_sync_wq_lock);
	if (likely(!intr_sync_wq)) {
		intr_sync_work[0].id = 0;
		intr_sync_work[1].id = 1;
		INIT_WORK(&intr_sync_work[0].work, do_intr_sync);
		INIT_WORK(&intr_sync_work[1].work, do_intr_sync);
		spin_lock_init(&intr_sync_work[0].lock);
		spin_lock_init(&intr_sync_work[1].lock);
		init_completion(&intr_sync_work[0].done);
		init_completion(&intr_sync_work[1].done);
		intr_sync_wq = alloc_ordered_workqueue("intr_syncd", WQ_MEM_RECLAIM);
		dbg_print("\nintr_sync: try to allocate intr_sync_queue\n");
	}
	mutex_unlock(&intr_sync_wq_lock);

	/* try to enqueue work again if the workqueue is created successfully */
	if (likely(intr_sync_wq))
		goto enqueue_sync_wait;

	printk("\nintr_sync: allocation failed, just call sync()\n");
	ret = sec_sys_sync();
	if (sync_ret)
		*sync_ret = ret;
	return 0;
}
#else /* CONFIG_INTERRUPTIBLE_SYNC */
int intr_sync(int *sync_ret)
{
	int ret = sec_sys_sync();
	if (sync_ret)
		*sync_ret = ret;
	return 0;
}
#endif /* CONFIG_INTERRUPTIBLE_SYNC */

/*
 * Do the filesystem syncing work. For simple filesystems
 * writeback_inodes_sb(sb) just dirties buffers with inodes so we have to
 * submit IO for these buffers via __sync_blockdev(). This also speeds up the
 * wait == 1 case since in that case write_inode() functions do
 * sync_dirty_buffer() and thus effectively write one block at a time.
 */
static int __sync_filesystem(struct super_block *sb, int wait)
{
	if (wait)
		sync_inodes_sb(sb);
	else
		writeback_inodes_sb(sb, WB_REASON_SYNC);

	if (sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, wait);
	return __sync_blockdev(sb->s_bdev, wait);
}

/*
 * Write out and wait upon all dirty data associated with this
 * superblock.  Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int sync_filesystem(struct super_block *sb)
{
	int ret;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/*
	 * No point in syncing out anything if the filesystem is read-only.
	 */
	if (sb_rdonly(sb))
		return 0;

	ret = __sync_filesystem(sb, 0);
	if (ret < 0)
		return ret;
	return __sync_filesystem(sb, 1);
}
EXPORT_SYMBOL(sync_filesystem);

static void sync_inodes_one_sb(struct super_block *sb, void *arg)
{
	if (!sb_rdonly(sb))
		sync_inodes_sb(sb);
}

static void sync_fs_one_sb(struct super_block *sb, void *arg)
{
	if (!sb_rdonly(sb) && sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, *(int *)arg);
}

static void fdatawrite_one_bdev(struct block_device *bdev, void *arg)
{
	filemap_fdatawrite(bdev->bd_inode->i_mapping);
}

static void fdatawait_one_bdev(struct block_device *bdev, void *arg)
{
	/*
	 * We keep the error status of individual mapping so that
	 * applications can catch the writeback error using fsync(2).
	 * See filemap_fdatawait_keep_errors() for details.
	 */
	filemap_fdatawait_keep_errors(bdev->bd_inode->i_mapping);
}

/*
 * Sync everything. We start by waking flusher threads so that most of
 * writeback runs on all devices in parallel. Then we sync all inodes reliably
 * which effectively also waits for all flusher threads to finish doing
 * writeback. At this point all data is on disk so metadata should be stable
 * and we tell filesystems to sync their metadata via ->sync_fs() calls.
 * Finally, we writeout all block devices because some filesystems (e.g. ext2)
 * just write metadata (such as inodes or bitmaps) to block device page cache
 * and do not sync it on their own in ->sync_fs().
 */
void ksys_sync(void)
{
	int nowait = 0, wait = 1;

	wakeup_flusher_threads(WB_REASON_SYNC);
	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &wait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	iterate_bdevs(fdatawait_one_bdev, NULL);
	if (unlikely(laptop_mode))
		laptop_sync_completion();
}

SYSCALL_DEFINE0(sync)
{
	ksys_sync();
	return 0;
}

static void do_sync_work(struct work_struct *work)
{
	int nowait = 0;

	/*
	 * Sync twice to reduce the possibility we skipped some inodes / pages
	 * because they were temporarily locked
	 */
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	printk("Emergency Sync complete\n");
	kfree(work);
}

void emergency_sync(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_sync_work);
		schedule_work(work);
	}
}

/*
 * sync a single super
 */
SYSCALL_DEFINE1(syncfs, int, fd)
{
	struct fd f = fdget(fd);
	struct super_block *sb;
	int ret;

	if (!f.file)
		return -EBADF;
	sb = f.file->f_path.dentry->d_sb;

	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);

	fdput(f);
	return ret;
}

/**
 * vfs_fsync_range - helper to sync a range of data & metadata to disk
 * @file:		file to sync
 * @start:		offset in bytes of the beginning of data range to sync
 * @end:		offset in bytes of the end of data range (inclusive)
 * @datasync:		perform only datasync
 *
 * Write back data in range @start..@end and metadata for @file to disk.  If
 * @datasync is set only metadata needed to access modified file data is
 * written.
 */
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;

	if (!file->f_op->fsync)
		return -EINVAL;
	if (!datasync && (inode->i_state & I_DIRTY_TIME))
		mark_inode_dirty_sync(inode);
	return file->f_op->fsync(file, start, end, datasync);
}
EXPORT_SYMBOL(vfs_fsync_range);

/**
 * vfs_fsync - perform a fsync or fdatasync on a file
 * @file:		file to sync
 * @datasync:		only perform a fdatasync operation
 *
 * Write back data and metadata for @file to disk.  If @datasync is
 * set only metadata needed to access modified file data is written.
 */
int vfs_fsync(struct file *file, int datasync)
{
	return vfs_fsync_range(file, 0, LLONG_MAX, datasync);
}
EXPORT_SYMBOL(vfs_fsync);

static int do_fsync(unsigned int fd, int datasync)
{
	struct fd f = fdget(fd);
	int ret = -EBADF;

	if (f.file) {
		ret = vfs_fsync(f.file, datasync);
		fdput(f);
		inc_syscfs(current);
	}
	return ret;
}

SYSCALL_DEFINE1(fsync, unsigned int, fd)
{
	return do_fsync(fd, 0);
}

SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
{
	return do_fsync(fd, 1);
}

int sync_file_range(struct file *file, loff_t offset, loff_t nbytes,
		    unsigned int flags)
{
	int ret;
	struct address_space *mapping;
	loff_t endbyte;			/* inclusive */
	umode_t i_mode;

	ret = -EINVAL;
	if (flags & ~VALID_FLAGS)
		goto out;

	endbyte = offset + nbytes;

	if ((s64)offset < 0)
		goto out;
	if ((s64)endbyte < 0)
		goto out;
	if (endbyte < offset)
		goto out;

	if (sizeof(pgoff_t) == 4) {
		if (offset >= (0x100000000ULL << PAGE_SHIFT)) {
			/*
			 * The range starts outside a 32 bit machine's
			 * pagecache addressing capabilities.  Let it "succeed"
			 */
			ret = 0;
			goto out;
		}
		if (endbyte >= (0x100000000ULL << PAGE_SHIFT)) {
			/*
			 * Out to EOF
			 */
			nbytes = 0;
		}
	}

	if (nbytes == 0)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */

	i_mode = file_inode(file)->i_mode;
	ret = -ESPIPE;
	if (!S_ISREG(i_mode) && !S_ISBLK(i_mode) && !S_ISDIR(i_mode) &&
			!S_ISLNK(i_mode))
		goto out;

	mapping = file->f_mapping;
	ret = 0;
	if (flags & SYNC_FILE_RANGE_WAIT_BEFORE) {
		ret = file_fdatawait_range(file, offset, endbyte);
		if (ret < 0)
			goto out;
	}

	if (flags & SYNC_FILE_RANGE_WRITE) {
		int sync_mode = WB_SYNC_NONE;

		if ((flags & SYNC_FILE_RANGE_WRITE_AND_WAIT) ==
			     SYNC_FILE_RANGE_WRITE_AND_WAIT)
			sync_mode = WB_SYNC_ALL;

		ret = __filemap_fdatawrite_range(mapping, offset, endbyte,
						 sync_mode);
		if (ret < 0)
			goto out;
	}

	if (flags & SYNC_FILE_RANGE_WAIT_AFTER)
		ret = file_fdatawait_range(file, offset, endbyte);

out:
	return ret;
}

/*
 * ksys_sync_file_range() permits finely controlled syncing over a segment of
 * a file in the range offset .. (offset+nbytes-1) inclusive.  If nbytes is
 * zero then ksys_sync_file_range() will operate from offset out to EOF.
 *
 * The flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE: wait upon writeout of all pages in the range
 * before performing the write.
 *
 * SYNC_FILE_RANGE_WRITE: initiate writeout of all those dirty pages in the
 * range which are not presently under writeback. Note that this may block for
 * significant periods due to exhaustion of disk request structures.
 *
 * SYNC_FILE_RANGE_WAIT_AFTER: wait upon writeout of all pages in the range
 * after performing the write.
 *
 * Useful combinations of the flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE: ensures that all pages
 * in the range which were dirty on entry to ksys_sync_file_range() are placed
 * under writeout.  This is a start-write-for-data-integrity operation.
 *
 * SYNC_FILE_RANGE_WRITE: start writeout of all dirty pages in the range which
 * are not presently under writeout.  This is an asynchronous flush-to-disk
 * operation.  Not suitable for data integrity operations.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE (or SYNC_FILE_RANGE_WAIT_AFTER): wait for
 * completion of writeout of all pages in the range.  This will be used after an
 * earlier SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE operation to wait
 * for that operation to complete and to return the result.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER
 * (a.k.a. SYNC_FILE_RANGE_WRITE_AND_WAIT):
 * a traditional sync() operation.  This is a write-for-data-integrity operation
 * which will ensure that all pages in the range which were dirty on entry to
 * ksys_sync_file_range() are written to disk.  It should be noted that disk
 * caches are not flushed by this call, so there are no guarantees here that the
 * data will be available on disk after a crash.
 *
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE and SYNC_FILE_RANGE_WAIT_AFTER will detect any
 * I/O errors or ENOSPC conditions and will return those to the caller, after
 * clearing the EIO and ENOSPC flags in the address_space.
 *
 * It should be noted that none of these operations write out the file's
 * metadata.  So unless the application is strictly performing overwrites of
 * already-instantiated disk blocks, there are no guarantees here that the data
 * will be available after a crash.
 */
int ksys_sync_file_range(int fd, loff_t offset, loff_t nbytes,
			 unsigned int flags)
{
	int ret;
	struct fd f;

	ret = -EBADF;
	f = fdget(fd);
	if (f.file)
		ret = sync_file_range(f.file, offset, nbytes, flags);

	fdput(f);
	return ret;
}

SYSCALL_DEFINE4(sync_file_range, int, fd, loff_t, offset, loff_t, nbytes,
				unsigned int, flags)
{
	return ksys_sync_file_range(fd, offset, nbytes, flags);
}

/* It would be nice if people remember that not all the world's an i386
   when they introduce new system calls */
SYSCALL_DEFINE4(sync_file_range2, int, fd, unsigned int, flags,
				 loff_t, offset, loff_t, nbytes)
{
	return ksys_sync_file_range(fd, offset, nbytes, flags);
}
