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

#include "dfc.h"

struct _dfc_sort;
typedef struct _dfc_sort dfc_sort_t;

struct _dfc_dependencies;
typedef struct _dfc_dependencies dfc_dependencies_t;

struct _dfc_link;
typedef struct _dfc_link dfc_link_t;

struct _dfc_request;
typedef struct _dfc_request dfc_request_t;

struct _dfc_client;
typedef struct _dfc_client dfc_client_t;

struct _dfc_manager;
typedef struct _dfc_manager dfc_manager_t;

struct _dfc_sort
{
    struct list_head list;
    void *           head;
    size_t           size;
    bool             pending;
    uint8_t          data[4096];
};

struct _dfc_dependencies
{
    void *  buffer;
    void *  head;
    size_t  size;
    uint8_t data[256];
};

struct _dfc_link
{
    dfc_request_t *  request;
    inode_t *        inode;
    uint64_t         graph;
    int32_t          index;
    struct list_head cycle;
    struct list_head client_list;
    struct list_head inode_list;
};

struct _dfc_request
{
    struct list_head list;
    dfc_request_t *  next;
    call_frame_t *   frame;
    xlator_t *       xl;
    dfc_client_t *   client;
    int64_t          txn;
    dfc_link_t       link1;
    dfc_link_t       link2;
    void *           sort;
    ssize_t          sort_size;
    uintptr_t *      delay;
    int32_t          refs;
    bool             ro;
    bool             bad;
    bool             ready;
};

struct _dfc_client
{
    uuid_t           uuid;
    sys_lock_t       lock;
    dfc_client_t *   next;
    dfc_manager_t *  dfc;
    int64_t          received_txn;
    int64_t          next_txn;
    dfc_sort_t *     sort;
    uint32_t         refs;
    uint32_t         txn_mask;
    dfc_request_t ** requests;
    struct list_head sort_slots;
    struct list_head sort_pending;
};

struct _dfc_manager
{
    sys_lock_t     lock;
    uint64_t       graph;
    dfc_client_t * clients[256];
};

#define DFC_REQ_SIZE SYS_CALLS_ADJUST_SIZE(sizeof(dfc_request_t))

int32_t __dump_xdata(dict_t * xdata, char * key, data_t * value, void * data)
{
    logI("    %s: %u", key, value->len);

    return 0;
}

err_t dfc_client_get(dfc_manager_t * dfc, uuid_t uuid, dfc_client_t ** client)
{
    dfc_client_t * tmp;

    sys_rcu_read_lock();

    tmp = sys_rcu_dereference(dfc->clients[uuid[0]]);
    while ((tmp != NULL) && (uuid_compare(tmp->uuid, uuid) != 0))
    {
        tmp = sys_rcu_dereference(tmp->next);
    }
    if (tmp == NULL)
    {
        return ENOENT;
    }

    if (atomic_inc_not_zero(&tmp->refs, memory_order_seq_cst,
                                        memory_order_seq_cst))
    {
        *client = tmp;
    }

    sys_rcu_read_unlock();

    return 0;
}

err_t __dfc_client_add(dfc_manager_t * dfc, uuid_t uuid, int64_t txn,
                       dfc_client_t ** client)
{
    dfc_client_t * tmp;
    err_t error;

    if (dfc_client_get(dfc, uuid, &tmp) != 0)
    {
        SYS_MALLOC(
            &tmp, dfc_mt_dfc_client_t,
            E(),
            RETERR()
        );

        uuid_copy(tmp->uuid, uuid);
        tmp->dfc = dfc;
        INIT_LIST_HEAD(&tmp->sort_slots);
        INIT_LIST_HEAD(&tmp->sort_pending);
        SYS_CALLOC0(
            &tmp->requests, 1024, sys_mt_list_head,
            E(),
            GOTO(failed, &error)
        );
        tmp->txn_mask = 1023;
        tmp->next_txn = 0;
        tmp->received_txn = txn;
        tmp->refs = 2;
        tmp->sort = NULL;

        sys_lock_initialize(&tmp->lock);

        tmp->next = dfc->clients[uuid[0]];

        sys_rcu_assign_pointer(dfc->clients[uuid[0]], tmp);
    }

    *client = tmp;

    return 0;

failed:
    SYS_FREE(tmp);

    return error;
}

SYS_RCU_CREATE(dfc_client_destroy, ((dfc_client_t *, client)))
{
    SYS_FREE(client->requests);
    SYS_FREE(client);
}

void dfc_client_put(dfc_client_t * client)
{
    int32_t i;

    if (atomic_dec(&client->refs, memory_order_seq_cst) == 1)
    {
        for (i = 0; i < 1024; i++)
        {
            if (client->requests[i] != NULL)
            {
                break;
            }
        }
        SYS_TEST(
            (i < 1024) || !list_empty(&client->sort_slots) ||
            !list_empty(&client->sort_pending),
            EBUSY,
            E(),
            ASSERT("Client has pending work to do")
        );

        SYS_RCU(dfc_client_destroy, (client));
    }
}

err_t __dfc_client_del(dfc_manager_t * dfc, uuid_t uuid)
{
    dfc_client_t * client, ** pprev;

    pprev = &dfc->clients[uuid[0]];
    client = *pprev;
    while (client != NULL)
    {
        if (uuid_compare(client->uuid, uuid) == 0)
        {
            sys_rcu_assign_pointer(*pprev, client->next);

            dfc_client_put(client);

            return 0;
        }
        pprev = &client->next;
        client = *pprev;
    }

    return ENOENT;
}

void dfc_sort_initialize(dfc_sort_t * sort)
{
    sort->head = sort->data;
    sort->size = sizeof(sort->data);
    sort->pending = true;
}

err_t dfc_sort_create(dfc_client_t * client)
{
    SYS_MALLOC(
        &client->sort, dfc_mt_dfc_sort_t,
        E(),
        RETERR()
    );

    dfc_sort_initialize(client->sort);

    return 0;
}

