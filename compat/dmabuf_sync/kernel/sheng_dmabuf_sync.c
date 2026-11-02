// SPDX-License-Identifier: GPL-2.0-only
#ifdef SHENG_SYNC_HOST_TEST
#include "bridge_host_shim.h"
#else
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-resv.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#endif
#include "../uapi/sheng_dmabuf_sync.h"

#define SHENG_FENCE_LIMIT 4096U

struct sheng_fences {
	struct dma_fence **items;
	unsigned int count;
	unsigned int capacity;
};

static void sheng_put_fences(struct dma_fence **fences, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i)
		dma_fence_put(fences[i]);
	kfree(fences);
}

// 同一时间线只保留较晚栅栏，限制单次合并的内存占用
static int sheng_append_fence(struct sheng_fences *output, struct dma_fence *fence)
{
	struct dma_fence **grown;
	unsigned int i, capacity;

	if (dma_fence_is_signaled(fence))
		return 0;
	for (i = 0; i < output->count; ++i) {
		if (output->items[i]->context != fence->context)
			continue;
		if (dma_fence_is_later(fence, output->items[i])) {
			dma_fence_put(output->items[i]);
			output->items[i] = dma_fence_get(fence);
		}
		return 0;
	}
	if (output->count == SHENG_FENCE_LIMIT)
		return -E2BIG;
	if (output->count == output->capacity) {
		capacity = output->capacity ? output->capacity * 2 : 8;
		grown = krealloc(output->items, capacity * sizeof(*grown), GFP_KERNEL);
		if (!grown)
			return -ENOMEM;
		output->items = grown;
		output->capacity = capacity;
	}
	output->items[output->count++] = dma_fence_get(fence);
	return 0;
}

// 仅展开完成条件为全部的链，外部数组保留原有条件
static int sheng_collect_head(struct sheng_fences *output, struct dma_fence *head)
{
	struct dma_fence *cursor, *contained;
	int error;

	for (cursor = dma_fence_get(head); cursor;
	     cursor = dma_fence_chain_walk(cursor)) {
		contained = dma_fence_chain_contained(cursor);
		error = sheng_append_fence(output, contained);
		if (error) {
			dma_fence_put(cursor);
			return error;
		}
	}
	return 0;
}

// 用内核链合并独立依赖，连续导入不加深数组嵌套
static struct dma_fence *sheng_merge_fences(struct dma_fence **heads,
	unsigned int count, struct dma_fence *extra)
{
	struct sheng_fences output = {0};
	struct dma_fence_chain *chain;
	struct dma_fence *result;
	unsigned int i;
	int error;

	for (i = 0; i < count; ++i) {
		error = sheng_collect_head(&output, heads[i]);
		if (error)
			goto put_output;
	}
	if (extra) {
		error = sheng_collect_head(&output, extra);
		if (error)
			goto put_output;
	}
	if (!output.count) {
		kfree(output.items);
		return dma_fence_get_stub();
	}
	result = output.items[0];
	for (i = 1; i < output.count; ++i) {
		chain = dma_fence_chain_alloc();
		if (!chain) {
			dma_fence_put(result);
			while (i < output.count)
				dma_fence_put(output.items[i++]);
			kfree(output.items);
			return ERR_PTR(-ENOMEM);
		}
		// 初始化接管两个引用，序号在本次新链内递增
		dma_fence_chain_init(chain, result, output.items[i], i);
		result = &chain->base;
	}
	kfree(output.items);
	return result;

put_output:
	sheng_put_fences(output.items, output.count);
	return ERR_PTR(error);
}

// 读取等待写者，写入等待当前全部访问者
static struct dma_fence *sheng_snapshot(struct dma_resv *reservation, bool write)
{
	struct dma_fence **shared = NULL;
	struct dma_fence *exclusive = NULL, *result;
	unsigned int count = 0;
	int error;

	error = dma_resv_get_fences(reservation, write ? NULL : &exclusive,
		&count, &shared);
	if (error) {
		sheng_put_fences(shared, count);
		dma_fence_put(exclusive);
		return ERR_PTR(error);
	}
	if (!write) {
		sheng_put_fences(shared, count);
		return exclusive ? exclusive : dma_fence_get_stub();
	}
	result = sheng_merge_fences(shared, count, NULL);
	sheng_put_fences(shared, count);
	return result;
}

