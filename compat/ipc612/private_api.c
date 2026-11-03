// SPDX-License-Identifier: GPL-2.0
#define ACE5_PRIVATE_IMPLEMENTATION
#include "private_api.h"
#include <linux/module.h>
#include <linux/version.h>
#include "sheng_hmos_api_v2.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0) || \
    LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#error "Harmony IPC API v2 requires Linux 6.12"
#endif

typeof(__list_lru_init) *ace5_api___list_lru_init;
typeof(__wake_up_pollfree) *ace5_api___wake_up_pollfree;
typeof(file_close_fd) *ace5_api_file_close_fd;
typeof(has_capability_noaudit) *ace5_api_has_capability_noaudit;
typeof(list_lru_add) *ace5_api_list_lru_add;
typeof(list_lru_count_node) *ace5_api_list_lru_count_node;
typeof(list_lru_del) *ace5_api_list_lru_del;
typeof(list_lru_destroy) *ace5_api_list_lru_destroy;
typeof(list_lru_isolate) *ace5_api_list_lru_isolate;
typeof(list_lru_walk_node) *ace5_api_list_lru_walk_node;
typeof(mmput_async) *ace5_api_mmput_async;
typeof(security_binder_set_context_mgr) *ace5_api_security_binder_set_context_mgr;
typeof(security_binder_transaction) *ace5_api_security_binder_transaction;
typeof(security_binder_transfer_binder) *ace5_api_security_binder_transfer_binder;
typeof(security_binder_transfer_file) *ace5_api_security_binder_transfer_file;
typeof(security_cred_getsecid) *ace5_api_security_cred_getsecid;
typeof(security_release_secctx) *ace5_api_security_release_secctx;
typeof(security_secid_to_secctx) *ace5_api_security_secid_to_secctx;
typeof(task_work_add) *ace5_api_task_work_add;
typeof(zap_page_range_single) *ace5_api_zap_page_range_single;


/* 校验 Linux 6.12 接口后再发布函数指针 */
int ace5_private_api_init(void)
{
    const struct sheng_hmos_ipc_api_v2 *api;

    api = __symbol_get("sheng_hmos_ipc_api_v2");
    if (!api) {
        pr_err("SHENG_IPC missing kernel API v2\n");
        return -ENOENT;
    }

    if (api->magic != SHENG_HMOS_API_MAGIC || api->version != 2 ||
        api->size != sizeof(*api))
        goto invalid;

    if (!api->api___list_lru_init)
        goto invalid;
    if (!api->api___wake_up_pollfree)
        goto invalid;
    if (!api->api_file_close_fd)
        goto invalid;
    if (!api->api_has_capability_noaudit)
        goto invalid;
    if (!api->api_list_lru_add)
        goto invalid;
    if (!api->api_list_lru_count_node)
        goto invalid;
    if (!api->api_list_lru_del)
        goto invalid;
    if (!api->api_list_lru_destroy)
        goto invalid;
    if (!api->api_list_lru_isolate)
        goto invalid;
    if (!api->api_list_lru_walk_node)
        goto invalid;
    if (!api->api_mmput_async)
        goto invalid;
    if (!api->api_security_binder_set_context_mgr)
        goto invalid;
    if (!api->api_security_binder_transaction)
        goto invalid;
    if (!api->api_security_binder_transfer_binder)
        goto invalid;
    if (!api->api_security_binder_transfer_file)
        goto invalid;
    if (!api->api_security_cred_getsecid)
        goto invalid;
    if (!api->api_security_release_secctx)
        goto invalid;
    if (!api->api_security_secid_to_secctx)
        goto invalid;
    if (!api->api_task_work_add)
        goto invalid;
    if (!api->api_zap_page_range_single)
        goto invalid;

    ace5_api___list_lru_init = api->api___list_lru_init;
    ace5_api___wake_up_pollfree = api->api___wake_up_pollfree;
    ace5_api_file_close_fd = api->api_file_close_fd;
    ace5_api_has_capability_noaudit = api->api_has_capability_noaudit;
    ace5_api_list_lru_add = api->api_list_lru_add;
    ace5_api_list_lru_count_node = api->api_list_lru_count_node;
    ace5_api_list_lru_del = api->api_list_lru_del;
    ace5_api_list_lru_destroy = api->api_list_lru_destroy;
    ace5_api_list_lru_isolate = api->api_list_lru_isolate;
    ace5_api_list_lru_walk_node = api->api_list_lru_walk_node;
    ace5_api_mmput_async = api->api_mmput_async;
    ace5_api_security_binder_set_context_mgr = api->api_security_binder_set_context_mgr;
    ace5_api_security_binder_transaction = api->api_security_binder_transaction;
    ace5_api_security_binder_transfer_binder = api->api_security_binder_transfer_binder;
    ace5_api_security_binder_transfer_file = api->api_security_binder_transfer_file;
    ace5_api_security_cred_getsecid = api->api_security_cred_getsecid;
    ace5_api_security_release_secctx = api->api_security_release_secctx;
    ace5_api_security_secid_to_secctx = api->api_security_secid_to_secctx;
    ace5_api_task_work_add = api->api_task_work_add;
    ace5_api_zap_page_range_single = api->api_zap_page_range_single;

    return 0;

invalid:
    __symbol_put("sheng_hmos_ipc_api_v2");
    pr_err("SHENG_IPC kernel API v2 layout or entry mismatch\n");
    return -ELIBBAD;
}


/* 为头文件内联函数提供相同类型的入口 */
void __wake_up_pollfree(struct wait_queue_head *head)
{
    ace5_api___wake_up_pollfree(head);
}


unsigned long list_lru_count_node(struct list_lru *lru, int nid)
{
    return ace5_api_list_lru_count_node(lru, nid);
}


unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
    list_lru_walk_cb isolate, void *arg, unsigned long *nr)
{
    return ace5_api_list_lru_walk_node(lru, nid, isolate, arg, nr);
}