err_t dfc_sort_unwind(call_frame_t * frame, dfc_sort_t * sort)
{
    dict_t * xdata;
    err_t error = 0;

    xdata = NULL;
    SYS_CALL(
        sys_dict_set_bin, (&xdata, DFC_XATTR_SORT, sort->data,
                           sizeof(sort->data) - sort->size, NULL),
        E(),
        GOTO(failed, &error)
    );

    logI("Sending sort request");
    dict_foreach(xdata, __dump_xdata, NULL);

    SYS_IO(sys_gf_getxattr_unwind, (frame, 0, 0, xdata, NULL), NULL);

    sys_dict_release(xdata);

    return 0;

failed:
    // Probably there is a serious memory problem. Try to unwind the sort
    // request and let the client decide. At least we may free some memory
    // and a future sort request will handle the pending data.
    SYS_IO(sys_gf_getxattr_unwind_error, (frame, ENOMEM, NULL), NULL);

    return error;
}

SYS_LOCK_CREATE(dfc_sort_client_send, ((dfc_client_t *, client),
                                       (dfc_sort_t *, sort)))
{
    dfc_request_t * req;

    while (!list_empty(&client->sort_slots))
    {
        req = list_entry(client->sort_slots.next, dfc_request_t, list);
        list_del_init(&req->list);

        if (sys_delay_cancel((uintptr_t *)req, false))
        {
            SYS_CALL(
                dfc_sort_unwind, (req->frame, sort),
                E(),
                GOTO(failed)
            );

            if (client->sort == sort)
            {
                dfc_sort_initialize(sort);
            }
            else
            {
                SYS_FREE(sort);
            }

            SYS_UNLOCK(&client->lock);

            return;
        }
    }

failed:
    list_add_tail(&sort->list, &client->sort_pending);

    SYS_UNLOCK(&client->lock);
}

SYS_LOCK_CREATE(__dfc_sort_client_retry, ((dfc_request_t *, req)))
{
    if (!list_empty(&req->list))
    {
        list_del_init(&req->list);

        SYS_UNLOCK(&req->client->lock);

        sys_delay_release((uintptr_t *)req);
    }
    else
    {
        SYS_UNLOCK(&req->client->lock);
    }
}

SYS_DELAY_CREATE(dfc_sort_client_retry, ((void, data, CALLS)))
{
    dfc_request_t * req;

    req = (dfc_request_t *)(data - DFC_REQ_SIZE);
    SYS_LOCK(&req->client->lock, INT64_MAX, __dfc_sort_client_retry, (req));

    SYS_IO(sys_gf_getxattr_unwind, (req->frame, 0, 0, NULL, NULL), NULL);
}

void dfc_dependency_initialize(dfc_dependencies_t * deps, int64_t txn)
{
    deps->head = deps->data;
    deps->size = sizeof(deps->data);
    SYS_CALL(
        sys_buf_check, (&deps->size, sizeof(int64_t)),
        E(),
        ASSERT("Internal buffer too small.")
    );
    __sys_buf_set_int64(&deps->head, txn);
    deps->buffer = deps->head;
}

err_t dfc_dependency_copy(dfc_dependencies_t * deps, void * data)
{
    return sys_buf_set_raw(&deps->head, &deps->size, data,
                           sizeof(uuid_t) + sizeof(int64_t));
}

err_t dfc_dependency_add(dfc_dependencies_t * deps, uuid_t uuid, int64_t txn)
{
    SYS_CALL(
        sys_buf_check, (&deps->size, sizeof(uuid_t) + sizeof(int64_t)),
        E(),
        RETERR()
    );
    __sys_buf_set_uuid(&deps->head, uuid);
    __sys_buf_set_int64(&deps->head, txn);

    return 0;
}

err_t dfc_dependency_set(dfc_dependencies_t * deps, uuid_t uuid, int64_t txn)
{
    void * ptr, * tmp;

    ptr = deps->buffer;
    while (ptr < deps->head)
    {
        if (uuid_compare(*__sys_buf_ptr_uuid(&ptr), uuid) == 0)
        {
            tmp = ptr;
            if (__sys_buf_get_int64(&ptr) < txn)
            {
                __sys_buf_set_int64(&tmp, txn);
            }

            return 0;
        }
        __sys_buf_get_int64(&ptr);
    }

    return dfc_dependency_add(deps, uuid, txn);
}

err_t dfc_dependency_merge(dfc_dependencies_t * dst, dfc_dependencies_t * src)
{
    void * ptr;
    uuid_t * uuid;
    int64_t txn;

    ptr = src->buffer;
    while (ptr < src->head)
    {
        uuid = __sys_buf_ptr_uuid(&ptr);
        txn = __sys_buf_get_int64(&ptr);
        SYS_CALL(
            dfc_dependency_set, (dst, *uuid, txn),
            E(),
            RETERR()
        );
    }

    return 0;
}

err_t dfc_link_add(xlator_t * xl, dfc_link_t * link, dfc_dependencies_t * deps)
{
    dfc_link_t * first, * current, * aux;
    dfc_client_t * client, * tmp;
    inode_t * inode;
    uint64_t value;
    err_t error;
    bool found;

    error = 0;

    inode = link->inode;
    LOCK(&inode->lock);

    logD("Adding %ld", link->request->txn);

    if ((__inode_ctx_get(inode, xl, &value) != 0) || (value == 0))
    {
        logD("Created context");
        value = (uint64_t)(uintptr_t)link;
        SYS_CODE(
            __inode_ctx_set, (inode, xl, &value),
            ENOSPC,
            E(),
            GOTO(done, &error)
        );
    }
    else
    {
        client = link->request->client;
        first = (dfc_link_t *)(uintptr_t)value;
        current = first;
        found = false;
        do
        {
            tmp = current->request->client;
            if (uuid_compare(tmp->uuid, client->uuid) == 0)
            {
                if (current->request->txn > link->request->txn)
                {
                    logD("Insert first");
                    list_add_tail(&link->client_list, &current->client_list);
                    list_add(&link->inode_list, &current->inode_list);
                    list_del_init(&current->inode_list);

                    if (current == first)
                    {
                        logD("Replace head");
                        value = (uint64_t)(uintptr_t)link;
                        SYS_CODE(
                            __inode_ctx_set, (inode, xl, &value),
                            ENOSPC,
                            E(),
                            ASSERT("Unable to modify inode context")
                        );
                        first = link;
                    }
                    current = link;
                }
                else
                {
                    logD("Insert inside");
                    aux = list_entry(current->client_list.prev, dfc_link_t,
                                     client_list);
                    while (aux->request->txn > link->request->txn)
                    {
                        aux = list_entry(aux->client_list.prev, dfc_link_t,
                                         client_list);
                    }
                    list_add(&link->client_list, &aux->client_list);
                }
                aux = current;
                do
                {
                    logD("-> %ld", aux->request->txn);
                    aux = list_entry(aux->client_list.next, dfc_link_t,
                                     client_list);
                } while (aux != current);
                found = true;
            }
            else
            {
                SYS_CODE(
                    dfc_dependency_add, (deps, tmp->uuid, tmp->received_txn),
                    EBUSY,
                    E(),
                    GOTO(done, &error)
                );
            }
            current = list_entry(current->inode_list.next, dfc_link_t,
                                 inode_list);
        } while (current != first);

        if (!found)
        {
            list_add_tail(&link->inode_list, &first->inode_list);
        }
    }

done:
    UNLOCK(&inode->lock);

    return error;
}

