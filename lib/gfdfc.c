/*
  Copyright (c) 2012-2013 DataLab, S.L. <http://www.datalab.es>

  This file is part of the DFC translator for GlusterFS.

  The DFC translator for GlusterFS is free software: you can redistribute
  it and/or modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  The DFC translator for GlusterFS is distributed in the hope that it will
  be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
  of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the DFC translator for GlusterFS. If not, see
  <http://www.gnu.org/licenses/>.
*/

#include "gfsys.h"

#include "gfdfc.h"

/*
typedef struct _dfc_queue
{
    uint64_t           next_id;
    struct list_head * requests;
    int32_t            mask;
    int32_t            pending;
} dfc_queue_t;

typedef struct _dfc_sort_data
{
    uint64_t   txn_id;
    uint16_t   server;
    uint16_t   me;
    uint32_t   count;
    uint64_t * values;
} dfc_sort_data_t;
*/

struct _dfc_sort
{
    void *  head;
    size_t  size;
    uint8_t data[4096];
};

struct _dfc_request
{
    struct list_head list;
    dfc_child_t *    child;
    call_frame_t *   frame;
    dfc_sort_t       sort;
};

struct _dfc_transaction
{
    sys_mutex_t      lock;
    struct list_head list;
    int64_t          id;
    dfc_t *          dfc;
    uint64_t         mask;
    uint32_t         state;
    dfc_sort_t       sort;
};

struct _dfc_child
{
    sys_lock_t       lock;
    struct list_head list;
    dfc_t *          dfc;
    xlator_t *       xl;
    int64_t          seq;
    int32_t          idx;
    uint32_t         count;
    uint32_t         active;
    struct list_head pool;
    dfc_sort_t *     sort;
};

struct _dfc
{
    sys_mutex_t        lock;
    uuid_t             uuid;
    xlator_t *         xl;
    loc_t              root_loc;
    int64_t            current_txn;
    int64_t            txn_mask;
    uint32_t           max_requests;
    uint32_t           requests;
    uint32_t           count;
    struct list_head   children;
    struct list_head * txns;
};

int32_t __xdata_dump(dict_t * xdata, char * key, data_t * value, void * data)
{
    logI("    %s: %u", key, value->len);

    return 0;
}

void dfc_sort_initialize(dfc_sort_t * sort)
{
    sort->head = sort->data;
    sort->size = sizeof(sort->data);
}

void dfc_sort_destroy(dfc_sort_t * sort)
{
    SYS_FREE(sort);
}

err_t dfc_sort_create(dfc_sort_t ** sort)
{
    dfc_sort_t * tmp;

    SYS_MALLOC(
        &tmp, gfdfc_mt_dfc_sort_t,
        E(),
        RETERR()
    );

    dfc_sort_initialize(tmp);

    *sort = tmp;

    return 0;
}

err_t __dfc_attach(dfc_t * dfc, int64_t id, void * data, size_t size,
                   dict_t ** xdata)
{
    SYS_CALL(
        sys_dict_set_uuid, (xdata, DFC_XATTR_UUID, dfc->uuid, NULL),
        E(),
        RETERR()
    );

    SYS_CALL(
        sys_dict_set_int64, (xdata, DFC_XATTR_ID, id, NULL),
        E(),
        RETERR()
    );

    if (data != NULL)
    {
        SYS_CALL(
            sys_dict_set_bin, (xdata, DFC_XATTR_SORT, data, size, NULL),
            E(),
            RETERR()
        );
    }
/*
    SYS_CALL(
        sys_dict_set_int64, (xdata, DFC_XATTR_TIME, time),
        E(),
        RETERR()
    );
*/
    return 0;
}

void dfc_request_destroy(dfc_request_t * req)
{
    atomic_dec(&req->child->count, memory_order_seq_cst);

    STACK_DESTROY(req->frame->root);
    SYS_FREE(req);
}

err_t dfc_request_create(dfc_child_t * child, dfc_request_t ** req)
{
    dfc_request_t * tmp;
    xlator_t * xl;
    err_t error;

    SYS_MALLOC(
        &tmp, gfdfc_mt_dfc_request_t,
        E(),
        RETERR()
    );

    xl = child->dfc->xl;
    SYS_PTR(
        &tmp->frame, create_frame, (xl, xl->ctx->pool),
        ENOMEM,
        E(),
        GOTO(failed, &error)
    );

    tmp->child = child;
    INIT_LIST_HEAD(&tmp->list);
    dfc_sort_initialize(&tmp->sort);

    atomic_inc(&child->count, memory_order_seq_cst);

    *req = tmp;

    return 0;

failed:
    SYS_FREE(tmp);

    return error;
}

