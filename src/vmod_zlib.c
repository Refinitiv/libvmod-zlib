/*
 * Copyright 2017 Thomson Reuters
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
#include "lib/libvgz/vgz.h"
#include "vcc_zlib_if.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

enum ce_type {
	CE_NONE = 0,
	CE_GZIP = 1,
	CE_DEFLATE =  2,
	CE_UNKNOWN = 3
};

#define ALLOC_BUFFER(ptr, l)				\
	do {						\
		(ptr) = calloc(sizeof(*(ptr)) * l, 1);	\
		XXXAN(ptr);				\
	} while (0)


#define REALLOC_BUFFER(ptr, size, newsize)				\
	do {								\
		void* newptr = realloc(ptr, newsize);			\
		XXXAN(newptr);						\
		if (newptr != (ptr)) {					\
			memset(newptr + size, 0, newsize - size);	\
			(ptr) = newptr;					\
		}							\
		size = newsize;						\
	} while (0)

#define FREE_BUFFER(x)					\
	do {						\
		if (x)					\
			free(x);			\
		(x) = NULL;				\
	} while (0)

#define HTC_FD(htc) (*((htc)->rfd))

int
read_and_uncompress(VRT_CTX, z_stream *compression_stream, struct http_conn *htc, ssize_t cl)
{
	// Read body and uncompress
	ssize_t gzbuf_size;
	char *gzbuf;
	char *out_buf = NULL;
	ssize_t out_len = 0;
	ssize_t out_size = cl * 4;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	gzbuf_size = (cl > cache_param->gzip_buffer) ? cache_param->gzip_buffer : cl;
	gzbuf = WS_Alloc(ctx->ws, gzbuf_size);
	AN(gzbuf);
	ALLOC_BUFFER(out_buf, out_size);

	while (cl > 0) {
		// Read initial buffer
		ssize_t gzbytes_to_read = (cl > gzbuf_size) ? gzbuf_size : cl;
		ssize_t gzbytes_read = read(HTC_FD(htc), gzbuf, gzbytes_to_read);
		if (gzbytes_read <= 0) {
			VSLb(ctx->vsl, SLT_Error, "unzip_request: can't read varnish socket (%d)", errno);
			FREE_BUFFER(out_buf);
			return (-1);
		}
		cl -= gzbytes_read;

		compression_stream->next_in = (Bytef *)gzbuf;
		compression_stream->avail_in = gzbytes_read;
		while (compression_stream->avail_in > 0) {
			if (out_size - out_len == 0) {
				REALLOC_BUFFER(out_buf, out_size, out_size * 2);
			}

			compression_stream->next_out = (Bytef *)(out_buf + out_len);
			compression_stream->avail_out = out_size - out_len;

			Bytef *before = compression_stream->next_out;
			int err = inflate(compression_stream, 0);
			if (err != Z_OK && err != Z_STREAM_END) {
				VSLb(ctx->vsl, SLT_Error, "unzip_request: inflate read buffer (%s)", compression_stream->msg);
				FREE_BUFFER(out_buf);
				return (-1);
			}
			unsigned len = compression_stream->next_out - before;
			out_len += len;
			VSLb(ctx->vsl, SLT_Debug, "unzip_request: inflate (%d bytes)", len);
		}
	}

	// Write uncompressed buffer to Varnish pipeline
	if (htc->pipeline_b != NULL) {
		unsigned pipesize;
		pipesize = htc->pipeline_e - htc->pipeline_b;
		if (out_size - out_len < pipesize) {
			REALLOC_BUFFER(out_buf, out_size, out_size + pipesize);
		}
		memcpy(out_buf + out_len, htc->pipeline_b, pipesize);
		out_len += pipesize;
	}
	htc->pipeline_b = out_buf;
	htc->pipeline_e = out_buf + out_len;

	// req headers will be used for sending body to backend
	VRT_SetHdr(ctx, HDR_REQ, H_Content_Encoding, vrt_magic_string_unset);
	VRT_SetHdr(ctx, HDR_REQ, H_Content_Length, VRT_INT_string(ctx, out_len), vrt_magic_string_end);
	return (0);
}

int http_content_encoding(struct http *http)
{
	const char *ptr;
	if (http_GetHdr(http, H_Content_Encoding, &ptr))
	{
		if (strstr(ptr, "gzip") != 0)
		{
			return CE_GZIP;
		}
		else if (strstr(ptr, "deflate") != 0)
		{
			return CE_DEFLATE;
		}
		else
		{
			return CE_UNKNOWN;
		}
	}
	return CE_NONE;
}

/*--------------------------------------------------------------------
 * Unzip of a request / response
 * Does not manage chunks.
 * Returns :
 * 0 if success
 * -1 if failed:
 * - unknown Content-Encoding
 * - no Content-Length found
 * - Transfer-Encoding present
 * - uncompress error
 */
VCL_INT __match_proto__(td_zlib_unzip_request)
vmod_unzip_request(VRT_CTX)
{
	const char* ptr;
	ssize_t cl;

	if (ctx->method != VCL_MET_RECV) {
		/* Can be called only in vcl_recv.
		** vcl_backend_fetch is a bad place since std.cache is in recv
		*/
		VSLb(ctx->vsl, SLT_Error, "unzip_request must be called in vcl_recv");
		return (-1);
	}

	// Content-Encoding handling
	enum ce_type ce;
	ce = http_content_encoding(ctx->http_req);
	if (ce == CE_NONE) {
		VSLb(ctx->vsl, SLT_Debug, "unzip_request: nothing to do");
		return (0);
	}
	else if (ce != CE_GZIP) {
		VSLb(ctx->vsl, SLT_Error, "unzip_request unsupported Content-Encoding");
		return (-1);
	}

	// Transfer-Encoding handling
	if (http_GetHdr(ctx->http_req, H_Transfer_Encoding, &ptr)) {
		VSLb(ctx->vsl, SLT_Error, "unzip_request unsupported Transfer-Encoding");
		return (-1);
	}
	// Get Content-Length
	cl = http_GetContentLength(ctx->http_req);
	if (cl <= 0) {
		VSLb(ctx->vsl, SLT_Error, "unzip_request: no Content-Length");
		return (-1);
	}

	//Initialize zlib stream
	z_stream *compression_stream;
	compression_stream = WS_Alloc(ctx->ws, sizeof(*compression_stream));

	// we are using libzlib (from varnish) and not zlib (from system)
	int sts = inflateInit2(compression_stream, 31);
	if (sts != Z_OK) {
		VSLb(ctx->vsl, SLT_Error, "unzip_request: can't run inflateInit2");
		WS_Release(ctx->ws, 0);
		return (-1);
	}

	int ret = read_and_uncompress(ctx, compression_stream, ctx->req->htc, cl);

	// clear zlib
	inflateEnd(compression_stream);
	if (ret >= 0){
		VSLb(ctx->vsl, SLT_Debug, "unzip_request: completed with success");
	}
	return ret;
}