static bool sheng_valid_transfer(const struct sheng_dmabuf_sync_file *request)
{
	return request->dma_buf_fd >= 0 && !request->reserved && request->flags &&
		!(request->flags & ~SHENG_DMABUF_SYNC_VALID_FLAGS);
}

static long sheng_export(void __user *user)
{
	struct sheng_dmabuf_sync_file request;
	struct dma_buf *buffer;
	struct dma_fence *fence;
	struct sync_file *sync;
	int fd, error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!sheng_valid_transfer(&request) || request.fence_fd != -1)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		error = fd;
		goto put_buffer;
	}
	fence = sheng_snapshot(buffer->resv, request.flags & SHENG_DMABUF_SYNC_WRITE);
	if (IS_ERR(fence)) {
		error = PTR_ERR(fence);
		goto put_fd;
	}
	sync = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync) {
		error = -ENOMEM;
		goto put_fd;
	}
	request.fence_fd = fd;
	// 先复制描述符再安装文件，复制失败时完整回滚
	if (copy_to_user(user, &request, sizeof(request))) {
		error = -EFAULT;
		fput(sync->file);
		goto put_fd;
	}
	fd_install(fd, sync->file);
	dma_buf_put(buffer);
	return 0;

put_fd:
	put_unused_fd(fd);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long sheng_import(void __user *user)
{
	struct sheng_dmabuf_sync_file request;
	struct dma_fence **previous = NULL;
	struct dma_fence *fence, *merged;
	struct dma_buf *buffer;
	unsigned int count = 0;
	int error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!sheng_valid_transfer(&request) || request.fence_fd < 0)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fence = sync_file_get_fence(request.fence_fd);
	if (!fence) {
		error = -EINVAL;
		goto put_buffer;
	}
	error = dma_resv_lock_interruptible(buffer->resv, NULL);
	if (error)
		goto put_fence;
	if (request.flags & SHENG_DMABUF_SYNC_WRITE) {
		// 旧版独占槽会清空读者，先合并未完成依赖再替换
		error = dma_resv_get_fences(buffer->resv, NULL, &count, &previous);
		if (!error) {
			merged = sheng_merge_fences(previous, count, fence);
			if (IS_ERR(merged)) {
				error = PTR_ERR(merged);
			} else {
				dma_resv_add_excl_fence(buffer->resv, merged);
				dma_fence_put(merged);
			}
		}
		sheng_put_fences(previous, count);
	} else {
		error = dma_resv_reserve_shared(buffer->resv, 1);
		if (!error)
			dma_resv_add_shared_fence(buffer->resv, fence);
	}
	dma_resv_unlock(buffer->resv);
put_fence:
	dma_fence_put(fence);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long sheng_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct sheng_dmabuf_sync_abi query;
	void __user *user = (void __user *)argument;

	(void)file;
	switch (command) {
	case SHENG_DMABUF_SYNC_QUERY:
		if (copy_from_user(&query, user, sizeof(query)))
			return -EFAULT;
		if (query.version || query.features || query.reserved)
			return -EINVAL;
		query.version = SHENG_DMABUF_SYNC_VERSION;
		query.features = SHENG_DMABUF_SYNC_FEATURES_REQUIRED;
		query.reserved = 0;
		return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
	case SHENG_DMABUF_SYNC_EXPORT:
		return sheng_export(user);
	case SHENG_DMABUF_SYNC_IMPORT:
		return sheng_import(user);
	default:
		return -ENOTTY;
	}
}

#ifndef SHENG_SYNC_HOST_TEST
#ifdef CONFIG_COMPAT
static long sheng_compat_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	return sheng_ioctl(file, command, (unsigned long)compat_ptr(argument));
}
#endif

static const struct file_operations sheng_operations = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = sheng_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sheng_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice sheng_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sheng_dmabuf_sync",
	.fops = &sheng_operations,
	.mode = 0600,
};

static int __init sheng_init(void)
{
	BUILD_BUG_ON(sizeof(struct sheng_dmabuf_sync_abi) != 16);
	BUILD_BUG_ON(sizeof(struct sheng_dmabuf_sync_file) != 16);
	return misc_register(&sheng_device);
}

static void __exit sheng_exit(void)
{
	misc_deregister(&sheng_device);
}

module_init(sheng_init);
module_exit(sheng_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Owned dma-buf reservation and sync-file bridge");
#endif