err_t __dfc_sort_send(dfc_child_t * child, dfc_sort_t * sort);

void dfc_request_free(dfc_request_t * req)
{
    dfc_child_t * child;
    dfc_sort_t sort;

    child = req->child;
    if ((child->count < child->dfc->max_requests) ||
        list_empty(&child->pool))
    {
        list_add_tail(&req->list, &child->pool);
    }
    else
    {
        dfc_request_destroy(req);
    }

    if (child->active < child->dfc->requests)
    {
        dfc_sort_initialize(&sort);
        __dfc_sort_send(child, &sort);
    }
}

void dfc_transaction_destroy(dfc_transaction_t * txn)
{
    sys_mutex_lock(&txn->dfc->lock);

    list_del_init(&txn->list);

    sys_mutex_unlock(&txn->dfc->lock);

    SYS_FREE(txn);
}

err_t dfc_transaction_create(dfc_t * dfc, dfc_transaction_t ** txn)
{
    dfc_transaction_t * tmp, * aux;
    struct list_head * item;
    int64_t id;

    SYS_MALLOC(
        &tmp, gfdfc_mt_dfc_transaction_t,
        E(),
        RETERR()
    );

    tmp->dfc = dfc;
    tmp->mask = 0;
    tmp->state = 0;

    sys_mutex_lock(&dfc->lock);

    tmp->id = dfc->current_txn++;
    id = tmp->id & dfc->txn_mask;
    item = dfc->txns[id].prev;
    while (item != &dfc->txns[id])
    {
        aux = list_entry(item, dfc_transaction_t, list);
        if (aux->id < tmp->id)
        {
            break;
        }
    }
    list_add_tail(&tmp->list, item);

    dfc_sort_initialize(&tmp->sort);
    sys_buf_set_int64(&tmp->sort.head, &tmp->sort.size, tmp->id);

    sys_mutex_initialize(&tmp->lock);

    sys_mutex_unlock(&dfc->lock);

    *txn = tmp;

    return 0;
}

err_t dfc_sort_update(dfc_t * dfc, dfc_sort_t * sort, uuid_t uuid, int64_t txn)
{
    uuid_t * client;
    void * ptr, * top, * aux;
    int64_t current;

    ptr = sort->data;
    top = ptr + sizeof(sort->data) - sort->size;
    __sys_buf_get_int64(&ptr);
    while (ptr < top)
    {
        client = __sys_buf_ptr_uuid(&ptr);
        aux = ptr;
        current = __sys_buf_get_int64(&ptr);

        if (uuid_compare(uuid, *client) == 0)
        {
            if ((uuid_compare(uuid, dfc->uuid) < 0) ^
                (current > txn))
            {
                __sys_buf_set_int64(&aux, txn);
            }

            return 0;
        }
    }

    SYS_CALL(
        sys_buf_check, (&sort->size, sizeof(uuid_t) + sizeof(int64_t)),
        E(),
        RETERR()
    );

    __sys_buf_set_uuid(&sort->head, uuid);
    __sys_buf_set_int64(&sort->head, txn);

    return 0;
}

void dfc_request_send(dfc_t * dfc, uint64_t mask, void * data, size_t size);