bool dfc_link_entry_allowed(dfc_link_t * link, dfc_link_t * root, uuid_t uuid,
                            int64_t txn)
{
    dfc_link_t * current;
    dfc_request_t * req;
    dfc_client_t * client;
    bool res;

    current = root;
    do
    {
        req = current->request;
        if (uuid_compare(req->client->uuid, uuid) == 0)
        {
            if (req->txn <= txn)
            {
                logD("Required transaction not executed yet (inode)");
                if (req->bad)
                {
                    logW("Cascading bad request flag");
                    link->request->bad = true;
                }
                return false;
            }
            return true;
        }
        current = list_entry(root->inode_list.next, dfc_link_t, inode_list);
    } while (current != root);

    SYS_CALL(
        dfc_client_get, (link->request->client->dfc, uuid, &client),
        E(),
        GOTO(failed)
    );

    res = (txn < client->next_txn);
    if (!res)
    {
        logD("Required transaction not executed yet (client)");
    }

    dfc_client_put(client);

    return res;

failed:
    link->request->bad = true;

    // Return true to discard this dependency. However, when the requests will
    // be ready to be executed it will fail and leave inode in an unhealthy
    // state.
    return true;
}

dfc_link_t * dfc_link_lookup(dfc_link_t * root, uuid_t uuid)
{
    dfc_link_t * node;

    node = root;
    do
    {
        if (uuid_compare(uuid, node->request->client->uuid) == 0)
        {
            return node;
        }
        node = list_entry(node->inode_list.next, dfc_link_t, inode_list);
    } while (node != root);

    return NULL;
}

int32_t dfc_link_scan(dfc_link_t * root, dfc_link_t * node, int64_t id,
                      int32_t * index, struct list_head * cycle)
{
    dfc_link_t * link;
    uuid_t * uuid;
    void * ptr;
    ssize_t size;
    int32_t min;

    node->graph = id;
    node->index = min = *index;
    (*index)++;
    list_add_tail(&node->cycle, cycle);

    ptr = node->request->sort;
    size = node->request->sort_size;
    while (size > 0)
    {
        uuid = __sys_buf_ptr_uuid(&ptr);
        __sys_buf_get_int64(&ptr);

        link = dfc_link_lookup(root, *uuid);
        if (link != NULL)
        {
            if (link->graph != id)
            {
                min = SYS_MIN(min, dfc_link_scan(root, link, id, index,
                                                 cycle));
            }
            else if (!list_empty(&link->cycle))
            {
                min = SYS_MIN(min, link->index);
            }
        }
    }

    if ((node->index == min) && (node->cycle.next == cycle))
    {
        list_del_init(&node->cycle);
    }

    return min;
}

char * dfc_uuid(char * text, uuid_t uuid)
{
    sprintf(text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x-%02x%02x%02x%02x%02x%02x",
                  uuid[0], uuid[1], uuid[2], uuid[3],
                  uuid[4], uuid[5], uuid[6], uuid[7],
                  uuid[8], uuid[9], uuid[10], uuid[11],
                  uuid[12], uuid[13], uuid[14], uuid[15]);

    return text;
}

bool dfc_link_in_cycle(struct list_head * cycle, uuid_t uuid)
{
    dfc_link_t * link;
    char uuid1[48], uuid2[48];

    list_for_each_entry(link, cycle, cycle)
    {
        if (uuid_compare(uuid, link->request->client->uuid) == 0)
        {
            logD("Breaking dependency from %s to %s",
                 dfc_uuid(uuid1, link->request->client->uuid),
                 dfc_uuid(uuid2, uuid));
            return true;
        }
    }

    return false;
}

void dfc_link_break(dfc_link_t * link, struct list_head * cycle)
{
    void * ptr, * top, * base;
    uuid_t * uuid;

    ptr = link->request->sort;
    top = ptr + link->request->sort_size;
    while (ptr != top)
    {
        uuid = __sys_buf_ptr_uuid(&ptr);
        base = ptr;
        __sys_buf_get_int64(&ptr);
        if (dfc_link_in_cycle(cycle, *uuid))
        {
            __sys_buf_set_int64(&base, 0);
        }
    }
}

dfc_request_t * dfc_link_allowed(dfc_link_t * link, dfc_link_t * root)
{
    dfc_request_t * req;
    dfc_link_t * node, * tmp;
    uuid_t * uuid;
    struct list_head cycle;
    void * ptr, * new_ptr, * base;
    size_t size, new_size;
    uint64_t graph;
    int64_t num;
    int32_t index;
    char uuid1[48];

    INIT_LIST_HEAD(&cycle);
    req = link->request;

    do
    {
        ptr = req->sort;
        new_ptr = ptr;
        new_size = 0;
        if (req->sort_size > 0)
        {
            size = req->sort_size;
            do
            {
                SYS_CALL(
                    sys_buf_check, (&size, sizeof(uuid_t) + sizeof(int64_t)),
                    E(),
                    ASSERT("Sort data is invalid.")
                );
                base = ptr;
                uuid = __sys_buf_ptr_uuid(&ptr);
                num = __sys_buf_get_int64(&ptr);
                logD("Evaluating %s:%lu", dfc_uuid(uuid1, *uuid), num);

                if (!dfc_link_entry_allowed(link, root, *uuid, num))
                {
                    if (new_ptr != base)
                    {
                        memcpy(new_ptr, base, sizeof(uuid_t) + sizeof(int64_t));
                    }
                    new_ptr += sizeof(uuid_t) + sizeof(int64_t);
                    new_size += sizeof(uuid_t) + sizeof(int64_t);
                }
            } while (size > 0);
        }

        logD("Remaining dependencies: %lu", new_size);

        req->sort_size = new_size;
        if (new_size == 0)
        {
            return req;
        }

        index = 0;
        graph = atomic_inc(&req->client->dfc->graph, memory_order_seq_cst);
        dfc_link_scan(root, link, graph, &index, &cycle);
        if (list_empty(&cycle))
        {
            logD("Request delayed due to dependencies");
            return NULL;
        }

        logD("Cycle detected in request dependencies.");

        tmp = NULL;
        list_for_each_entry(node, &cycle, cycle)
        {
            if (tmp == NULL)
            {
                tmp = node;
            }
            else if (uuid_compare(tmp->request->client->uuid,
                                  node->request->client->uuid) > 0)
            {
                tmp = node;
            }
        }

        dfc_link_break(tmp, &cycle);

        while (!list_empty(&cycle))
        {
            list_del_init(cycle.next);
        }
    } while (1);
}

