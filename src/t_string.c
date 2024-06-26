/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <math.h> /* isnan(), isinf() */
#include "server.h"

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

static int checkStringLength(client *c, long long size)
{
    if (size > 512 * 1024 * 1024) {
        addReplyError(c, "string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1 << 0) /* Set if key not exists. */
#define OBJ_SET_XX (1 << 1) /* Set if key exists. */
#define OBJ_SET_EX (1 << 2) /* Set if time in seconds is given */
#define OBJ_SET_PX (1 << 3) /* Set if time in ms in given */

/* Q-Redis: since we only have on writer thread which is server thread, we do
 * not need rcu_read_lock(), and rcu_read_unlock() block for write/update redis
 * command. And one advantage of not using rcu_read_(un)lock block is the
 * write/update command function can use syncrhonize_rcu() to free val instead
 * of call_rcu
 * */

void setGenericCommand(client *c,
                       int flags,
                       robj *key,
                       robj *val,
                       robj *expire,
                       int unit,
                       robj *ok_reply,
                       robj *abort_reply)
{
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    if (expire) {
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) !=
            C_OK)
            return;
        if (milliseconds <= 0) {
            addReplyErrorFormat(c, "invalid expire time in %s", c->cmd->name);
            return;
        }
        if (unit == UNIT_SECONDS)
            milliseconds *= 1000;
    }

    // ToDo: one rcu read lock covers both expires and db dict?
    // We do not need rcu read lock for write/update commands.
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db, key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db, key) == NULL)) {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
    setKey(c->db, key, val);
    server.dirty++;
    if (expire)
        setExpire(c->db, key, mstime() + milliseconds);
    notifyKeyspaceEvent(NOTIFY_STRING, "set", key, c->db->id);
    if (expire)
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setCommand(client *c)
{
    int j;
    robj *expire = NULL;
    robj *key = NULL;
    robj *val = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_SET_NO_FLAGS;

    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc - 1) ? NULL : c->argv[j + 1];

        if ((a[0] == 'n' || a[0] == 'N') && (a[1] == 'x' || a[1] == 'X') &&
            a[2] == '\0' && !(flags & OBJ_SET_XX)) {
            flags |= OBJ_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX)) {
            flags |= OBJ_SET_XX;
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_PX) && next) {
            flags |= OBJ_SET_EX;
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_EX) && next) {
            flags |= OBJ_SET_PX;
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
    }

    // c->argv[2] = tryObjectEncoding(c->argv[2]);
    // copy key and val string, instead using original c->argv, as ref increment
    // in server thread may not be seen in worker thread, which can result in
    // panic or memory leak.
    val = dupStringObject(c->argv[2]);
    key = dupStringObject(c->argv[1]);
    setGenericCommand(c, flags, key, val, expire, unit, NULL, NULL);
    decrRefCount(val);
    decrRefCount(key);
}

void setnxCommand(client *c)
{
    robj *key = NULL;
    robj *val = NULL;

    key = dupStringObject(c->argv[1]);
    val = dupStringObject(c->argv[2]);
    // c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c, OBJ_SET_NX, key, val, NULL, 0, shared.cone,
                      shared.czero);
    decrRefCount(key);
    decrRefCount(val);
}

void setexCommand(client *c)
{
    robj *key = NULL;
    robj *val = NULL;

    key = dupStringObject(c->argv[1]);
    val = dupStringObject(c->argv[3]);
    // c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2],
                      UNIT_SECONDS, NULL, NULL);
    decrRefCount(key);
    decrRefCount(val);
}

void psetexCommand(client *c)
{
    robj *key = NULL;
    robj *val = NULL;

    key = dupStringObject(c->argv[1]);
    val = dupStringObject(c->argv[3]);
    // c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2],
                      UNIT_MILLISECONDS, NULL, NULL);
    decrRefCount(key);
    decrRefCount(val);
}