err_t dfc_sort_process_one(dfc_t * dfc, dfc_child_t * child, void * data,
                           size_t size)
{
    dfc_transaction_t * txn;
    struct list_head * item;
    uuid_t * uuid;
    int64_t id, num, need;
    err_t error;

    SYS_CALL(
        sys_buf_get_int64, (&data, &size, &num),
        E(),
        RETERR()
    );

    logD("Processing txn %lu", num);

    id = num & dfc->txn_mask;

    sys_mutex_lock(&dfc->lock);

    item = dfc->txns[id].next;
    while (item != &dfc->txns[id])
    {
        txn = list_entry(item, dfc_transaction_t, list);
        if (txn->id >= num)
        {
            if (txn->id == num)
            {
                goto found;
            }

            break;
        }
        item = item->next;
    }

    sys_mutex_unlock(&dfc->lock);

    logE("Unknown referenced transaction.");

    return ENOENT;

found:
    sys_mutex_unlock(&dfc->lock);

    sys_mutex_lock(&txn->lock);

    while (size > 0)
    {
        SYS_CALL(
            sys_buf_ptr_uuid, (&data, &size, &uuid),
            E(),
            GOTO(failed_lock, &error)
        );
        SYS_CALL(
            sys_buf_get_int64, (&data, &size, &need),
            E(),
            GOTO(failed_lock, &error)
        );

        SYS_CALL(
            dfc_sort_update, (dfc, &txn->sort, *uuid, need),
            E(),
            GOTO(failed_lock, &error)
        );
    }

    txn->mask |= 1 << child->idx;

    SYS_TEST(
        size == 0,
        EINVAL,
        E(),
        LOG(E(), "Invalid sort buffer received."),
        GOTO(failed_lock, &error)
    );

    error = 0;

failed_lock:
    sys_mutex_unlock(&txn->lock);

    logD("Transaction %lu is ready (%d)", num, txn->state);

    if ((atomic_dec(&txn->state, memory_order_seq_cst) & 0xFFFF) == 1)
    {
        dfc_request_send(txn->dfc, txn->mask, txn->sort.data,
                         sizeof(txn->sort.data) - txn->sort.size);
    }

    return error;
}

static char dfc_hex[] = "0123456789ABCDEF";

void dfc_dump(char * text, uint8_t * data, size_t size)
{
    uint32_t off, i;
    char buf[80];

    if (size == 0)
    {
        return;
    }

    buf[4] = ' ';
    buf[5] = '|';
    buf[54] = ' ';
    buf[55] = '|';
    buf[56] = ' ';
    buf[73] = 0;
    off = 0;
    do
    {
        buf[0] = dfc_hex[off >> 24];
        buf[1] = dfc_hex[(off >> 16) & 15];
        buf[2] = dfc_hex[(off >> 8) & 15];
        buf[3] = dfc_hex[off & 15];
        for (i = 0; i < 16; i++)
        {
            if (size > 0)
            {
                size--;
                buf[6 + i * 3] = ' ';
                buf[7 + i * 3] = dfc_hex[*data >> 4];
                buf[8 + i * 3] = dfc_hex[*data & 15];
                buf[57 + i] = *data++;
            }
            else
            {
                buf[6 + i * 3] = buf[7 + i * 3] = buf[8 + i * 3] = ' ';
                buf[57 + i] = '.';
            }
        }
        logD("%s", buf);
    } while (size > 0);
}

err_t dfc_sort_process(dfc_t * dfc, dfc_child_t * child, dfc_sort_t * sort)
{
    void * data;
    uint32_t length;

    dfc_dump("sort data", sort->head, sort->size);

    while (sort->size > 0)
    {
        SYS_CALL(
            sys_buf_ptr_block, (&sort->head, &sort->size, &data, &length),
            E(),
            RETERR()
        );

        dfc_sort_process_one(dfc, child, data, length);
    }

    SYS_TEST(
        sort->size == 0,
        EINVAL,
        E(),
        RETERR()
    );

    return 0;
}

SYS_CBK_CREATE(dfc_sort_recv, data, ((dfc_t *, dfc), (dfc_request_t *, req)))
{
    SYS_GF_CBK_CALL_TYPE(getxattr) * args;
    dfc_sort_t * sort;

    atomic_dec(&req->child->active, memory_order_seq_cst);

    args = (SYS_GF_CBK_CALL_TYPE(getxattr) *)data;
    logI("Processing sort request:");
    dict_foreach(args->dict, __xdata_dump, NULL);

    sort = &req->sort;
    sort->head = sort->data;
    sort->size = sizeof(sort->data);
    SYS_CALL(
        sys_dict_get_bin, (args->dict, DFC_XATTR_SORT, sort->data,
                           &sort->size),
        E(),
        GOTO(done)
    );

    SYS_CALL(
        dfc_sort_process, (dfc, req->child, sort),
        E()
    );

done:
    dfc_request_free(req);
}