dfc_request_t * dfc_link_check(dfc_link_t * link, dfc_link_t * root)
{
    dfc_request_t * req;

    if (link->request->ready)
    {
        req = dfc_link_allowed(link, root);
        if ((req != NULL) &&
            (atomic_dec(&req->refs, memory_order_seq_cst) == 1))
        {
            return req;
        }
        else
        {
            if (req == NULL)
            {
                logD("Request not allowed");
            }
            else
            {
                logD("Another inode has dependencies: %d", req->refs);
            }
        }
    }
    else
    {
        logD("Request is not ready");
    }

    return NULL;
}

void dfc_request_execute(dfc_request_t * req);

SYS_ASYNC_CREATE(dfc_link_del, ((xlator_t *, xl), (dfc_link_t *, link)))
{
    dfc_link_t * root, * tmp;
    dfc_request_t * req = NULL;
    uint64_t value;
    inode_t * inode;

    inode = link->inode;
    logD("Removing request from inode %p", inode);
    LOCK(&inode->lock);

    SYS_ASSERT(
        (__inode_ctx_get(inode, xl, &value) == 0) && (value != 0),
        "The inode does not have pending requests, but it should."
    );
    root = (dfc_link_t *)(uintptr_t)value;

    if (!list_empty(&link->client_list))
    {
        tmp = list_entry(link->client_list.next, dfc_link_t, client_list);
        if ((link == root) || !list_empty(&link->inode_list))
        {
            req = dfc_link_check(tmp, root);
        }
        list_add(&tmp->inode_list, &link->inode_list);
        list_del_init(&link->client_list);
    }

    if (link == root)
    {
        if (list_empty(&link->inode_list))
        {
            value = 0;
        }
        else
        {
            value = (uint64_t)(uintptr_t)list_entry(link->inode_list.next,
                                                    dfc_link_t, inode_list);
        }
        SYS_CALL(
            __inode_ctx_set, (inode, xl, &value),
            E(),
            ASSERT("Failed to modify inode context.")
        );
    }
    list_del_init(&link->inode_list);

    UNLOCK(&inode->lock);

    if (req != NULL)
    {
        logD("Dispatching request %ld after request %ld", req->txn,
             link->request->txn);
        dfc_request_execute(req);
    }
}

SYS_ASYNC_CREATE(dfc_link_execute, ((dfc_link_t *, link)))
{
    dfc_link_t * root;
    dfc_request_t * req = NULL;
    uint64_t value;

    logD("Evaluating order of execution on inode %p", link->inode);

    LOCK(&link->inode->lock);

    SYS_ASSERT(
        (__inode_ctx_get(link->inode, link->request->xl, &value) == 0) &&
        (value != 0),
        "The inode does not have pending requests, but it should."
    );
    root = (dfc_link_t *)(uintptr_t)value;
    if ((root == link) || !list_empty(&link->inode_list))
    {
        req = dfc_link_check(link, root);
    }

    UNLOCK(&link->inode->lock);

    if (req != NULL)
    {
        dfc_request_execute(req);
    }
    else
    {
        logD("Request %ld cannot be executed yet on inode %p",
             link->request->txn, link->inode);
    }
}

SYS_CBK_CREATE(dfc_request_complete, data, ((dfc_request_t *, req)))
{
    sys_gf_unwind(req->frame, 0, -1, NULL, NULL, (uintptr_t *)req, data);

    SYS_ASSERT(
        req->txn == req->client->next_txn,
        "Current request does not correspond to the next transaction: "
        "%ld, %ld", req->txn, req->client->next_txn
    );

    atomic_inc(&req->client->next_txn, memory_order_seq_cst);

    if (req->link1.inode != NULL)
    {
        dfc_link_del(req->xl, &req->link1);
    }
    if (req->link2.inode != NULL)
    {
        dfc_link_del(req->xl, &req->link2);
    }
}

void dfc_request_execute(dfc_request_t * req)
{
    logD("Dispatching request %ld", req->txn);
    if (!req->bad)
    {
        sys_gf_wind(req->frame, NULL, FIRST_CHILD(req->xl),
                    SYS_CBK(dfc_request_complete, (req)),
                    NULL, (uintptr_t *)req, (uintptr_t *)req + DFC_REQ_SIZE);
    }
    else
    {
        logD("Request is bad");
        sys_gf_unwind_error(req->frame, EIO, NULL, NULL, NULL,
                            (uintptr_t *)req, (uintptr_t *)req + DFC_REQ_SIZE);

        if (req->link1.inode != NULL)
        {
            dfc_link_del(req->xl, &req->link1);
        }
        if (req->link2.inode != NULL)
        {
            dfc_link_del(req->xl, &req->link2);
        }
    }
}

