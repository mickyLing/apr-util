/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#include "apr.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_pools.h"
#include "apr_tables.h"
#include "apr_buckets.h"
#include "apr_errno.h"
#define APR_WANT_MEMFUNC
#define APR_WANT_STRFUNC
#include "apr_want.h"

#if APR_HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

static apr_status_t brigade_cleanup(void *data) 
{
    return apr_brigade_cleanup(data);
}

APU_DECLARE(apr_status_t) apr_brigade_cleanup(void *data)
{
    apr_bucket_brigade *b = data;
    apr_bucket *e;

    /*
     * Bah! We can't use APR_RING_FOREACH here because this bucket has
     * gone away when we dig inside it to get the next one.
     */
    while (!APR_BRIGADE_EMPTY(b)) {
        e = APR_BRIGADE_FIRST(b);
        apr_bucket_delete(e);
    }
    /*
     * We don't need to free(bb) because it's allocated from a pool.
     */
    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_destroy(apr_bucket_brigade *b)
{
    apr_pool_cleanup_kill(b->p, b, brigade_cleanup);
    return apr_brigade_cleanup(b);
}

APU_DECLARE(apr_bucket_brigade *) apr_brigade_create(apr_pool_t *p,
                                                     apr_bucket_alloc_t *list)
{
    apr_bucket_brigade *b;

    b = apr_palloc(p, sizeof(*b));
    b->p = p;
    b->bucket_alloc = list;

    APR_RING_INIT(&b->list, apr_bucket, link);

    apr_pool_cleanup_register(b->p, b, brigade_cleanup, apr_pool_cleanup_null);
    return b;
}

APU_DECLARE(apr_bucket_brigade *) apr_brigade_split(apr_bucket_brigade *b,
                                                    apr_bucket *e)
{
    apr_bucket_brigade *a;
    apr_bucket *f;

    a = apr_brigade_create(b->p, b->bucket_alloc);
    /* Return an empty brigade if there is nothing left in 
     * the first brigade to split off 
     */
    if (e != APR_BRIGADE_SENTINEL(b)) {
        f = APR_RING_LAST(&b->list);
        APR_RING_UNSPLICE(e, f, link);
        APR_RING_SPLICE_HEAD(&a->list, e, f, apr_bucket, link);
    }
    return a;
}

APU_DECLARE(apr_status_t) apr_brigade_partition(apr_bucket_brigade *b,
                                                apr_off_t point,
                                                apr_bucket **after_point)
{
    apr_bucket *e;
    const char *s;
    apr_size_t len;
    apr_status_t rv;

    if (point < 0) {
        /* this could cause weird (not necessarily SEGV) things to happen */
        return APR_EINVAL;
    }
    if (point == 0) {
        *after_point = APR_BRIGADE_FIRST(b);
        return APR_SUCCESS;
    }

    APR_BRIGADE_FOREACH(e, b) {
        if ((point > (apr_size_t)(-1)) && (e->length == (apr_size_t)(-1))) {
            /* XXX: point is too far out to simply split this bucket,
             * we must fix this bucket's size and keep going... */
            rv = apr_bucket_read(e, &s, &len, APR_BLOCK_READ);
            if (rv != APR_SUCCESS) {
                *after_point = e;
                return rv;
            }
        }
        if ((point < e->length) || (e->length == (apr_size_t)(-1))) {
            /* We already checked e->length -1 above, so we now
             * trust e->length < MAX_APR_SIZE_T.
             * First try to split the bucket natively... */
            if ((rv = apr_bucket_split(e, (apr_size_t)point)) 
                    != APR_ENOTIMPL) {
                *after_point = APR_BUCKET_NEXT(e);
                return rv;
            }

            /* if the bucket cannot be split, we must read from it,
             * changing its type to one that can be split */
            rv = apr_bucket_read(e, &s, &len, APR_BLOCK_READ);
            if (rv != APR_SUCCESS) {
                *after_point = e;
                return rv;
            }

            /* this assumes that len == e->length, which is okay because e
             * might have been morphed by the apr_bucket_read() above, but
             * if it was, the length would have been adjusted appropriately */
            if (point < e->length) {
                rv = apr_bucket_split(e, (apr_size_t)point);
                *after_point = APR_BUCKET_NEXT(e);
                return rv;
            }
        }
        if (point == e->length) {
            *after_point = APR_BUCKET_NEXT(e);
            return APR_SUCCESS;
        }
        point -= e->length;
    }
    *after_point = APR_BRIGADE_SENTINEL(b); 
    return APR_INCOMPLETE;
}

APU_DECLARE(apr_status_t) apr_brigade_length(apr_bucket_brigade *bb,
                                             int read_all, apr_off_t *length)
{
    apr_off_t total = 0;
    apr_bucket *bkt;

    APR_BRIGADE_FOREACH(bkt, bb) {
        if (bkt->length == (apr_size_t)(-1)) {
            const char *ignore;
            apr_size_t len;
            apr_status_t status;

            if (!read_all) {
                *length = -1;
                return APR_SUCCESS;
            }

            if ((status = apr_bucket_read(bkt, &ignore, &len,
                                          APR_BLOCK_READ)) != APR_SUCCESS) {
                return status;
            }
        }

        total += bkt->length;
    }

    *length = total;
    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_flatten(apr_bucket_brigade *bb,
                                              char *c, apr_size_t *len)
{
    apr_size_t actual = 0;
    apr_bucket *b;
 
    APR_BRIGADE_FOREACH(b, bb) {
        const char *str;
        apr_size_t str_len;
        apr_status_t status;

        status = apr_bucket_read(b, &str, &str_len, APR_BLOCK_READ);
        if (status != APR_SUCCESS) {
            return status;
        }

        /* If we would overflow. */
        if (str_len + actual > *len) {
            str_len = *len - actual;
        }

        /* XXX: It appears that overflow of the final bucket
         * is DISCARDED without any warning to the caller.
         */
        memcpy(c, str, str_len);

        c += str_len;
        actual += str_len;

        /* XXX: Is this a bug in actual == *len or did we intend to
         * flatten all trailing 0 byte buckets?
         */
        if (actual > *len) {
            break;
        }
    }

    *len = actual;
    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_pflatten(apr_bucket_brigade *bb,
                                               char **c,
                                               apr_size_t *len,
                                               apr_pool_t *pool)
{
    apr_off_t actual;
    apr_size_t total;
    apr_status_t rv;

    apr_brigade_length(bb, 1, &actual);
    
    /* XXX: This is dangerous beyond belief.  At least in the
     * apr_brigade_flatten case, the user explicitly stated their
     * buffer length - so we don't up and palloc 4GB for a single
     * file bucket.  This API must grow a useful max boundry,
     * either compiled-in or preset via the *len value.
     *
     * Shouldn't both fn's grow an additional return value for 
     * the case that the brigade couldn't be flattened into the
     * provided or allocated buffer (such as APR_EMOREDATA?)
     * Not a failure, simply an advisory result.
     */
    total = (apr_size_t)actual;

    *c = apr_palloc(pool, total);
    
    rv = apr_brigade_flatten(bb, *c, &total);

    if (rv != APR_SUCCESS) {
        return rv;
    }

    *len = total;
    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_split_line(apr_bucket_brigade *bbOut,
                                                 apr_bucket_brigade *bbIn,
                                                 apr_read_type_e block,
                                                 apr_off_t maxbytes)
{
    apr_off_t readbytes = 0;

    while (!APR_BRIGADE_EMPTY(bbIn)) {
        const char *pos;
        const char *str;
        apr_size_t len;
        apr_status_t rv;
        apr_bucket *e;

        e = APR_BRIGADE_FIRST(bbIn);
        rv = apr_bucket_read(e, &str, &len, block);

        if (rv != APR_SUCCESS) {
            return rv;
        }

        pos = memchr(str, APR_ASCII_LF, len);
        /* We found a match. */
        if (pos != NULL) {
            apr_bucket_split(e, pos - str + 1);
            APR_BUCKET_REMOVE(e);
            APR_BRIGADE_INSERT_TAIL(bbOut, e);
            return APR_SUCCESS;
        }
        APR_BUCKET_REMOVE(e);
        APR_BRIGADE_INSERT_TAIL(bbOut, e);
        readbytes += len;
        /* We didn't find an APR_ASCII_LF within the maximum line length. */
        if (readbytes >= maxbytes) {
            break;
        }
    }

    return APR_SUCCESS;
}


APU_DECLARE(apr_status_t) apr_brigade_to_iovec(apr_bucket_brigade *b, 
                                               struct iovec *vec, int *nvec)
{
    int left = *nvec;
    apr_bucket *e;
    struct iovec *orig;
    apr_size_t iov_len;
    apr_status_t rv;

    orig = vec;
    APR_BRIGADE_FOREACH(e, b) {
        if (left-- == 0)
            break;

        rv = apr_bucket_read(e, (const char **)&vec->iov_base, &iov_len,
                             APR_NONBLOCK_READ);
        if (rv != APR_SUCCESS)
            return rv;
        vec->iov_len = iov_len; /* set indirectly in case size differs */
        ++vec;
    }

    *nvec = vec - orig;
    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_vputstrs(apr_bucket_brigade *b, 
                                               apr_brigade_flush flush,
                                               void *ctx,
                                               va_list va)
{
    for (;;) {
        const char *str = va_arg(va, const char *);
        apr_status_t rv;

        if (str == NULL)
            break;

        rv = apr_brigade_write(b, flush, ctx, str, strlen(str));
        if (rv != APR_SUCCESS)
            return rv;
    }

    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_putc(apr_bucket_brigade *b,
                                           apr_brigade_flush flush, void *ctx,
                                           const char c)
{
    return apr_brigade_write(b, flush, ctx, &c, 1);
}

APU_DECLARE(apr_status_t) apr_brigade_write(apr_bucket_brigade *b,
                                            apr_brigade_flush flush,
                                            void *ctx, 
                                            const char *str, apr_size_t nbyte)
{
    apr_bucket *e = APR_BRIGADE_LAST(b);
    char *buf;

    if (!APR_BRIGADE_EMPTY(b) && APR_BUCKET_IS_HEAP(e) &&
        (e->length + nbyte <= ((apr_bucket_heap *)e->data)->alloc_len))
    {
        /* there is a sufficiently big buffer bucket available */
        apr_bucket_heap *h = e->data;
        buf = h->base + e->start + e->length;
    }
    else {
        if (nbyte > APR_BUCKET_BUFF_SIZE) {
            /* too big to buffer */
            if (flush) {
                e = apr_bucket_transient_create(str, nbyte, b->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(b, e);
                return flush(b, ctx);
            }
            else {
                e = apr_bucket_heap_create(str, nbyte, NULL, b->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(b, e);
                return APR_SUCCESS;
            }
        }
        else {
            /* bigger than the current buffer can handle, but we don't
             * mind making a new buffer */
            buf = apr_bucket_alloc(APR_BUCKET_BUFF_SIZE, b->bucket_alloc);
            e = apr_bucket_heap_create(buf, APR_BUCKET_BUFF_SIZE,
                                       apr_bucket_free, b->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(b, e);
            e->length = 0;   /* We are writing into the brigade, and
                              * allocating more memory than we need.  This
                              * ensures that the bucket thinks it is empty just
                              * after we create it.  We'll fix the length
                              * once we put data in it below.
                              */
        }
    }

    memcpy(buf, str, nbyte);
    e->length += nbyte;

    return APR_SUCCESS;
}

APU_DECLARE(apr_status_t) apr_brigade_puts(apr_bucket_brigade *b,
                                           apr_brigade_flush flush, void *ctx,
                                           const char *str)
{
    apr_bucket *e = APR_BRIGADE_LAST(b);
    if (!APR_BRIGADE_EMPTY(b) && APR_BUCKET_IS_HEAP(e)) {
        /* If there is some space available in a heap bucket
         * at the end of the brigade, start copying the string
         */
        apr_bucket_heap *h = e->data;
        char *buf = h->base + e->start + e->length;
        apr_size_t bytes_avail = h->alloc_len - e->length;
        const char *s = str;

        while (bytes_avail && *s) {
            *buf++ = *s++;
            bytes_avail--;
        }
        e->length += (s - str);
        if (!*s) {
            return APR_SUCCESS;
        }
        str = s;
    }

    /* If the string has not been copied completely to the brigade,
     * delegate the remaining work to apr_brigade_write(), which
     * knows how to grow the brigade
     */
    return apr_brigade_write(b, flush, ctx, str, strlen(str));
}

APU_DECLARE_NONSTD(apr_status_t) apr_brigade_putstrs(apr_bucket_brigade *b, 
                                                     apr_brigade_flush flush,
                                                     void *ctx, ...)
{
    va_list va;
    apr_status_t rv;

    va_start(va, ctx);
    rv = apr_brigade_vputstrs(b, flush, ctx, va);
    va_end(va);
    return rv;
}

APU_DECLARE_NONSTD(apr_status_t) apr_brigade_printf(apr_bucket_brigade *b, 
                                                    apr_brigade_flush flush,
                                                    void *ctx, 
                                                    const char *fmt, ...)
{
    va_list ap;
    apr_status_t rv;

    va_start(ap, fmt);
    rv = apr_brigade_vprintf(b, flush, ctx, fmt, ap);
    va_end(ap);
    return rv;
}

struct brigade_vprintf_data_t {
    apr_vformatter_buff_t vbuff;

    apr_bucket_brigade *b;  /* associated brigade */
    apr_brigade_flush *flusher; /* flushing function */
    void *ctx;

    char *cbuff; /* buffer to flush from */
};

static apr_status_t brigade_flush(apr_vformatter_buff_t *buff)
{
    /* callback function passed to ap_vformatter to be
     * called when vformatter needs to buff and
     * buff.curpos > buff.endpos
     */

    /* "downcast," have really passed a brigade_vprintf_data_t* */
    struct brigade_vprintf_data_t *vd = (struct brigade_vprintf_data_t*)buff;
    apr_status_t res = APR_SUCCESS;

    res = apr_brigade_write(vd->b, *vd->flusher, vd->ctx, vd->cbuff,
                          APR_BUCKET_BUFF_SIZE);

    if(res != APR_SUCCESS) {
      return -1;
    }

    vd->vbuff.curpos = vd->cbuff;
    vd->vbuff.endpos = vd->cbuff + APR_BUCKET_BUFF_SIZE;

    return res;
}

APU_DECLARE(apr_status_t) apr_brigade_vprintf(apr_bucket_brigade *b,
                                              apr_brigade_flush flush,
                                              void *ctx,
                                              const char *fmt, va_list va)
{
    /* the cast, in order of appearance */
    struct brigade_vprintf_data_t vd;
    char buf[APR_BUCKET_BUFF_SIZE];
    apr_size_t written;

    vd.vbuff.curpos = buf;
    vd.vbuff.endpos = buf + APR_BUCKET_BUFF_SIZE;
    vd.b = b;
    vd.flusher = &flush;
    vd.ctx = ctx;
    vd.cbuff = buf;

    written = apr_vformatter(brigade_flush, &vd.vbuff, fmt, va);

    if (written == -1) {
      return -1;
    }

    /* tack on null terminator to remaining string */
    *(vd.vbuff.curpos) = '\0';

    /* write out what remains in the buffer */
    return apr_brigade_write(b, flush, ctx, buf, vd.vbuff.curpos - buf);
}

