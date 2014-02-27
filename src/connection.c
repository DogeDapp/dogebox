
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* fff needs libuv */
#include <uv.h>

/* for uint32_t */
#include <stdint.h>

#include "bitfield.h"
#include "dogebox.h"
#include "dogebox_connection.h"
#include "dogebox_msghandler.h"

/* for sys_t */
#include "linked_list_queue.h"

/* for iterating through f2p hashmap */
#include "linked_list_hashmap.h"

/* for f2p_t */
#include "file2piece_mapper.h"

/* for filewatcher */
#include "fff.h"

/* dogebox local needs dm_stats_t */
#include "bt.h"

/* for piece database */
#include "bt_piece_db.h"

/* for sys_t */
#include "dogebox_local.h"

/* for filelog reading */
#include "bencode.h"

/* for piggy backing on pwp_conn_private_t */
#include "sparse_counter.h"
#include "pwp_connection.h"
#include "pwp_connection_private.h"

typedef struct {
    pwp_conn_private_t pwp_conn;
    void* udata;
} conn_private_t;

of_conn_t* of_conn_new(of_conn_cb_t* cb, void* udata)
{
    conn_private_t *me;

    me = calloc(1, sizeof(conn_private_t));
    me->udata = udata;
    return (of_conn_t*)me;
}

#if 0
void of_conn_keepalive(of_conn_t* pco)
{
#if 0
    fake_pc_t* pc = (void*)pco;
    pc->mtype = OF_MSGTYPE_KEEPALIVE;
#endif
}

void of_conn_fulllog(of_conn_t* pco, msg_fulllog_t *m)
{
#if 0
    fake_pc_t* pc = (void*)pco;
    pc->mtype = OF_MSGTYPE_FULLLOG;
    memcpy(&pc->fulllog, m, sizeof(msg_fulllog_t));
#endif
}

void of_conn_pwp(of_conn_t* pco, msg_pwp_t *m)
{
#if 0
    fake_pc_t* pc = (void*)pco;
    pc->mtype = OF_MSGTYPE_FULLLOG;
    memcpy(&pc->fulllog, m, sizeof(msg_pwp_t));
#endif
}

static int of_conn_pwp_dispatch(void *pc_,
        const unsigned char* buf, unsigned int len)
{
//    uv_mutex_lock(&me->mutex);
//    bt_dm_dispatch_from_buffer(me->bc,peer_nethandle,buf,len);
//    uv_mutex_unlock(&me->mutex);
    return 0;
}
#endif

#if 0
void of_conn_filelog(of_conn_t* pco, char* filelog, int len)
{
    fake_pc_t* pc = (void*)pco;
    pc->mtype = OF_MSGTYPE_FULLLOG;
    memcpy(&pc->fulllog, m, sizeof(msg_fulllog_t));
}
#endif

#if 0
void of_conn_piecelog(of_conn_t* pco, char* filelog, int len)
{
    fake_pc_t* pc = (void*)pco;
    pc->mtype = OF_MSGTYPE_FULLLOG;
    memcpy(&pc->fulllog, m, sizeof(msg_fulllog_t));
}
#endif

static void __process_file_dict(conn_private_t* me, bencode_t* d)
{
    /* piece mappper */
    void* pm;

    // TODO: switch away from path
    char path[1000];
    const char *ppath = path;
    int pathlen = 0;
    long int fsize = 0;
    long int piece_idx = 0;
    long int pieces = 0;
    long int mtime = 0;

    pm = ((sys_t*)me->udata)->pm;

    while (bencode_dict_has_next(d))
    {
        bencode_t benk;
        const char *key;
        int klen;

        bencode_dict_get_next(d, &benk, &key, &klen);

        if (!strncmp(key, "path", klen))
        {
            bencode_string_value(&benk, &ppath, &pathlen);
        }
        else if (!strncmp(key, "size", klen))
        {
            bencode_int_value(&benk, &fsize);
        }
        else if (!strncmp(key, "is_deleted", klen))
        {
            //bencode_string_value(&benk, &fsize);
        }
        else if (!strncmp(key, "piece_idx", klen))
        {
            bencode_int_value(&benk, &piece_idx);
        }
        else if (!strncmp(key, "pieces", klen))
        {
            bencode_int_value(&benk, &pieces);
        }
        else if (!strncmp(key, "mtime", klen))
        {
            bencode_int_value(&benk, &mtime);
        }
    }

    file_t* f = f2p_get_file_from_path(pm, path);

    if (!f)
    {
        /* files from file logs are files by default */
        f2p_file_added(pm, path, 0, fsize, mtime);
    }
    else if (f->mtime < mtime)
    {
        if (f->size != fsize)
        {
            f2p_file_changed(pm, path, fsize, mtime);
        }
        else
        {
            f2p_file_remap(pm, path, piece_idx, pieces);
        }
    }
}

void of_conn_filelog(void* pc, const unsigned char* buf, unsigned int len)
{
    conn_private_t* me = pc;      
    bencode_t ben;

    printf("Received filelog: '%.*s'\n", len, buf);

    bencode_init(&ben, buf, len);
    if (!bencode_is_list(&ben))
    {
        printf("bad file log, expected list\n");
        return;
    }

    while (bencode_list_has_next(&ben))
    {
        bencode_t dict;

        bencode_list_get_next(&ben, &dict);
        __process_file_dict(me, &dict);
    }
}

void of_conn_piecelog(void* pc, const unsigned char* buf, unsigned int len)
{
    conn_private_t* me = pc;      
    bencode_t ben;

    /* piece database */
    void* db = ((sys_t*)me->udata)->pm;

    printf("Received piecelog: '%.*s'\n", len, buf);

    bencode_init(&ben, buf, len);
    if (!bencode_is_list(&ben))
    {
        printf("bad file log, expected list\n");
        return;
    }

    while (bencode_list_has_next(&ben))
    {
        bencode_t dict;

        bencode_list_get_next(&ben, &dict);

        // TODO: switch away from path
        long int piece_idx = 0;
        long int piece_size = 0;
        long int mtime = 0;
        unsigned char hash[20];
        int hash_len = 0;

        while (bencode_dict_has_next(&dict))
        {
            bencode_t benk;
            const char *key;
            int klen;

            bencode_dict_get_next(&dict, &benk, &key, &klen);

            if (!strncmp(key, "idx", klen))
            {
                bencode_int_value(&benk, &piece_idx);
            }
            else if (!strncmp(key, "size", klen))
            {
                bencode_int_value(&benk, &piece_size);
            }
            else if (!strncmp(key, "hash", klen))
            {
                bencode_string_value(&benk, (const char**)&hash, &hash_len);
            }
            else if (!strncmp(key, "mtime", klen))
            {
                bencode_int_value(&benk, &mtime);
            }
        }

        void* p = bt_piecedb_get(db, (int)piece_idx);

        assert(p);
    }
}