err_t dfc_request_dependencies(dfc_request_t * req, dfc_dependencies_t * deps)
{
    dfc_dependencies_t deps_aux;
    err_t error;

    if (req->link1.inode != NULL)
    {
        logD("Checking inode1 dependencies");
        SYS_CALL(
            dfc_link_add, (req->xl, &req->link1, deps),
            E(),
            RETERR()
        );
    }
    if (req->link2.inode != NULL)
    {
        logD("Checking inode2 dependencies");
        dfc_dependency_initialize(&deps_aux, 0);
        SYS_CALL(
            dfc_link_add, (req->xl, &req->link2, &deps_aux),
            E(),
            GOTO(failed, &error)
        );
        dfc_dependency_merge(deps, &deps_aux);
    }

    return 0;

failed:
    dfc_link_del(req->xl, &req->link1);

    return error;
}

err_t dfc_dependency_build(dfc_dependencies_t * deps, dfc_request_t * req)
{
    dfc_dependency_initialize(deps, req->txn);
    return SYS_CALL(
               dfc_request_dependencies, (req, deps),
               E(),
               RETERR()
           );
}

void dfc_dependency_dump(dfc_request_t * req, dfc_dependencies_t * deps)
{
    void * ptr;
    uuid_t * uuid;
    int64_t num;
    char uuid1[48];

    ptr = deps->buffer;
    if (ptr != deps->head)
    {
        logI("Request %s:%lu depends on:",
             dfc_uuid(uuid1, req->client->uuid), req->txn);

        while (ptr != deps->head)
        {
            uuid = __sys_buf_ptr_uuid(&ptr);
            num = __sys_buf_get_int64(&ptr);
            logI("    %s:%lu", dfc_uuid(uuid1, *uuid), num);
        }
    }
    else
    {
        logI("Request %s:%lu does not have dependencies",
             dfc_uuid(uuid1, req->client->uuid), req->txn);
    }
}

err_t dfc_request_prepare(dfc_manager_t * dfc, dfc_request_t * req,
                          void * data, size_t size)
{
    dfc_dependencies_t deps;
    dfc_client_t * client;
    void * tmp, * top;

    dfc_dependency_initialize(&deps, 0);

    top = data + size;
    while (top > data)
    {
        tmp = data;
        SYS_CALL(
            dfc_client_get, (dfc, *__sys_buf_ptr_uuid(&data), &client),
            E(),
            LOG(E(), "Unknown referenced client"),
            RETERR()
        );

        if (__sys_buf_get_int64(&data) >= client->next_txn)
        {
            SYS_CALL(
                dfc_dependency_copy, (&deps, tmp),
                E(),
                LOG(E(), "Unable to copy dependencies"),
                RETERR()
            );
        }
    }

    size = deps.head - deps.buffer;
    if (size > 0)
    {
        SYS_ALLOC(
            &req->sort, size, sys_mt_uint8_t,
            E(),
            RETERR()
        );
        memcpy(req->sort, deps.buffer, size);
    }

    req->sort_size = size;

    return 0;
}

SYS_DELAY_DECLARE(dfc_sort_client_process, ((dfc_request_t *, req)));

err_t dfc_sort_parse(dfc_client_t * client, void * sort, size_t size)
{
    dfc_request_t * req, ** preq;
    void * ptr, * data, * tmp;
    int64_t txn;
    size_t bsize;
    uint32_t length;

    logD("Parsing sort data: %lu", size);

    ptr = sort;
    while (size > 0)
    {
        SYS_CALL(
            sys_buf_ptr_block, (&ptr, &size, &data, &length),
            E(),
            RETERR()
        );

        bsize = length;
        SYS_CALL(
            sys_buf_get_int64, (&data, &bsize, &txn),
            E(),
            CONTINUE()
        );

        preq = &client->requests[txn & client->txn_mask];
        req = *preq;
        while ((req != NULL) && (req->txn != txn))
        {
            preq = &req->next;
            req = *preq;
        }
        SYS_TEST(
            req != NULL,
            EINVAL,
            T(),
            LOG(W(), "Request not found"),
            CONTINUE()
        );

        if (dfc_request_prepare(client->dfc, req, data, bsize) != 0)
        {
            logD("Failed to prepare request %ld", req->txn);
            req->bad = true;
        }

        logD("Processed txn = %lu, received_txn = %lu",
             txn, client->received_txn);
        while (client->received_txn == txn)
        {
            logI("Going to execute transaction %lu", txn);

            *preq = req->next;

            client->received_txn++;
            tmp = req->sort;
            if (!sys_delay_cancel(req->delay, false))
            {
                SYS_FREE(tmp);
            }
            else
            {
                dfc_sort_client_process(req);
            }
            txn++;
            req = client->requests[txn & client->txn_mask];
            while ((req != NULL) && (req->txn != txn))
            {
                req = req->next;
            }
            if (req == NULL)
            {
                break;
            }
        }
    }

    SYS_FREE(sort);

    return 0;
}

SYS_LOCK_CREATE(dfc_sort_client_recv, ((dfc_client_t *, client),
                                       (call_frame_t *, frame),
                                       (int64_t, txn), (void *, data),
                                       (size_t, size)))
{
    dfc_sort_t * sort;
    dfc_request_t * req;

    SYS_CALL(
        dfc_sort_parse, (client, data, size),
        E()
    );

    if (!list_empty(&client->sort_pending))
    {
        sort = list_entry(client->sort_pending.next, dfc_sort_t, list);
        list_del_init(&sort->list);

        dfc_sort_unwind(frame, sort);

        if (client->sort == sort)
        {
            dfc_sort_initialize(sort);
        }
        else
        {
            SYS_FREE(sort);
        }
    }
    else
    {
        req = (dfc_request_t *)__SYS_DELAY(30000, DFC_REQ_SIZE,
                                           dfc_sort_client_retry, (NULL), 1);
        req->client = client;
        req->frame = frame;
        list_add_tail(&req->list, &client->sort_slots);
    }

    SYS_UNLOCK(&client->lock);
}

