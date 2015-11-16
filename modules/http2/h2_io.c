/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>

#include "h2_private.h"
#include "h2_io.h"
#include "h2_response.h"
#include "h2_task.h"
#include "h2_util.h"

h2_io *h2_io_create(int id, apr_pool_t *pool, apr_bucket_alloc_t *bucket_alloc)
{
    h2_io *io = apr_pcalloc(pool, sizeof(*io));
    if (io) {
        io->id = id;
        io->pool = pool;
        io->bucket_alloc = bucket_alloc;
        io->bbin = NULL;
        io->bbout = NULL;
    }
    return io;
}

void h2_io_destroy(h2_io *io)
{
    if (io->pool) {
        apr_pool_destroy(io->pool);
        /* gone */
    }
}

void h2_io_set_response(h2_io *io, h2_response *response) 
{
    AP_DEBUG_ASSERT(io->pool);
    AP_DEBUG_ASSERT(response);
    AP_DEBUG_ASSERT(!io->response);
    io->response = h2_response_copy(io->pool, response);
    if (response->rst_error) {
        h2_io_rst(io, response->rst_error);
    }
}


void h2_io_rst(h2_io *io, int error)
{
    io->rst_error = error;
    io->eos_in = 1;
}

int h2_io_in_has_eos_for(h2_io *io)
{
    return io->eos_in || (io->bbin && h2_util_has_eos(io->bbin, 0));
}

int h2_io_out_has_data(h2_io *io)
{
    return io->bbout && h2_util_bb_has_data_or_eos(io->bbout);
}

apr_off_t h2_io_out_length(h2_io *io)
{
    if (io->bbout) {
        apr_off_t len = 0;
        apr_brigade_length(io->bbout, 0, &len);
        return (len > 0)? len : 0;
    }
    return 0;
}

apr_status_t h2_io_in_read(h2_io *io, apr_bucket_brigade *bb, 
                           apr_size_t maxlen)
{
    apr_off_t start_len = 0;
    apr_bucket *last;
    apr_status_t status;

    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    
    if (!io->bbin || APR_BRIGADE_EMPTY(io->bbin)) {
        return io->eos_in? APR_EOF : APR_EAGAIN;
    }
    
    apr_brigade_length(bb, 1, &start_len);
    last = APR_BRIGADE_LAST(bb);
    status = h2_util_move(bb, io->bbin, maxlen, NULL, "h2_io_in_read");
    if (status == APR_SUCCESS) {
        apr_bucket *nlast = APR_BRIGADE_LAST(bb);
        apr_off_t end_len = 0;
        apr_brigade_length(bb, 1, &end_len);
        if (last == nlast) {
            return APR_EAGAIN;
        }
        io->input_consumed += (end_len - start_len);
    }
    return status;
}

apr_status_t h2_io_in_write(h2_io *io, apr_bucket_brigade *bb)
{
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    
    if (io->eos_in) {
        return APR_EOF;
    }
    io->eos_in = h2_util_has_eos(bb, 0);
    if (!APR_BRIGADE_EMPTY(bb)) {
        if (!io->bbin) {
            io->bbin = apr_brigade_create(io->pool, io->bucket_alloc);
        }
        return h2_util_move(io->bbin, bb, 0, NULL, "h2_io_in_write");
    }
    return APR_SUCCESS;
}

apr_status_t h2_io_in_close(h2_io *io)
{
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    
    if (io->bbin) {
        APR_BRIGADE_INSERT_TAIL(io->bbin, 
                                apr_bucket_eos_create(io->bbin->bucket_alloc));
    }
    io->eos_in = 1;
    return APR_SUCCESS;
}

apr_status_t h2_io_out_readx(h2_io *io,  
                             h2_io_data_cb *cb, void *ctx, 
                             apr_off_t *plen, int *peos)
{
    apr_status_t status;
    
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    
    if (io->eos_out) {
        *plen = 0;
        *peos = 1;
        return APR_SUCCESS;
    }
    else if (!io->bbout) {
        *plen = 0;
        *peos = 0;
        return APR_EAGAIN;
    }
    
    if (cb == NULL) {
        /* just checking length available */
        status = h2_util_bb_avail(io->bbout, plen, peos);
    }
    else {
        status = h2_util_bb_readx(io->bbout, cb, ctx, plen, peos);
        if (status == APR_SUCCESS) {
            io->eos_out = *peos;
        }
    }
    
    return status;
}

apr_status_t h2_io_out_read_to(h2_io *io, apr_bucket_brigade *bb, 
                               apr_off_t *plen, int *peos)
{
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    
    if (io->eos_out) {
        *plen = 0;
        *peos = 1;
        return APR_SUCCESS;
    }
    else if (!io->bbout) {
        *plen = 0;
        *peos = 0;
        return APR_EAGAIN;
    }

    io->eos_out = *peos = h2_util_has_eos(io->bbout, *plen);
    return h2_util_move(bb, io->bbout, *plen, NULL, "h2_io_read_to");
}

apr_status_t h2_io_out_write(h2_io *io, apr_bucket_brigade *bb, 
                             apr_size_t maxlen, int *pfile_handles_allowed)
{
    apr_status_t status;
    int start_allowed;
    
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }

    if (io->eos_out) {
        apr_off_t len;
        /* We have already delivered an EOS bucket to a reader, no
         * sense in storing anything more here.
         */
        status = apr_brigade_length(bb, 1, &len);
        if (status == APR_SUCCESS) {
            if (len > 0) {
                /* someone tries to write real data after EOS, that
                 * does not look right. */
                status = APR_EOF;
            }
            /* cleanup, as if we had moved the data */
            apr_brigade_cleanup(bb);
        }
        return status;
    }

    if (!io->bbout) {
        io->bbout = apr_brigade_create(io->pool, io->bucket_alloc);
    }
    
    /* Let's move the buckets from the request processing in here, so
     * that the main thread can read them when it has time/capacity.
     *
     * Move at most "maxlen" memory bytes. If buckets remain, it is
     * the caller's responsibility to take care of this.
     *
     * We allow passing of file buckets as long as we do not have too
     * many open files already buffered. Otherwise we will run out of
     * file handles.
     */
    start_allowed = *pfile_handles_allowed;
    status = h2_util_move(io->bbout, bb, maxlen, pfile_handles_allowed, 
                          "h2_io_out_write");
    /* track # file buckets moved into our pool */
    if (start_allowed != *pfile_handles_allowed) {
        io->files_handles_owned += (start_allowed - *pfile_handles_allowed);
    }
    return status;
}


apr_status_t h2_io_out_close(h2_io *io)
{
    if (io->rst_error) {
        return APR_ECONNABORTED;
    }
    if (!io->bbout) {
        io->bbout = apr_brigade_create(io->pool, io->bucket_alloc);
    }
    if (!io->eos_out && !h2_util_has_eos(io->bbout, 0)) {
        APR_BRIGADE_INSERT_TAIL(io->bbout, 
                                apr_bucket_eos_create(io->bbout->bucket_alloc));
    }
    return APR_SUCCESS;
}
