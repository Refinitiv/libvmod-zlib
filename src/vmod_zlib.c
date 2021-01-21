/*
 * Copyright 2019 Refinitiv
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

/* need vcl.h before vrt.h for vmod_evet_f typedef */
#include "vdef.h"
#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"

/* we use libvgz to have {start,stop,last}_bit */
#include "lib/libvgz/vgz.h"
#include "vcc_zlib_if.h"
#include "vsb.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define READ_BUFFER_SIZE 8192

#ifdef VMOD_ZLIB_DEBUG
#include <syslog.h>
#define DEBUG(x) x
#else
#define DEBUG(x) (void)NULL
#endif

static const struct gethdr_s VGC_HDR_REQ_Content_2d_Length =
    { HDR_REQ, "\017Content-Length:"};

static void
clean(void *priv)
{
	struct vsb	**pvsb;
	struct vsb	*vsb;

	if (priv) {
		pvsb = (struct vsb **)priv;
		CAST_OBJ_NOTNULL(vsb, *pvsb, VSB_MAGIC);
		DEBUG(syslog(LOG_INFO, "zlib: destroy vsb %lu", (uintptr_t)vsb));
		DEBUG(syslog(LOG_INFO, "zlib: free %lu", (uintptr_t)pvsb));
		VSB_destroy(&vsb);
		free(pvsb);
	}
}

static struct vsb**
VSB_get(VRT_CTX, struct vmod_priv *priv)
{
	struct vsb **pvsb;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(priv);
	if(priv->priv == NULL) {
		priv->priv = malloc(sizeof(pvsb));
		pvsb = (struct vsb **)priv->priv;
		*pvsb = VSB_new_auto();
		DEBUG(syslog(LOG_INFO, "zlib: malloc %lu", (uintptr_t)pvsb));
		DEBUG(syslog(LOG_INFO, "zlib: new_auto vsb %lu", (uintptr_t)*pvsb));
		priv->free = clean;
	}
	else {
		pvsb = (struct vsb **)priv->priv;
		CHECK_OBJ_NOTNULL(*pvsb, VSB_MAGIC);
	}
	return (pvsb);
}

static int
fill_pipeline(VRT_CTX, struct vsb** pvsb, struct http_conn *htc, ssize_t len)
{
	struct vsb	*vsb;
	char		buffer[READ_BUFFER_SIZE];
	ssize_t		l;
	ssize_t		i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CAST_OBJ_NOTNULL(vsb, *pvsb, VSB_MAGIC);
	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	AN(len > 0);
	l = 0;
	if (htc->pipeline_b) {
		l = htc->pipeline_e - htc->pipeline_b;
		AN(l > 0);
		if (l >= len) {
			return (l);
		}
		AZ(VSB_bcat(vsb, htc->pipeline_b, l));
		len -= l;
	}

	i = 0;
	while (len > 0) {
		i = read(htc->fd, buffer, READ_BUFFER_SIZE);
		if (i < 0) {
			if (htc->pipeline_b == htc->pipeline_e) {
				htc->pipeline_b = NULL;
				htc->pipeline_e = NULL;
			}
			// XXX: VTCP_Assert(i); // but also: EAGAIN
			VSLb(ctx->req->htc->vfc->wrk->vsl, SLT_FetchError,
			    "%s", strerror(errno));
			ctx->req->req_body_status = REQ_BODY_FAIL;
			return (i);
		}
		AZ(VSB_bcat(vsb, buffer, i));
		len -= i;
	}
	VSB_finish(vsb);
	AN(VSB_len(vsb) > 0);
	htc->pipeline_b = VSB_data(vsb);
	htc->pipeline_e = htc->pipeline_b + VSB_len(vsb);
	return (VSB_len(vsb));
}

void
log_stream(VRT_CTX, z_stream *stream) {
	VSC_C_main->n_gunzip++;
	VSLb(ctx->vsl, SLT_Gzip, "%s %jd %jd %jd %jd %jd",
	    "U D -",
	    (intmax_t)stream->total_in,
	    (intmax_t)stream->total_out,
	    (intmax_t)stream->start_bit,
	    (intmax_t)stream->last_bit,
	    (intmax_t)stream->stop_bit);
}