SYS_LOCK_CREATE(dfc_sort_client_add, ((dfc_request_t *, req)))
{
    dfc_dependencies_t deps;
    dfc_client_t * client;
    dfc_sort_t * sort;
    err_t error = ENOBUFS;

    client = req->client;

    logD("Processing request dependencies");

    SYS_CALL(
        dfc_dependency_build, (&deps, req),
        E(),
        GOTO(failed)
    );

    dfc_dependency_dump(req, &deps);

    sort = client->sort;
    if (sort != NULL)
    {
        error = sys_buf_set_block(&sort->head, &sort->size, deps.data,
                                  sizeof(deps.data) - deps.size);
    }
    if (error != 0)
    {
        SYS_CALL(
            dfc_sort_create, (client),
            E(),
            GOTO(failed, &error)
        );
        sort = client->sort;
        SYS_CALL(
            sys_buf_set_block, (&sort->head, &sort->size, deps.data,
                                sizeof(deps.data) - deps.size),
            E(),
            GOTO(failed, &error)
        );
    }

    if (sort->pending)
    {
        sort->pending = false;
        // Delay send to allow other requests to be accumulated.
        SYS_LOCK(&client->lock, INT64_MAX, dfc_sort_client_send, (client,
                                                                  sort));
    }

    req->next = client->requests[req->txn & client->txn_mask];
    client->requests[req->txn & client->txn_mask] = req;

    SYS_UNLOCK(&client->lock);

    return;

failed:
    SYS_UNLOCK(&client->lock);

    req->bad = true;

    sys_delay_execute(req->delay, error);
}

err_t dfc_analyze_xattr(uint32_t * mask, uint32_t value, err_t error)
{
    if ((error == 0) || (error == ENOENT))
    {
        if (error == 0)
        {
            (*mask) |= value;
        }

        return 0;
    }

    return EINVAL;
}

err_t dfc_analyze(dfc_manager_t * dfc, dict_t ** xdata, uuid_t uuid,
                  int64_t * txn, void ** sort, size_t * size)
{
    size_t length;
    uint32_t mask;
    uint8_t data[1024];
    char uuid1[48];

    mask = 0;

    SYS_CALL(
        dfc_analyze_xattr, (&mask, 1, sys_dict_del_block(xdata, DFC_XATTR_UUID,
                                                         uuid,
                                                         sizeof(uuid_t))),
        E(),
        RETVAL(EINVAL)
    );
    SYS_CALL(
        dfc_analyze_xattr, (&mask, 2, sys_dict_del_int64(xdata, DFC_XATTR_ID,
                                                         txn)),
        E(),
        RETVAL(EINVAL)
    );
    length = sizeof(data);
    SYS_CALL(
        dfc_analyze_xattr, (&mask, 4, sys_dict_del_bin(xdata, DFC_XATTR_SORT,
                                                       data, &length)),
        E(),
        RETVAL(EINVAL)
    );

    if (mask == 0)
    {
        return ENOENT;
    }
    if ((mask & 3) != 3)
    {
        logE("Invalid DFC request.");

        return EINVAL;
    }
    if ((mask & 4) != 0)
    {
        if (sort == NULL)
        {
            logE("Unexpected DFC sort request.");

            return EINVAL;
        }

        *size = length;
        SYS_ALLOC(
            sort, length, sys_mt_uint8_t,
            E(),
            RETVAL(EINVAL)
        );

        memcpy(*sort, data, length);
    }

    logI("Request from %s: %ld, %lu", dfc_uuid(uuid1, uuid), *txn,
         (mask & 4) ? length : 0);

    return 0;
}

SYS_DELAY_DEFINE(dfc_sort_client_process, ((dfc_request_t *, req)))
{
    bool deps;

    req->ready = true;

    logD("Executing request %lu", req->txn);

    deps = false;
    if (req->link1.inode != NULL)
    {
        dfc_link_execute(&req->link1);
        deps = true;
    }
    if (req->link2.inode != NULL)
    {
        dfc_link_execute(&req->link2);
        deps = true;
    }

    if (!deps)
    {
        dfc_request_execute(req);
    }
}

SYS_LOCK_CREATE(dfc_managed, ((dfc_manager_t *, dfc), (dfc_request_t *, req),
                              (uuid_t, uuid, ARRAY, sizeof(uuid_t))))
{
    dfc_client_t * client;

    SYS_CALL(
        dfc_client_get, (dfc, uuid, &client),
        E(),
        LOG(E(), "DFC client not found. Rejecting request."),
        GOTO(failed)
    );

    req->client = client;
    req->link2.request = req->link1.request = req;
    INIT_LIST_HEAD(&req->link1.client_list);
    INIT_LIST_HEAD(&req->link1.inode_list);
    INIT_LIST_HEAD(&req->link1.cycle);
    INIT_LIST_HEAD(&req->link2.client_list);
    INIT_LIST_HEAD(&req->link2.inode_list);
    INIT_LIST_HEAD(&req->link2.cycle);

    req->bad = false;
    req->ready = false;

    req->sort = NULL;
    req->sort_size = -1;

    req->delay = SYS_DELAY(2000, dfc_sort_client_process, (req), 2);
    SYS_LOCK(&client->lock, INT64_MAX, dfc_sort_client_add, (req));

    SYS_UNLOCK(&dfc->lock);

    return;

failed:
    SYS_UNLOCK(&dfc->lock);

    sys_gf_unwind_error(req->frame, EIO, NULL, NULL, NULL, (uintptr_t *)req,
                        (uintptr_t *)req + DFC_REQ_SIZE);
}

SYS_LOCK_CREATE(__dfc_init_handler, ((dfc_manager_t *, dfc),
                                     (call_frame_t *, frame),
                                     (xlator_t *, xl),
                                     (uuid_t, uuid, ARRAY, sizeof(uuid_t)),
                                     (int64_t, txn),
                                     (uintptr_t *, data)))
{
    dfc_client_t * client;

    SYS_CALL(
        __dfc_client_add, (dfc, uuid, txn, &client),
        E(),
        GOTO(failed)
    );

    SYS_UNLOCK(&dfc->lock);

    dfc_client_put(client);

    sys_gf_wind_tail(frame, FIRST_CHILD(xl), NULL, NULL, data, data);

    return;

failed:
    SYS_UNLOCK(&dfc->lock);

    sys_gf_unwind_error(frame, EIO, NULL, NULL, NULL, data, data);
}

SYS_ASYNC_CREATE(dfc_init_handler, ((dfc_manager_t *, dfc),
                                    (call_frame_t *, frame),
                                    (xlator_t *, xl),
                                    (uuid_t, uuid, ARRAY, sizeof(uuid_t)),
                                    (int64_t, txn),
                                    SYS_GF_ARGS_lookup))
{
    uintptr_t * data = SYS_GF_FOP(lookup);
    SYS_LOCK(&dfc->lock, INT64_MAX,
             __dfc_init_handler, (dfc, frame, xl, uuid, txn, data));
}