err_t __dfc_sort_send(dfc_child_t * child, dfc_sort_t * sort)
{
    dfc_request_t * req;
    dict_t * xdata;
    err_t error;

    if (list_empty(&child->pool))
    {
        SYS_CALL(
            dfc_request_create, (child, &req),
            E(),
            LOG(E(), "Failed to create a request to send DFC sort data."),
            RETERR()
        );
    }
    else
    {
        req = list_entry(child->pool.next, dfc_request_t, list);
        list_del_init(&req->list);
    }

    xdata = NULL;
    SYS_CALL(
        __dfc_attach, (child->dfc, child->seq, sort->data,
                       sizeof(sort->data) - sort->size, &xdata),
        E(),
        LOG(E(), "Failed to prepare a DFC sort request."),
        GOTO(failed, &error)
    );

    logI("getxattr xdata = %p", xdata);
    dict_foreach(xdata, __xdata_dump, NULL);
    atomic_inc(&child->active, memory_order_seq_cst);
    SYS_IO(sys_gf_getxattr_wind, (req->frame, NULL, child->xl,
                                  &child->dfc->root_loc, DFC_XATTR_SORT,
                                  xdata),
           SYS_CBK(dfc_sort_recv, (child->dfc, req)), NULL);

    return 0;

failed:
    dfc_request_free(req);

    return error;
}

SYS_LOCK_CREATE(dfc_sort_send, ((dfc_child_t *, child), (dfc_sort_t *, sort)))
{
    SYS_CALL(
        __dfc_sort_send, (child, sort),
        E(),
        RETURN()
    );

    if (child->sort == sort)
    {
        dfc_sort_initialize(sort);
    }
    else
    {
        SYS_FREE(sort);
    }

    SYS_UNLOCK(&child->lock);
}

void dfc_sort_add(dfc_child_t * child, void * data, size_t size)
{
    dfc_sort_t * sort;
    err_t error = ENOBUFS;

    sort = child->sort;
    if (sort != NULL)
    {
        error = SYS_CALL(
                    sys_buf_set_block, (&sort->head, &sort->size, data, size),
                    D()
                );
    }

    if (error != 0)
    {
        SYS_CALL(
            dfc_sort_create, (&sort),
            E(),
            LOG(E(), "Cannot allocate buffers for DFC sort."),
            RETURN()
        );

        child->sort = sort;

        SYS_CALL(
            sys_buf_set_block, (&sort->head, &sort->size, data, size),
            E(),
            LOG(E(), "Cannot store data into DFC sort buffers."),
            RETURN()
        );
    }

    SYS_LOCK(&child->lock, INT64_MAX, dfc_sort_send, (child, sort));
}

void dfc_request_send(dfc_t * dfc, uint64_t mask, void * data, size_t size)
{
    dfc_child_t * child;

    logD("Sending %lu bytes of sort data to childs %lX", size, mask);

    list_for_each_entry(child, &dfc->children, list)
    {
        if ((mask & 1) != 0)
        {
            dfc_sort_add(child, data, size);
        }

        mask >>= 1;
    }
}

void dfc_child_destroy(dfc_child_t * child)
{
    dfc_request_t * req;

    while (!list_empty(&child->pool))
    {
        req = list_entry(child->pool.next, dfc_request_t, list);
        list_del_init(&req->list);

        dfc_request_destroy(req);
    }

    SYS_TEST(
        child->count == 0,
        EBUSY,
        E(),
        ASSERT("There are DFC requests being processed")
    );

    SYS_FREE(child);
}

err_t dfc_child_create(dfc_t * dfc, xlator_t * xl, dfc_child_t ** child)
{
    dfc_child_t * tmp;

    SYS_MALLOC(
        &tmp, gfdfc_mt_dfc_child_t,
        E(),
        RETERR()
    );

    sys_lock_initialize(&tmp->lock);

    tmp->dfc = dfc;
    tmp->xl = xl;
    tmp->count = 0;
    tmp->active = 0;
    tmp->seq = 0;
    tmp->idx = dfc->count;
    INIT_LIST_HEAD(&tmp->list);
    INIT_LIST_HEAD(&tmp->pool);

    tmp->sort = NULL;

    *child = tmp;

    return 0;
}

void dfc_destroy(dfc_t * dfc)
{
    dfc_child_t * child;

    while (!list_empty(&dfc->children))
    {
        child = list_entry(dfc->children.next, dfc_child_t, list);
        list_del_init(&child->list);

        dfc_child_destroy(child);
    }

    sys_mutex_terminate(&dfc->lock);

    if (dfc->txns != NULL)
    {
        SYS_FREE(dfc->txns);
    }
    SYS_FREE(dfc);
}