int getGenericCommand(client *c)
{
    robj *o;

    rcu_read_lock();
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL) {
        rcu_read_unlock();
        return C_OK;
    }

    if (o->type != OBJ_STRING) {
        addReply(c, shared.wrongtypeerr);
        rcu_read_unlock();
        return C_ERR;
    } else {
        addReplyBulk(c, o);
        rcu_read_unlock();
        return C_OK;
    }
}

void getCommand(client *c)
{
    getGenericCommand(c);
}

void getsetCommand(client *c)
{
    robj *key = NULL;
    robj *val = NULL;

    if (getGenericCommand(c) == C_ERR)
        return;
    // c->argv[2] = tryObjectEncoding(c->argv[2]);
    key = dupStringObject(c->argv[1]);
    val = dupStringObject(c->argv[2]);
    setKey(c->db, key, val);
    notifyKeyspaceEvent(NOTIFY_STRING, "set", key, c->db->id);
    server.dirty++;
    decrRefCount(key);
    decrRefCount(val);
}

void setrangeCommand(client *c)
{
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    if (getLongFromObjectOrReply(c, c->argv[2], &offset, NULL) != C_OK)
        return;

    if (offset < 0) {
        addReplyError(c, "offset is out of range");
        return;
    }

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            addReply(c, shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c, offset + sdslen(value)) != C_OK)
            return;

        o = createObject(OBJ_STRING, sdsnewlen(NULL, offset + sdslen(value)));
        dbAdd(c->db, c->argv[1], o);
    } else {
        size_t olen;

        /* Key exists, check type */
        if (checkType(c, o, OBJ_STRING))
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c, olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c, offset + sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db, c->argv[1], o);
    }

    if (sdslen(value) > 0) {
        sds oc = NULL;
        sds nc = NULL;
        oc = o->ptr;
        nc = sdsdup(o->ptr);
        nc = sdsgrowzero(nc, offset + sdslen(value));
        memcpy((char *) nc + offset, value, sdslen(value));
        rcu_assign_pointer(o->ptr, nc);
        synchronize_rcu();
        sdsfree(oc);
        signalModifiedKey(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING, "setrange", c->argv[1], c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c, sdslen(o->ptr));
}

void getrangeCommand(client *c)
{
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    rcu_read_lock();

    if (getLongLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) {
        rcu_read_unlock();
        return;
    }
    if (getLongLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK) {
        rcu_read_unlock();
        return;
    }
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptybulk)) == NULL ||
        checkType(c, o, OBJ_STRING)) {
        rcu_read_unlock();
        return;
    }
    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf, sizeof(llbuf), (long) o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) {
        addReply(c, shared.emptybulk);
        rcu_read_unlock();
        return;
    }
    if (start < 0)
        start = strlen + start;
    if (end < 0)
        end = strlen + end;
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    if ((unsigned long long) end >= strlen)
        end = strlen - 1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        addReply(c, shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c, (char *) str + start, end - start + 1);
    }
    rcu_read_unlock();
}

void mgetCommand(client *c)
{
    int j;

    rcu_read_lock();
    addReplyMultiBulkLen(c, c->argc - 1);
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db, c->argv[j]);
        if (o == NULL) {
            addReply(c, shared.nullbulk);
        } else {
            if (o->type != OBJ_STRING) {
                addReply(c, shared.nullbulk);
            } else {
                addReplyBulk(c, o);
            }
        }
    }
    rcu_read_unlock();
}