SYS_ASYNC_CREATE(dfc_sort_handler, ((dfc_manager_t *, dfc),
                                    (call_frame_t *, frame),
                                    (xlator_t *, xl),
                                    (uuid_t, uuid, ARRAY, sizeof(uuid_t)),
                                    (int64_t, txn), (void *, sort),
                                    (size_t, size)))
{
    dfc_client_t * client;

    SYS_CALL(
        dfc_client_get, (dfc, uuid, &client),
        E(),
        GOTO(failed)
    );

    SYS_LOCK(&client->lock, INT64_MAX,
             dfc_sort_client_recv, (client, frame, txn, sort, size));

    return;

failed:
    SYS_IO(sys_gf_getxattr_unwind_error, (frame, EIO, NULL), NULL);
}

#define DFC_MANAGE(_fop, _ro, _inode1, _inode2) \
    SYS_ASYNC_CREATE(dfc_managed_##_fop, ((dfc_manager_t *, dfc), \
                                          (call_frame_t *, frame), \
                                          (xlator_t *, xl), \
                                          (uuid_t, uuid, ARRAY, \
                                                         sizeof(uuid_t)), \
                                          (int64_t, txn), \
                                          SYS_GF_ARGS_##_fop)) \
    { \
        dfc_request_t * req; \
        req = (dfc_request_t *)SYS_GF_FOP(_fop, DFC_REQ_SIZE); \
        req->frame = frame; \
        req->xl = xl; \
        req->txn = txn; \
        req->ro = _ro; \
        req->link1.inode = _inode1; \
        req->link2.inode = _inode2; \
        req->refs = ((_inode1) != NULL) + ((_inode2) != NULL); \
        SYS_LOCK(&dfc->lock, INT64_MAX, dfc_managed, (dfc, req, uuid)); \
    }

DFC_MANAGE(access,       true,  loc->inode,     NULL)
DFC_MANAGE(create,       false, loc->parent,    NULL)
DFC_MANAGE(entrylk,      true,  loc->parent,    NULL)
DFC_MANAGE(fentrylk,     true,  fd->inode,      NULL)
// TODO: Can flush, fsync and fsyncdir be really considered read-only ?
DFC_MANAGE(flush,        true,  fd->inode,      NULL)
DFC_MANAGE(fsync,        true,  fd->inode,      NULL)
DFC_MANAGE(fsyncdir,     true,  fd->inode,      NULL)
DFC_MANAGE(getxattr,     true,  loc->inode,     NULL)
DFC_MANAGE(fgetxattr,    true,  fd->inode,      NULL)
DFC_MANAGE(inodelk,      true,  loc->inode,     NULL)
DFC_MANAGE(finodelk,     true,  fd->inode,      NULL)
DFC_MANAGE(link,         false, oldloc->inode,  newloc->parent)
DFC_MANAGE(lk,           true,  fd->inode,      NULL)
DFC_MANAGE(lookup,       true,  loc->parent,    NULL)
DFC_MANAGE(mkdir,        false, loc->parent,    NULL)
DFC_MANAGE(mknod,        false, loc->parent,    NULL)
DFC_MANAGE(open,         true,  loc->inode,     NULL)
DFC_MANAGE(opendir,      true,  loc->inode,     NULL)
DFC_MANAGE(rchecksum,    true,  fd->inode,      NULL)
DFC_MANAGE(readdir,      true,  fd->inode,      NULL)
DFC_MANAGE(readdirp,     true,  fd->inode,      NULL)
DFC_MANAGE(readlink,     true,  loc->inode,     NULL)
DFC_MANAGE(readv,        true,  fd->inode,      NULL)
DFC_MANAGE(removexattr,  false, loc->inode,     NULL)
DFC_MANAGE(fremovexattr, false, fd->inode,      NULL)
DFC_MANAGE(rename,       false, oldloc->parent, newloc->parent)
DFC_MANAGE(rmdir,        false, loc->parent,    loc->inode)
DFC_MANAGE(setattr,      false, loc->inode,     NULL)
DFC_MANAGE(fsetattr,     false, fd->inode,      NULL)
DFC_MANAGE(setxattr,     false, loc->inode,     NULL)
DFC_MANAGE(fsetxattr,    false, fd->inode,      NULL)
DFC_MANAGE(stat,         true,  loc->inode,     NULL)
DFC_MANAGE(fstat,        true,  fd->inode,      NULL)
DFC_MANAGE(statfs,       true,  loc->inode,     NULL)
DFC_MANAGE(symlink,      false, loc->parent,    NULL)
DFC_MANAGE(truncate,     false, loc->inode,     NULL)
DFC_MANAGE(ftruncate,    false, fd->inode,      NULL)
DFC_MANAGE(unlink,       false, loc->parent,    loc->inode)
DFC_MANAGE(writev,       false, fd->inode,      NULL)
DFC_MANAGE(xattrop,      false, loc->inode,     NULL)
DFC_MANAGE(fxattrop,     false, fd->inode,      NULL)

#define DFC_FOP(_fop) \
    static int32_t dfc_##_fop(call_frame_t * frame, xlator_t * xl, \
                              SYS_ARGS_DECL((SYS_GF_ARGS_##_fop))) \
    { \
        uuid_t uuid; \
        int64_t txn; \
        dfc_manager_t * dfc = xl->private; \
        logI("DFC(" #_fop ")"); \
        err_t error = dfc_analyze(dfc, &xdata, uuid, &txn, \
                                  NULL, NULL); \
        if (error == ENOENT) \
        { \
            sys_gf_##_fop##_wind_tail(frame, FIRST_CHILD(xl), \
                                      SYS_ARGS_NAMES((SYS_GF_ARGS_##_fop))); \
            return 0; \
        } \
        if (error == 0) \
        { \
            logI("DFC(" #_fop ") managed"); \
            SYS_ASYNC( \
                dfc_managed_##_fop, (dfc, frame, xl, uuid, txn, \
                                     SYS_ARGS_NAMES((SYS_GF_ARGS_##_fop))) \
            ); \
            return 0; \
        } \
        sys_gf_##_fop##_unwind_error(frame, EIO, NULL); \
        return 0; \
    }

static int32_t dfc_lookup(call_frame_t * frame, xlator_t * xl,
                          SYS_ARGS_DECL((SYS_GF_ARGS_lookup)))
{
    uuid_t uuid;
    int64_t txn;
    size_t size;
    dfc_manager_t * dfc = xl->private;
    void * sort = NULL;
    logI("DFC(lookup)");
    err_t error = dfc_analyze(dfc, &xdata, uuid, &txn, &sort, &size);

    if (error == ENOENT)
    {
        sys_gf_lookup_wind_tail(frame, FIRST_CHILD(xl),
                                SYS_ARGS_NAMES((SYS_GF_ARGS_lookup)));
        return 0;
    }
    if (error == 0)
    {
        if (sort == NULL)
        {
            logI("DFC(lookup) managed");
            SYS_ASYNC(
                dfc_managed_lookup, (dfc, frame, xl, uuid, txn,
                                     SYS_ARGS_NAMES((SYS_GF_ARGS_lookup)))
            );
            return 0;
        }
        if ((loc->name == NULL) && (strcmp(loc->path, "/") == 0))
        {
            logI("DFC(lookup) init");
            SYS_FREE(sort);
            SYS_ASYNC(
                dfc_init_handler, (dfc, frame, xl, uuid, txn,
                                   SYS_ARGS_NAMES((SYS_GF_ARGS_lookup)))
            );
            return 0;
        }
    }

    sys_gf_lookup_unwind_error(frame, EIO, NULL);

    return 0;
}

static int32_t dfc_getxattr(call_frame_t * frame, xlator_t * xl,
                            SYS_ARGS_DECL((SYS_GF_ARGS_getxattr)))
{
    uuid_t uuid;
    int64_t txn;
    size_t size;
    dfc_manager_t * dfc = xl->private;
    void * sort = NULL;
    logI("DFC(getxattr)");
    dict_foreach(xdata, __dump_xdata, NULL);
    err_t error = dfc_analyze(dfc, &xdata, uuid, &txn, &sort, &size);

    if (error == ENOENT)
    {
        sys_gf_getxattr_wind_tail(frame, FIRST_CHILD(xl),
                                  SYS_ARGS_NAMES((SYS_GF_ARGS_getxattr)));
        return 0;
    }
    if (error == 0)
    {
        if (sort == NULL)
        {
            logI("DFC(getxattr) managed");
            SYS_ASYNC(
                dfc_managed_getxattr, (dfc, frame, xl, uuid, txn,
                                       SYS_ARGS_NAMES((SYS_GF_ARGS_getxattr)))
            );
            return 0;
        }
        if ((loc->name == NULL) && (strcmp(loc->path, "/") == 0))
        {
            logI("DFC(getxattr) sort");
            SYS_ASYNC(dfc_sort_handler, (dfc, frame, xl, uuid, txn, sort,
                                         size));
            return 0;
        }
        SYS_FREE(sort);
    }

    sys_gf_getxattr_unwind_error(frame, EIO, NULL);

    return 0;
}

DFC_FOP(access)
DFC_FOP(create)
DFC_FOP(entrylk)
DFC_FOP(fentrylk)
DFC_FOP(flush)
DFC_FOP(fsync)
DFC_FOP(fsyncdir)
DFC_FOP(fgetxattr)
DFC_FOP(inodelk)
DFC_FOP(finodelk)
DFC_FOP(link)
DFC_FOP(lk)
DFC_FOP(mkdir)
DFC_FOP(mknod)
DFC_FOP(open)
DFC_FOP(opendir)
DFC_FOP(rchecksum)
DFC_FOP(readdir)
DFC_FOP(readdirp)
DFC_FOP(readlink)
DFC_FOP(readv)
DFC_FOP(removexattr)
DFC_FOP(fremovexattr)
DFC_FOP(rename)
DFC_FOP(rmdir)
DFC_FOP(setattr)
DFC_FOP(fsetattr)
DFC_FOP(setxattr)
DFC_FOP(fsetxattr)
DFC_FOP(stat)
DFC_FOP(fstat)
DFC_FOP(statfs)
DFC_FOP(symlink)
DFC_FOP(truncate)
DFC_FOP(ftruncate)
DFC_FOP(unlink)
DFC_FOP(writev)
DFC_FOP(xattrop)
DFC_FOP(fxattrop)

static int32_t dfc_forget(xlator_t * this, inode_t * inode)
{
    return 0;
}

static int32_t dfc_invalidate(xlator_t * this, inode_t * inode)
{
    return 0;
}

static int32_t dfc_release(xlator_t * this, fd_t * fd)
{
    return 0;
}

static int32_t dfc_releasedir(xlator_t * this, fd_t * fd)
{
    return 0;
}

int32_t mem_acct_init(xlator_t * this)
{
    SYS_ASSERT(this != NULL, "Current translator is NULL");

    return SYS_CALL(
               xlator_mem_acct_init, (this, dfc_mt_end + 1),
               E(),
               RETERR()
           );
}

int32_t init(xlator_t * this)
{
    dfc_manager_t * dfc;
    err_t error;

    SYS_ASSERT(this != NULL, "Current translator is NULL");

    SYS_CALL(
        gfsys_initialize, (NULL, false),
        E(),
        RETVAL(-1)
    );

    SYS_TEST(
        this->parents != NULL,
        EINVAL,
        E(),
        LOG(E(), "Volume does not have a parent"),
        GOTO(failed, &error)
    );

    SYS_MALLOC0(
        &dfc, dfc_mt_dfc_manager_t,
        E(),
        GOTO(failed, &error)
    );

    sys_lock_initialize(&dfc->lock);
/*
    SYS_CALL(
        dfc_parse_options, (this),
        E(),
        GOTO(failed_dfc, &error)
    );
*/
    this->private = dfc;

    logD("The Distributed FOP Coordinator translator is ready");

    return 0;

failed:
    logE("The Distributed FOP Coordinator translator could not start. "
         "Error %d", error);

    return -1;
}

void fini(xlator_t * this)
{
    SYS_ASSERT(this != NULL, "Current translator is NULL");

    SYS_FREE(this->private);
}

SYS_GF_FOP_TABLE(dfc);
SYS_GF_CBK_TABLE(dfc);

struct volume_options options[] =
{
    { }
};