err_t dfc_create(xlator_t * xl, uint32_t max_requests, uint32_t requests,
                 inode_t * inode, dfc_t ** dfc)
{
    dfc_t * tmp;
    dfc_child_t * child;
    dfc_request_t * req;
    xlator_list_t * list;
    int32_t i;
    err_t error;

    SYS_MALLOC(
        &tmp, gfdfc_mt_dfc_t,
        E(),
        RETERR()
    );

    sys_mutex_initialize(&tmp->lock);

    tmp->xl = xl;
    memset(&tmp->root_loc, 0, sizeof(tmp->root_loc));
    tmp->root_loc.gfid[15] = 1;
    tmp->root_loc.path = "/";
    tmp->root_loc.name = "";
    tmp->root_loc.inode = inode_ref(inode);
    tmp->max_requests = max_requests;
    tmp->requests = requests;
    INIT_LIST_HEAD(&tmp->children);
    memcpy(tmp->uuid, xl->ctx->process_uuid, sizeof(tmp->uuid));

    tmp->txns = NULL;
    SYS_CALLOC(
        &tmp->txns, 1024, gfdfc_mt_dfc_transaction_t,
        E(),
        GOTO(failed, &error)
    );
    for (i = 0; i < 1024; i++)
    {
        INIT_LIST_HEAD(&tmp->txns[i]);
    }
    tmp->txn_mask = 1023;
    tmp->current_txn = 0;

    tmp->count = 0;
    for (list = xl->children; list != NULL; list = list->next)
    {
        SYS_CALL(
            dfc_child_create, (tmp, list->xlator, &child),
            E(),
            GOTO(failed, &error)
        );

        list_add_tail(&child->list, &tmp->children);
        tmp->count++;

        for (i = 0; i < requests; i++)
        {
            SYS_CALL(
                dfc_request_create, (child, &req),
                E(),
                GOTO(failed, &error)
            );

            list_add_tail(&req->list, &child->pool);
        }
    }

    *dfc = tmp;

    return 0;

failed:
    dfc_destroy(tmp);

    return error;
}

err_t dfc_prepare(xlator_t * xl, uint32_t max_requests, uint32_t requests,
                  inode_t * inode, dfc_t ** dfc, dict_t ** xdata)
{
    dfc_t * tmp;
    err_t error;

    SYS_CALL(
        dfc_create, (xl, max_requests, requests, inode, &tmp),
        E(),
        RETERR()
    );

    SYS_CALL(
        __dfc_attach, (tmp, 0, xdata, 0, xdata),
        E(),
        GOTO(failed, &error)
    );

    xl->private = tmp;
    *dfc = tmp;

    return 0;

failed:
    dfc_destroy(tmp);

    return error;
}

SYS_ASYNC_CREATE(__dfc_initialize, ((dfc_t *, dfc)))
{
    dfc_child_t * child;
    dfc_sort_t sort;
    int32_t i;

    dfc_sort_initialize(&sort);
    list_for_each_entry(child, &dfc->children, list)
    {
        for (i = 0; i < dfc->requests; i++)
        {
            SYS_CALL(
                __dfc_sort_send, (child, &sort),
                E()
            );
        }
    }
}

void dfc_initialize(dfc_t * dfc)
{
    SYS_ASYNC(__dfc_initialize, (dfc));
}

err_t dfc_begin(dfc_t * dfc, dfc_transaction_t ** txn)
{
    dfc_transaction_t * tmp;

    SYS_CALL(
        dfc_transaction_create, (dfc, &tmp),
        E(),
        RETERR()
    );

    *txn = tmp;

    return 0;
}

void dfc_end(dfc_transaction_t * txn, uint32_t count)
{
    uint32_t state;

    state = atomic_add_return(&txn->state, (count << 16) | count,
                              memory_order_seq_cst);
    if ((state >> 16) == 0)
    {
        dfc_transaction_destroy(txn);
    }
    else if ((state & 0xFFFF) == 0)
    {
        dfc_request_send(txn->dfc, txn->mask, txn->sort.data,
                         sizeof(txn->sort.data) - txn->sort.size);
    }
}

err_t dfc_attach(dfc_transaction_t * txn, dict_t ** xdata)
{
    SYS_CALL(
        __dfc_attach, (txn->dfc, txn->id, NULL, 0, xdata),
        E(),
        RETERR()
    );

    return 0;
}

bool dfc_complete(dfc_transaction_t * txn)
{
    if ((atomic_sub(&txn->state, 0x10000, memory_order_seq_cst) >> 16) == 1)
    {
        dfc_transaction_destroy(txn);

        return true;
    }

    return false;
}