void msetGenericCommand(client *c, int nx)
{
    int j, busykeys = 0;

    if ((c->argc % 2) == 0) {
        addReplyError(c, "wrong number of arguments for MSET");
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            robj *key = dupStringObject(c->argv[j]);
            if (lookupKeyWrite(c->db, key) != NULL) {
                busykeys++;
            }
            decrRefCount(key);
        }
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

    for (j = 1; j < c->argc; j += 2) {
        robj *key = dupStringObject(c->argv[j]);
        robj *val = dupStringObject(c->argv[j + 1]);
        // c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db, key, val);
        notifyKeyspaceEvent(NOTIFY_STRING, "set", key, c->db->id);
        decrRefCount(key);
        decrRefCount(val);
    }
    server.dirty += (c->argc - 1) / 2;
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(client *c)
{
    msetGenericCommand(c, 0);
}

void msetnxCommand(client *c)
{
    msetGenericCommand(c, 1);
}

void incrDecrCommand(client *c, long long incr)
{
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_STRING))
        return;
    if (getLongLongFromObjectOrReply(c, o, &value, NULL) != C_OK)
        return;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN - oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX - oldvalue))) {
        addReplyError(c, "increment or decrement would overflow");
        return;
    }
    value += incr;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) && value >= LONG_MIN &&
        value <= LONG_MAX) {
        new = o;
        // o->ptr = (void*)((long)value);
        rcu_assign_pointer(o->ptr, (void *) ((long) value));
    } else {
        new = createStringObjectFromLongLong(value);
        if (o) {
            dbOverwrite(c->db, c->argv[1], new);
        } else {
            dbAdd(c->db, c->argv[1], new);
        }
    }
    signalModifiedKey(c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING, "incrby", c->argv[1], c->db->id);
    server.dirty++;
    addReply(c, shared.colon);
    addReply(c, new);
    addReply(c, shared.crlf);
}

void incrCommand(client *c)
{
    incrDecrCommand(c, 1);
}

void decrCommand(client *c)
{
    incrDecrCommand(c, -1);
}

void incrbyCommand(client *c)
{
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;
    incrDecrCommand(c, incr);
}

void decrbyCommand(client *c)
{
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;
    incrDecrCommand(c, -incr);
}

void incrbyfloatCommand(client *c)
{
    long double incr, value;
    robj *o, *new, *aux;

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_STRING))
        return;
    if (getLongDoubleFromObjectOrReply(c, o, &value, NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c, "increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value, 1);
    if (o)
        dbOverwrite(c->db, c->argv[1], new);
    else
        dbAdd(c->db, c->argv[1], new);
    signalModifiedKey(c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING, "incrbyfloat", c->argv[1], c->db->id);
    server.dirty++;
    addReplyBulk(c, new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET", 3);
    rewriteClientCommandArgument(c, 0, aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c, 2, new);
}

void appendCommand(client *c)
{
    size_t totlen;
    robj *o, *append;
    robj *key = dupStringObject(c->argv[1]);
    robj *val = dupStringObject(c->argv[2]);

    o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        /* Create the key */
        // c->argv[2] = tryObjectEncoding(c->argv[2]);
        // dbAdd(c->db,c->argv[1],c->argv[2]);
        dbAdd(c->db, key, val);
        incrRefCount(val);
        totlen = stringObjectLen(val);
    } else {
        sds oc = NULL;
        sds nc = NULL;

        /* Key exists, check type */
        if (checkType(c, o, OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = val;
        totlen = stringObjectLen(o) + sdslen(append->ptr);
        if (checkStringLength(c, totlen) != C_OK) {
            decrRefCount(key);
            decrRefCount(val);
            return;
        }

        /* Append the value */
        o = dbUnshareStringValue(c->db, key, o);
        // o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        nc = sdscatlen(o->ptr, append->ptr, sdslen(append->ptr));
        oc = o->ptr;
        rcu_assign_pointer(o->ptr, nc);
        synchronize_rcu();
        sdsfree(oc);
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db, key);
    notifyKeyspaceEvent(NOTIFY_STRING, "append", key, c->db->id);
    server.dirty++;
    addReplyLongLong(c, totlen);
    decrRefCount(key);
    decrRefCount(val);
}

void strlenCommand(client *c)
{
    robj *o;
    rcu_read_lock();
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, o, OBJ_STRING)) {
        rcu_read_unlock();
        return;
    }
    addReplyLongLong(c, stringObjectLen(o));
    rcu_read_unlock();
}
