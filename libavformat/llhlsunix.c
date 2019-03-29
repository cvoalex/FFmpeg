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
#include "libavutil/time.h"
#include "os_support.h"
#include "network.h"
#include <sys/un.h>
#include "url.h"

#define kLLHLS_UNIX_MAGIC_ERROR "<<<=== MAGIC_ERROR_STRING {SHOULDNT BE IN TS/MP4} ===>>>"

typedef struct llhlsUnixContext {
    const AVClass *class;
    struct sockaddr_un addr;
    int fd;
	char chunkUri[1024];
	char chunkLastbytes[1024];
	int data_read;
} llhlsUnixContext;

#define ED AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_ENCODING_PARAM
static const AVOption llhlsunix_options[] = {
    { NULL }
};

static const AVClass llhlsunix_class = {
    .class_name = "llhlsunix",
    .item_name  = av_default_item_name,
    .option     = llhlsunix_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static char *findBufStr(char *str, const char *substr, size_t n)
{
    char *p = str, *pEnd = str+n;
    size_t substr_len = strlen(substr);

    if(0 == substr_len)
        return str; // the empty string is contained everywhere.

    pEnd -= (substr_len - 1);
    for(;p < pEnd; ++p)
    {
        if(0 == strncmp(p, substr, substr_len))
            return p;
    }
    return NULL;
}

static int llhlsunix_open(URLContext *h, const char *filename, int flags)
{
    llhlsUnixContext *s = h->priv_data;
	memset(s->chunkLastbytes, 0, sizeof(s->chunkLastbytes));
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
		av_log(s, AV_LOG_INFO, "- llhls: ERROR. fail socket=%i\n",ret);
        return ff_neterrno();
	}

    int timeout = 100;
	s->data_read = 0;
    ret = ff_listen_connect(fd, (struct sockaddr *)&s->addr,
                            sizeof(s->addr), timeout, h, 0);
	if (ret == -61){
		// During player reloads shutdowns may need reader to wait
		av_log(s, AV_LOG_INFO, "- llhls: ERROR. fail connect=%i, trying again...\n",ret);
		av_usleep(300);
		ret = ff_listen_connect(fd, (struct sockaddr *)&s->addr,
								sizeof(s->addr), timeout, h, 0);
	}
    if (ret < 0){
		av_log(s, AV_LOG_INFO, "- llhls: ERROR. fail connect=%i\n",ret);
        goto fail;
	}
    s->fd = fd;
	if(s->chunkUri[0] != 0){
		// With final /0
		ret = send(s->fd, s->chunkUri, strlen(s->chunkUri)+1, MSG_NOSIGNAL);
		if(ret <= 0){
			av_log(s, AV_LOG_INFO, "- llhls: ERROR. fail send=%i, trying again...\n",ret);
			av_usleep(300);
			ret = send(s->fd, s->chunkUri, strlen(s->chunkUri)+1, MSG_NOSIGNAL);
		}
		av_log(s, AV_LOG_INFO, "- llhls: OK. requesting uri=%s, fd = %i, ret = %i, errno = %i\n", s->chunkUri, s->fd, ret, ff_neterrno());
	}
    return 0;

fail:
    if (fd >= 0){
        closesocket(fd);
	}
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
	if(ret < 0){
		if(ret_errno == AVERROR(EAGAIN)){
			//av_log(s, AV_LOG_INFO, "- llhls: reading EAGAIN, uri = %s\n", s->chunkUri);
			return AVERROR(EAGAIN);
		}
		return ret_errno;
	}
	if(ret > 0){
		int lastbytes = FFMIN(ret, sizeof(s->chunkLastbytes));
		int lastoffset = 0;
		if(ret > sizeof(s->chunkLastbytes)){
			lastoffset = ret-sizeof(s->chunkLastbytes);
		}
		memcpy(s->chunkLastbytes, buf+lastoffset, lastbytes);
	}
	// Checking last datas for magic error code value - kLLHLS_UNIX_MAGIC_ERROR
	const char *magicErr = kLLHLS_UNIX_MAGIC_ERROR;
	if(findBufStr(s->chunkLastbytes, magicErr, sizeof(s->chunkLastbytes)) != NULL){
		av_log(s, AV_LOG_INFO, "- llhls: error for uri = %s, data_read = %i\n", s->chunkUri, s->data_read);
		return AVERROR_INVALIDDATA;
	}
	if(ret == 0){
		av_log(s, AV_LOG_INFO, "- llhls: done for uri = %s, data_read = %i\n", s->chunkUri, s->data_read);
		return AVERROR_EOF;
	}
	s->data_read += ret;
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
	//av_log(s, AV_LOG_INFO, "- llhls: closing socket for uri = %s, data_read = %i\n", s->chunkUri, s->data_read);
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