/* -------------------------------------------------------------------------------------/
   Decompress one part
   Note that we are using libzlib (from varnish) and not zlib (from system)
*/
ssize_t uncompress_pipeline(VRT_CTX, struct vsb** pvsb, struct http_conn *htc)
{
	struct vsb	*body;
	struct vsb	*output;
	z_stream	stream;
	char		*buffer;
	int		err;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	memset(&stream, 0, sizeof(stream));
	if (inflateInit2(&stream, 31) != Z_OK) {
		VSLb(ctx->vsl, SLT_Error, "zlib: can't run inflateInit2");
		return (-1);
	}

	output = VSB_new_auto();
	DEBUG(syslog(LOG_INFO, "zlib: new_auto vsb %lu", (uintptr_t)output));
	DEBUG(syslog(LOG_INFO, "zlib: workspace free %ld", ctx->ws->e - ctx->ws->f));
	buffer = WS_Alloc(ctx->ws, cache_param->gzip_buffer);
	XXXAN(buffer);

	stream.next_in = (Bytef *)htc->pipeline_b;
	stream.avail_in = htc->pipeline_e - htc->pipeline_b;
	while (stream.avail_in > 0) {
		stream.next_out = (Bytef *)buffer;
		stream.avail_out = cache_param->gzip_buffer;

		DEBUG(VSLb(ctx->vsl, SLT_Debug, "zlib: inflateb in (%lu/%u) out (%lu/%u)",
			(uintptr_t)stream.next_in, stream.avail_in,
			(uintptr_t)stream.next_out, stream.avail_out));
		err = inflate(&stream, Z_SYNC_FLUSH);
		DEBUG(VSLb(ctx->vsl, SLT_Debug, "zlib: inflatef in (%lu/%u) out (%lu/%u)",
			(uintptr_t)stream.next_in, stream.avail_in,
			(uintptr_t)stream.next_out, stream.avail_out));
		if ((err != Z_OK && err != Z_STREAM_END) || (err == Z_STREAM_END && stream.avail_in != 0)) {
			VSLb(ctx->vsl, SLT_Error, "zlib: inflate read buffer (%d/%s)", err, stream.msg);
			log_stream(ctx, &stream);
			inflateEnd(&stream);
			DEBUG(syslog(LOG_INFO, "zlib: destroy vsb %lu", (uintptr_t)output));
			VSB_destroy(&output);
			WS_Reset(ctx->ws, buffer);
			return (-1);
		}
		AZ(VSB_bcat(output, buffer, (char*)stream.next_out - buffer));
	}
	log_stream(ctx, &stream);
	inflateEnd(&stream);
	VSB_finish(output);
	WS_Reset(ctx->ws, buffer);

	// We got a complete uncompressed buffer
	// We need to write it back into pipeline
	if (*pvsb) {
		CAST_OBJ_NOTNULL(body, *pvsb, VSB_MAGIC);
		DEBUG(syslog(LOG_INFO, "zlib: destroy vsb %lu", (uintptr_t)*pvsb));
		VSB_destroy(pvsb);
	}
	*pvsb = output;
	htc->pipeline_b = VSB_data(output);
	htc->pipeline_e = htc->pipeline_b + VSB_len(output);
	if (htc->pipeline_b == htc->pipeline_e) {
		htc->pipeline_b = NULL;
		htc->pipeline_e = NULL;
	}
	return (VSB_len(output));
}

ssize_t validate_request(VRT_CTX)
{
	const char *ptr;
	ssize_t cl;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->method != VCL_MET_RECV) {
		/* Can be called only in vcl_recv.
		** vcl_backend_fetch is a bad place since std.cache is in recv
		*/
		VSLb(ctx->vsl, SLT_Error, "zlib: must be called in vcl_recv");
		return (-1);
	}

	// Content-Encoding handling
	if (http_GetHdr(ctx->http_req, H_Content_Encoding, &ptr)) {
		if (strstr(ptr, "deflate") != 0) {
			VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Content-Encoding");
			return (-1);
		}
		else if (strstr(ptr, "gzip") == 0) {
			VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Content-Encoding");
			return (-1);
		}
	}
	else {
		VSLb(ctx->vsl, SLT_Gzip, "zlib: nothing to do");
		return (0);
	}

	// Transfer-Encoding handling
	if (http_GetHdr(ctx->http_req, H_Transfer_Encoding, &ptr)) {
		VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Transfer-Encoding");
		return (-1);
	}
	// Get Content-Length
	cl = http_GetContentLength(ctx->http_req);
	if (cl <= 0) {
		VSLb(ctx->vsl, SLT_Gzip, "zlib: no Content-Length");
		return (cl);
	}
	return (cl);
}

/*--------------------------------------------------------------------
 * Unzip of a request / response
 * Does not manage chunks.
 * Returns :
 * 0 if success or no Content-Length
 * -1 if failed:
 * - unknown Content-Encoding
 * - Transfer-Encoding present
 * - uncompress error
 */
VCL_INT __match_proto__(td_zlib_unzip_request)
	vmod_unzip_request(VRT_CTX, struct vmod_priv *priv)
{
	struct vsb	**pvsb;
	ssize_t		cl;
	ssize_t		ret;

	cl = validate_request(ctx);
	if (cl <= 0) {
		// can be 0 (no body) or -1 (wrong req)
		return (cl);
	}

	pvsb = VSB_get(ctx, priv);
	ret = fill_pipeline(ctx, pvsb, ctx->req->htc, cl);
	if (ret <= 0) {
		VSLb(ctx->vsl, SLT_Error, "zlib: read error (%ld)", ret);
		return (-1);
	}
	AN(ret == cl);
	cl = uncompress_pipeline(ctx, pvsb, ctx->req->htc);
	if (cl < 0) {
		VSLb(ctx->vsl, SLT_Error, "zlib: can't uncompress pipeline");
		return (-1);
	}
	else if (cl == 0) {
		http_Unset(ctx->req->http, H_Content_Length);
		ctx->req->req_body_status = REQ_BODY_NONE;
		ctx->req->htc->body_status = BS_EOF;
		ctx->req->htc->content_length = -1;
	}
	else {
		VRT_SetHdr(ctx, &VGC_HDR_REQ_Content_2d_Length, VRT_INT_string(ctx, cl),
		    vrt_magic_string_end);
		ctx->req->req_body_status = REQ_BODY_WITH_LEN;
		ctx->req->htc->body_status = BS_LENGTH;
		ctx->req->htc->content_length = cl;
	}
	http_Unset(ctx->req->http, H_Content_Encoding);

	DEBUG(VSLb(ctx->vsl, SLT_Debug, "zlib: completed with success"));
	return (0);
}
