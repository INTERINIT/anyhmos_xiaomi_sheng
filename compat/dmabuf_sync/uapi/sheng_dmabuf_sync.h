/* SPDX-License-Identifier: MIT */
#ifndef SHENG_DMABUF_SYNC_UAPI_H
#define SHENG_DMABUF_SYNC_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SHENG_DMABUF_SYNC_DEVICE "/dev/sheng_dmabuf_sync"
#define SHENG_DMABUF_SYNC_VERSION 1U
#define SHENG_DMABUF_SYNC_FEATURE_EXPORT (1U << 0)
#define SHENG_DMABUF_SYNC_FEATURE_IMPORT (1U << 1)
#define SHENG_DMABUF_SYNC_FEATURES_REQUIRED \
   (SHENG_DMABUF_SYNC_FEATURE_EXPORT | SHENG_DMABUF_SYNC_FEATURE_IMPORT)
#define SHENG_DMABUF_SYNC_READ (1U << 0)
#define SHENG_DMABUF_SYNC_WRITE (1U << 1)
#define SHENG_DMABUF_SYNC_VALID_FLAGS \
   (SHENG_DMABUF_SYNC_READ | SHENG_DMABUF_SYNC_WRITE)

/* 查询版本及已实现的同步文件能力 */
struct sheng_dmabuf_sync_abi {
   __u32 version;
   __u32 features;
   __u64 reserved;
};

/* 导出描述符归调用者所有，导入仅借用描述符 */
struct sheng_dmabuf_sync_file {
   __s32 dma_buf_fd;
   __u32 flags;
   __s32 fence_fd;
   __u32 reserved;
};

#define SHENG_DMABUF_SYNC_QUERY \
   _IOR('S', 0, struct sheng_dmabuf_sync_abi)
#define SHENG_DMABUF_SYNC_EXPORT \
   _IOWR('S', 1, struct sheng_dmabuf_sync_file)
#define SHENG_DMABUF_SYNC_IMPORT \
   _IOW('S', 2, struct sheng_dmabuf_sync_file)

#endif
