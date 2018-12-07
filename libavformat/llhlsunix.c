/*
 * Unix socket protocol
 * Copyright (c) 2013 Luca Barbato
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 *
 * Unix socket url_protocol
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "os_support.h"
#include "network.h"
#include <sys/un.h>
#include "url.h"

typedef struct llhlsUnixContext {
    const AVClass *class;
    struct sockaddr_un addr;
    int timeout;
    int fd;
	char chunkUri[1024];
} llhlsUnixContext;

#define OFFSET(x) offsetof(llhlsUnixContext, x)
#define ED AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_ENCODING_PARAM
static const AVOption llhlsunix_options[] = {
    { "timeout",   "Timeout in ms",                         OFFSET(timeout), AV_OPT_TYPE_INT,   { .i64 = -1 },                  -1, INT_MAX, ED },
    { NULL }
};

static const AVClass llhlsunix_class = {
    .class_name = "llhlsunix",
    .item_name  = av_default_item_name,
    .option     = llhlsunix_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int llhlsunix_open(URLContext *h, const char *filename, int flags)
{
    llhlsUnixContext *s = h->priv_data;
    int fd = 0, ret = 0;

    av_strstart(filename, "llhls:", &filename);

	char filenamePre[1024] = {0};
	memset(s->chunkUri, 0, sizeof(s->chunkUri));
	char* delimiter = av_strnstr(filename,"?", strlen(filename));
	if(delimiter != NULL){
		memset(&s->addr, 0, sizeof(s->addr));
		av_strlcpy(filenamePre,filename+2,(delimiter-filename)-2-1);
		av_strlcpy(s->chunkUri,delimiter+1,strlen(filename)-strlen(delimiter)-1);
		filename = filenamePre;
	}

    s->addr.sun_family = AF_UNIX;
    strncpy(s->addr.sun_path, filename, 90);// 90 < 102/104 for sure
    if ((fd = ff_socket(AF_UNIX, SOCK_STREAM, 0)) < 0){
		av_log(s, AV_LOG_INFO, "- llhls: fail socket=%i\n",ret);
        return ff_neterrno();
	}

    if (s->timeout < 0 && h->rw_timeout){
        s->timeout = h->rw_timeout / 1000;
	}

    ret = ff_listen_connect(fd, (struct sockaddr *)&s->addr,
                            sizeof(s->addr), s->timeout, h, 0);
    if (ret < 0){
		av_log(s, AV_LOG_INFO, "- llhls: fail connect=%i\n",ret);
        goto fail;
	}
    s->fd = fd;
	if(s->chunkUri[0] != 0){
		// With final /0
		ret = send(s->fd, s->chunkUri, strlen(s->chunkUri)+1, MSG_NOSIGNAL);
		av_log(s, AV_LOG_INFO, "- llhls: requesting uri=%s, client_fd = %i, ret = %i, errno = %i\n", s->chunkUri, s->fd, ret, ff_neterrno());
	}
    return 0;

fail:
    if (fd >= 0)
        closesocket(fd);
    return ret;
}

static int llhlsunix_read(URLContext *h, uint8_t *buf, int size)
{
    llhlsUnixContext *s = h->priv_data;
    int ret;
    //if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
    //    ret = ff_network_wait_fd(s->fd, 0);
    //    if (ret < 0){
	//		av_log(s, AV_LOG_INFO, "- llhls: reading wait, ret=%i\n", ret);
    //        return ret;
	//	}
    //}
    ret = recv(s->fd, buf, size, 0);
	int ret_errno = ff_neterrno();
	//av_log(s, AV_LOG_INFO, "- llhls: reading done, ret=%i, errno=%i EAGAIN=%i\n", ret, ret_errno, AVERROR(EAGAIN));
	if(ret < 0 && ret_errno == AVERROR(EAGAIN)){
		//av_log(s, AV_LOG_INFO, "- llhls: reading EAGAIN, uri = %s\n", s->chunkUri);
		return AVERROR(EAGAIN);
	}
	if(ret == 0){
		av_log(s, AV_LOG_INFO, "- llhls: reading AVERROR_EOF, uri = %s\n", s->chunkUri);
		return AVERROR_EOF;
	}
    //return ret < 0 ? ff_neterrno() : ret;
	return ret;
}

static int llhlsunix_write(URLContext *h, const uint8_t *buf, int size)
{
    llhlsUnixContext *s = h->priv_data;
    int ret;
    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->fd, 1);
        if (ret < 0)
            return ret;
    }
    ret = send(s->fd, buf, size, MSG_NOSIGNAL);
    return ret < 0 ? ff_neterrno() : ret;
}

static int llhlsunix_close(URLContext *h)
{
    llhlsUnixContext *s = h->priv_data;
    closesocket(s->fd);
    return 0;
}

static int llhlsunix_get_file_handle(URLContext *h)
{
    llhlsUnixContext *s = h->priv_data;
    return s->fd;
}

const URLProtocol ff_llhlsunix_protocol = {
    .name                = "llhls",
    .url_open            = llhlsunix_open,
    .url_read            = llhlsunix_read,
    .url_write           = llhlsunix_write,
    .url_close           = llhlsunix_close,
    .url_get_file_handle = llhlsunix_get_file_handle,
    .priv_data_size      = sizeof(llhlsUnixContext),
    .priv_data_class     = &llhlsunix_class,
    .flags               = URL_PROTOCOL_FLAG_NETWORK + AVIO_FLAG_NONBLOCK,
};
