/*
Copyright (c) 2009-2013
	Lars-Dominik Braun <lars@6xq.net>
Copyright (c) 2011-2012
	Micha� Cicho� <michcic@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 1 /* required by getaddrinfo() */
#define _BSD_SOURCE /* snprintf() */
#define _DARWIN_C_SOURCE /* snprintf() on OS X */
#endif

#ifdef _MSC_VER
#define waitress_strdup						_strdup
#define waitress_snprintf					_snprintf
#define waitress_strcasecmp					_stricmp
#else
#define waitress_strdup						strdup
#define waitress_snprintf					snprintf
#define waitress_strcasecmp					strcasecmp
#endif

#ifdef _WIN32
#define waitress_size_t_spec				"%Iu"
#define waitress_nfds_t						ULONG
#define waitress_read(handle, buf, len)		recv(handle, buf, len, 0)
#define waitress_write(handle, buf, len)	send(handle, buf, len, 0)
#define waitress_close(handle)				closesocket(handle)
#define waitress_setsockopt(handle, level, optname, optval, optlen) \
	setsockopt(handle, level, optname, (char*)(optval), optlen)
#define waitress_getsockopt(handle, level, optname, optval, optlen) \
	getsockopt(handle, level, optname, (char*)(optval), optlen)
#else
#define waitress_size_t_spec				"%zu"
#define waitress_nfds_t						nfds_t
#define waitress_read(handle, buf, len)		read(handle, buf, len)
#define waitress_write(handle, buf, len)	write(handle, buf, len)
#define waitress_close(handle)				close(handle)
#define waitress_setsockopt(handle, level, optname, optval, optlen) \
	setsockopt(handle, level, optname, optval, optlen)
#define waitress_getsockopt(handle, level, optname, optval, optlen) \
	getsockopt(handle, level, optname, optval, optlen)
#endif

#include "config.h"
#include "waitress.h"

#if WAITRESS_USE_POLARSSL
#include <polarssl/ssl.h>
#include <polarssl/entropy.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/x509.h>
#include <polarssl/sha1.h>

struct _polarssl_ctx
{
	ssl_context			ssl;
	ssl_session			session;
	entropy_context		entrophy;
	ctr_drbg_context	rnd;
};

#endif

#include <sys/types.h>
#ifdef _WIN32
#define _WIN32_WINNT 0x501
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#endif
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>

#if WAITRESS_USE_GNUTLS
#include <gnutls/x509.h>
#endif

#define strcaseeq(a,b) (waitress_strcasecmp(a,b) == 0)
#define WAITRESS_HTTP_VERSION "1.1"

typedef struct {
	char *data;
	size_t pos;
} WaitressFetchBufCbBuffer_t;

static WaitressReturn_t WaitressReceiveHeaders (WaitressHandle_t *, size_t *);

#define READ_RET(buf, count, size) \
		if ((wRet = waith->request.read (waith, buf, count, size)) != \
				WAITRESS_RET_OK) { \
			return wRet; \
		}

#define WRITE_RET(buf, count) \
		if ((wRet = waith->request.write (waith, buf, count)) != WAITRESS_RET_OK) { \
			return wRet; \
		}

#ifdef _WIN32
static void WaitressStaticFree (void) {

#if WAITRESS_USE_GNUTLS
	gnutls_global_deinit ();
#endif

	WSACleanup ();
}

static void WaitressStaticInit (void) {

	/* In order to avoid static member constructor attribute can be used
	 * on GCC. Under MSVC very same functionality require rather nasty
	 * trick with pragmas and CRT initialization pages.
	 *
	 * I picked solution simple to understand and implement.
	 */
	static bool isInitialized = false;

	if (false == isInitialized) {

		WSADATA wsaData;
		WSAStartup (MAKEWORD(2, 2), &wsaData);

#if WAITRESS_USE_GNUTLS
		gnutls_global_init ();

		gcry_check_version (NULL);
		gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
		gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif

		atexit (WaitressStaticFree);

		isInitialized = true;
	}
}
#endif

void WaitressInit (WaitressHandle_t *waith) {
	assert (waith != NULL);

	#ifdef _WIN32
	WaitressStaticInit ();
	#endif

	memset (waith, 0, sizeof (*waith));
	waith->timeout = 30000;
}

void WaitressFree (WaitressHandle_t *waith) {
	assert (waith != NULL);

	free (waith->url.url);
	free (waith->proxy.url);
	memset (waith, 0, sizeof (*waith));
}

/*	Proxy set up?
 *	@param Waitress handle
 *	@return true|false
 */
static bool WaitressProxyEnabled (const WaitressHandle_t *waith) {
	assert (waith != NULL);

	return waith->proxy.host != NULL;
}

/*	urlencode post-data
 *	@param encode this
 *	@return malloc'ed encoded string, don't forget to free it
 */
char *WaitressUrlEncode (const char *in) {
	size_t inLen;
	char *out;
	const char *inPos;
	char *outPos;

	assert (in != NULL);

	inLen = strlen (in);
	/* worst case: encode all characters */
	out = calloc (inLen * 3 + 1, sizeof (*in));
	inPos = in;
	outPos = out;

	while (inPos - in < (int)inLen) {
		if (!isalnum (*inPos) && *inPos != '_' && *inPos != '-' && *inPos != '.') {
			*outPos++ = '%';
			waitress_snprintf (outPos, 3, "%02x", *inPos & 0xff);
			outPos += 2;
		} else {
			/* copy character */
			*outPos++ = *inPos;
		}
		++inPos;
	}

	return out;
}

/*	base64 encode data
 *	@param encode this
 *	@return malloc'ed string
 */
static char *WaitressBase64Encode (const char *in) {
	char *out, *outPos;
	const char *inPos;
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz0123456789+/";
	const size_t alphabetLen = strlen (alphabet);
	size_t inLen;

	assert (in != NULL);

	inLen = strlen (in);

	/* worst case is 1.333 */
	out = malloc ((inLen * 2 + 1) * sizeof (*out));
	if (out == NULL) {
		return NULL;
	}
	outPos = out;
	inPos = in;

	while (inLen >= 3) {
		uint8_t idx;

		idx = ((*inPos) >> 2) & 0x3f;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = ((*inPos) & 0x3) << 4;
		++inPos;
		idx |= ((*inPos) >> 4) & 0xf;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = ((*inPos) & 0xf) << 2;
		++inPos;
		idx |= ((*inPos) >> 6) & 0x3;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = (*inPos) & 0x3f;
		++inPos;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		inLen -= 3;
	}

	switch (inLen) {
		case 2: {
			uint8_t idx;

			idx = ((*inPos) >> 2) & 0x3f;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0x3) << 4;
			++inPos;
			idx |= ((*inPos) >> 4) & 0xf;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0xf) << 2;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			*outPos = '=';
			++outPos;
			break;
		}

		case 1: {
			uint8_t idx;

			idx = ((*inPos) >> 2) & 0x3f;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0x3) << 4;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			*outPos = '=';
			++outPos;

			*outPos = '=';
			++outPos;
			break;
		}
	}
	*outPos = '\0';

	return out;
}

/*	Split http url into host, port and path
 *	@param url
 *	@param returned url struct
 *	@return url is a http url? does not say anything about its validity!
 */
static bool WaitressSplitUrl (const char *inurl, WaitressUrl_t *retUrl) {
	static const char *httpPrefix = "http://";

	assert (inurl != NULL);
	assert (retUrl != NULL);

	/* is http url? */
	if (strncmp (httpPrefix, inurl, strlen (httpPrefix)) == 0) {
		enum {FIND_USER, FIND_PASS, FIND_HOST, FIND_PORT, FIND_PATH, DONE}
				state = FIND_USER, newState = FIND_USER;
		char *url, *urlPos, *assignStart;
		const char **assign = NULL;

		url = waitress_strdup (inurl);
		free (retUrl->url);
		retUrl->url = url;

		urlPos = url + strlen (httpPrefix);
		assignStart = urlPos;

		if (*urlPos == '\0') {
			state = DONE;
		}

		while (state != DONE) {
			const char c = *urlPos;

			switch (state) {
				case FIND_USER: {
					if (c == ':') {
						assign = &retUrl->user;
						newState = FIND_PASS;
					} else if (c == '@') {
						assign = &retUrl->user;
						newState = FIND_HOST;
					} else if (c == '/') {
						/* not a user */
						assign = &retUrl->host;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->host;
						newState = DONE;
					}
					break;
				}

				case FIND_PASS: {
					if (c == '@') {
						assign = &retUrl->password;
						newState = FIND_HOST;
					} else if (c == '/') {
						/* not a password */
						assign = &retUrl->port;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->port;
						newState = DONE;
					}
					break;
				}

				case FIND_HOST: {
					if (c == ':') {
						assign = &retUrl->host;
						newState = FIND_PORT;
					} else if (c == '/') {
						assign = &retUrl->host;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->host;
						newState = DONE;
					}
					break;
				}

				case FIND_PORT: {
					if (c == '/') {
						assign = &retUrl->port;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->port;
						newState = DONE;
					}
					break;
				}

				case FIND_PATH: {
					if (c == '\0') {
						assign = &retUrl->path;
						newState = DONE;
					}
					break;
				}

				case DONE:
					break;
			} /* end switch */

			if (assign != NULL) {
				*assign = assignStart;
				*urlPos = '\0';
				assignStart = urlPos+1;

				state = newState;
				assign = NULL;
			}

			++urlPos;
		} /* end while */

		/* fixes for our state machine logic */
		if (retUrl->user != NULL && retUrl->host == NULL && retUrl->port != NULL) {
			retUrl->host = retUrl->user;
			retUrl->user = NULL;
		}
		return true;
	} /* end if strncmp */

	return false;
}

/*	Parse url and set host, port, path
 *	@param Waitress handle
 *	@param url: protocol://host:port/path
 */
bool WaitressSetUrl (WaitressHandle_t *waith, const char *url) {
	return WaitressSplitUrl (url, &waith->url);
}

/*	Set http proxy
 *	@param waitress handle
 *  @param url, e.g. http://proxy:80/
 */
bool WaitressSetProxy (WaitressHandle_t *waith, const char *url) {
	return WaitressSplitUrl (url, &waith->proxy);
}

/*	Callback for WaitressFetchBuf, appends received data to \0-terminated
 *	buffer
 *	@param received data
 *	@param data size
 *	@param buffer structure
 */
static WaitressCbReturn_t WaitressFetchBufCb (void *recvData, size_t recvDataSize,
		void *extraData) {
	char *recvBytes = recvData;
	WaitressFetchBufCbBuffer_t *buffer = extraData;

	if (buffer->data == NULL) {
		if ((buffer->data = malloc (sizeof (*buffer->data) *
				(recvDataSize + 1))) == NULL) {
			return WAITRESS_CB_RET_ERR;
		}
	} else {
		char *newbuf;
		if ((newbuf = realloc (buffer->data,
				sizeof (*buffer->data) *
				(buffer->pos + recvDataSize + 1))) == NULL) {
			free (buffer->data);
			return WAITRESS_CB_RET_ERR;
		}
		buffer->data = newbuf;
	}
	memcpy (buffer->data + buffer->pos, recvBytes, recvDataSize);
	buffer->pos += recvDataSize;
	buffer->data[buffer->pos] = '\0';

	return WAITRESS_CB_RET_OK;
}

/*	Fetch string. Beware! This overwrites your waith->data pointer
 *	@param waitress handle
 *	@param \0-terminated result buffer, malloced (don't forget to free it
 *			yourself)
 */
WaitressReturn_t WaitressFetchBuf (WaitressHandle_t *waith, char **retBuffer) {
	WaitressFetchBufCbBuffer_t buffer;
	WaitressReturn_t wRet;

	assert (waith != NULL);
	assert (retBuffer != NULL);

	memset (&buffer, 0, sizeof (buffer));

	waith->data = &buffer;
	waith->callback = WaitressFetchBufCb;

	wRet = WaitressFetchCall (waith);
	*retBuffer = buffer.data;
	return wRet;
}

/*	write () wrapper with poll () timeout
 *	@param waitress handle
 *	@param write buffer
 *	@param write count bytes
 *	@return number of written bytes or -1 on error
 */
static ssize_t WaitressPollWrite (void *data, const void *buf, size_t count) {
	int pollres = -1;
	ssize_t retSize;
	WaitressHandle_t *waith = data;
	fd_set fds;
	struct timeval tv;

	assert (waith != NULL);
	assert (buf != NULL);

	/* FIXME: simplify logic */
	memset (&tv, 0, sizeof (tv));
	tv.tv_sec = waith->timeout / 1000;
	tv.tv_usec = (waith->timeout % 1000) * 1000;

	FD_ZERO (&fds);
	FD_SET (waith->request.sockfd, &fds);

	pollres = select (waith->request.sockfd, NULL, &fds, &fds, &tv);
	if (pollres == 0) {
		waith->request.readWriteRet = WAITRESS_RET_TIMEOUT;
		return -1;
	} else if (pollres == -1) {
		waith->request.readWriteRet = WAITRESS_RET_ERR;
		return -1;
	}
	if ((retSize = waitress_write (waith->request.sockfd, buf, count)) == -1) {
		waith->request.readWriteRet = WAITRESS_RET_ERR;
		return -1;
	}
	waith->request.readWriteRet = WAITRESS_RET_OK;
	return retSize;
}

static WaitressReturn_t WaitressOrdinaryWrite (void *data, const char *buf,
		const size_t size) {
	WaitressHandle_t *waith = data;

	WaitressPollWrite (waith, buf, size);
	return waith->request.readWriteRet;
}

static WaitressReturn_t WaitressTlsWrite (void *data, const char *buf,
		const size_t size) {
#if WAITRESS_USE_GNUTLS
	WaitressHandle_t *waith = data;

	if (gnutls_record_send (waith->request.tlsSession, buf, size) < 0) {
		return WAITRESS_RET_TLS_WRITE_ERR;
	}
	return waith->request.readWriteRet;
#endif

#if WAITRESS_USE_POLARSSL
	WaitressHandle_t *waith = data;

	if (ssl_write (&waith->request.sslCtx->ssl, buf, size) < 0) {
		return WAITRESS_RET_TLS_WRITE_ERR;
	}

	return waith->request.readWriteRet;
#endif
}

/*	read () wrapper with poll () timeout
 *	@param waitress handle
 *	@param write to this buf, not NULL terminated
 *	@param buffer size
 *	@return number of read bytes or -1 on error
 */
static ssize_t WaitressPollRead (void *data, void *buf, size_t count) {
	int pollres = -1;
	ssize_t retSize;
	WaitressHandle_t *waith = data;
	fd_set fds;
	struct timeval tv;

	assert (waith != NULL);
	assert (buf != NULL);

	memset (&tv, 0, sizeof (tv));
	tv.tv_sec = waith->timeout / 1000;
	tv.tv_usec = (waith->timeout % 1000) * 1000;

	/* FIXME: simplify logic */
	FD_ZERO (&fds);
	FD_SET (waith->request.sockfd, &fds);

	pollres = select (waith->request.sockfd, &fds, NULL, &fds, &tv);
	if (pollres == 0) {
		waith->request.readWriteRet = WAITRESS_RET_TIMEOUT;
		return -1;
	} else if (pollres == -1) {
		waith->request.readWriteRet = WAITRESS_RET_ERR;
		return -1;
	}
	if ((retSize = waitress_read (waith->request.sockfd, buf, count)) == -1) {
		waith->request.readWriteRet = WAITRESS_RET_READ_ERR;
		return -1;
	}
	waith->request.readWriteRet = WAITRESS_RET_OK;
	return retSize;
}

static WaitressReturn_t WaitressOrdinaryRead (void *data, char *buf,
		const size_t size, size_t *retSize) {
	WaitressHandle_t *waith = data;

	const ssize_t ret = WaitressPollRead (waith, buf, size);
	if (ret != -1) {
		assert (ret >= 0);
		*retSize = (size_t) ret;
	}
	return waith->request.readWriteRet;
}

static WaitressReturn_t WaitressTlsRead (void *data, char *buf,
		const size_t size, size_t *retSize) {
#if WAITRESS_USE_GNUTLS
	WaitressHandle_t *waith = data;

	ssize_t ret = gnutls_record_recv (waith->request.tlsSession, buf, size);
	if (ret < 0) {
		return WAITRESS_RET_TLS_READ_ERR;
	} else {
		*retSize = ret;
	}
	return waith->request.readWriteRet;
#endif

#if WAITRESS_USE_POLARSSL
	WaitressHandle_t *waith = data;
	int ret;

	*retSize = 0;

	do
	{
		ret = ssl_read (&waith->request.sslCtx->ssl, buf, size);

		if (ret == POLARSSL_ERR_SSL_PEER_CLOSE_NOTIFY)
			return (waith->request.readWriteRet = WAITRESS_RET_OK);

		if (ret < 0)
			return (waith->request.readWriteRet = WAITRESS_RET_TLS_READ_ERR);

		if (ret >= 0)
			break;

	} while (1);

	*retSize = ret;

	return (waith->request.readWriteRet = WAITRESS_RET_OK);
#endif
}

/*	send basic http authorization
 *	@param waitress handle
 *	@param url containing user/password
 *	@param header name prefix
 */
static bool WaitressFormatAuthorization (WaitressHandle_t *waith,
		WaitressUrl_t *url, const char *prefix, char *writeBuf,
		const size_t writeBufSize) {
	assert (waith != NULL);
	assert (url != NULL);
	assert (prefix != NULL);
	assert (writeBuf != NULL);
	assert (writeBufSize > 0);

	if (url->user != NULL) {
		char userPass[1024], *encodedUserPass;
		waitress_snprintf (userPass, sizeof (userPass), "%s:%s", url->user,
				(url->password != NULL) ? url->password : "");
		encodedUserPass = WaitressBase64Encode (userPass);

		assert (encodedUserPass != NULL);
		waitress_snprintf (writeBuf, writeBufSize, "%sAuthorization: Basic %s\r\n",
				prefix, encodedUserPass);
		free (encodedUserPass);
		return true;
	}
	return false;
}

/*	get default http port if none was given
 */
static const char *WaitressDefaultPort (const WaitressUrl_t * const url) {
	assert (url != NULL);

	if (url->tls) {
		return url->tlsPort == NULL ? "443" : url->tlsPort;
	} else {
		return url->port == NULL ? "80" : url->port;
	}
}

/*	get line from string
 *	@param string beginning/return value of last call
 *	@return start of _next_ line or NULL if there is no next line
 */
static char *WaitressGetline (char * const str) {
	char *eol;

	assert (str != NULL);

	eol = strchr (str, '\n');
	if (eol == NULL) {
		return NULL;
	}

	/* make lines parseable by string routines */
	*eol = '\0';
	if (eol-1 >= str && *(eol-1) == '\r') {
		*(eol-1) = '\0';
	}
	/* skip \0 */
	++eol;

	assert (eol >= str);

	return eol;
}

/*	identity encoding handler
 */
static WaitressHandlerReturn_t WaitressHandleIdentity (void *data, char *buf,
		const size_t size) {
	WaitressHandle_t *waith = data;

	assert (data != NULL);
	assert (buf != NULL);

	waith->request.contentReceived += size;
	if (waith->callback (buf, size, waith->data) == WAITRESS_CB_RET_ERR) {
		return WAITRESS_HANDLER_ABORTED;
	} else {
		return WAITRESS_HANDLER_CONTINUE;
	}
}

/*	chunked encoding handler
 */
static WaitressHandlerReturn_t WaitressHandleChunked (void *data, char *buf,
		const size_t size) {
	WaitressHandle_t * const waith = data;
	size_t pos = 0;

	while (pos < size) {
		switch (waith->request.chunkedState) {
			case CHUNKSIZE:
				/* Poor man’s hex to integer. This avoids another buffer that
				 * fills until the terminating \r\n is received. */
				if (buf[pos] >= '0' && buf[pos] <= '9') {
					waith->request.chunkSize <<= 4;
					waith->request.chunkSize |= buf[pos] & 0xf;
				} else if (buf[pos] >= 'a' && buf[pos] <= 'f') {
					waith->request.chunkSize <<= 4;
					waith->request.chunkSize |= (buf[pos]+9) & 0xf;
				} else if (buf[pos] == '\r') {
					/* ignore */
				} else if (buf[pos] == '\n') {
					waith->request.chunkedState = DATA;
					/* last chunk has size 0 */
					if (waith->request.chunkSize == 0) {
						return WAITRESS_HANDLER_DONE;
					}
				} else {
					/* everything else is a protocol violation */
					return WAITRESS_HANDLER_ERR;
				}
				++pos;
				break;

			case DATA:
				if (waith->request.chunkSize > 0) {
					size_t payloadSize = size - pos;
					assert (size >= pos);

					if (payloadSize > waith->request.chunkSize) {
						payloadSize = waith->request.chunkSize;
					}
					if (WaitressHandleIdentity (waith, &buf[pos],
							payloadSize) == WAITRESS_HANDLER_ABORTED) {
						return WAITRESS_HANDLER_ABORTED;
					}
					pos += payloadSize;
					assert (waith->request.chunkSize >= payloadSize);
					waith->request.chunkSize -= payloadSize;
				} else {
					/* next chunk size starts in the next line */
					if (buf[pos] == '\n') {
						waith->request.chunkedState = CHUNKSIZE;
					}
					++pos;
				}
				break;
		}
	}

	return WAITRESS_HANDLER_CONTINUE;
}

/*	handle http header
 */
static void WaitressHandleHeader (WaitressHandle_t *waith, const char * const key,
		const char * const value) {
	assert (waith != NULL);
	assert (key != NULL);
	assert (value != NULL);

	if (strcaseeq (key, "Content-Length")) {
		waith->request.contentLength = atol (value);
		waith->request.contentLengthKnown = true;
	} else if (strcaseeq (key, "Transfer-Encoding")) {
		if (strcaseeq (value, "chunked")) {
			waith->request.dataHandler = WaitressHandleChunked;
		}
	}
}

/*	parse http status line and return status code
 */
static int WaitressParseStatusline (const char * const line) {
	char status[4] = "000";

	assert (line != NULL);

	if (sscanf (line, "HTTP/1.%*1[0-9] %3[0-9] ", status) == 1) {
		return atoi (status);
	}
	return -1;
}

/*	verify server certificate
 */
static WaitressReturn_t WaitressTlsVerify (const WaitressHandle_t *waith) {
#if WAITRESS_USE_GNUTLS
	gnutls_session_t session = waith->request.tlsSession;
	unsigned int certListSize;
	const gnutls_datum_t *certList;
	gnutls_x509_crt_t cert;
	char fingerprint[20];
	size_t fingerprintSize;

	if (gnutls_certificate_type_get (session) != GNUTLS_CRT_X509) {
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;
	}

	if ((certList = gnutls_certificate_get_peers (session,
			&certListSize)) == NULL) {
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;
	}

	if (gnutls_x509_crt_init (&cert) != GNUTLS_E_SUCCESS) {
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;
	}

	if (gnutls_x509_crt_import (cert, &certList[0],
			GNUTLS_X509_FMT_DER) != GNUTLS_E_SUCCESS) {
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;
	}

	fingerprintSize = sizeof (fingerprint);
	if (gnutls_x509_crt_get_fingerprint (cert, GNUTLS_DIG_SHA1, fingerprint,
			&fingerprintSize) != 0) {
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;
	}

	assert (waith->tlsFingerprint != NULL);
	if (memcmp (fingerprint, waith->tlsFingerprint, sizeof (fingerprint)) != 0) {
		return WAITRESS_RET_TLS_FINGERPRINT_MISMATCH;
	}

	gnutls_x509_crt_deinit (cert);

	return WAITRESS_RET_OK;
#endif

#if WAITRESS_USE_POLARSSL
	unsigned char fingerprint[20];

	x509_cert* cert = waith->request.sslCtx->ssl.peer_cert;

	if (NULL == cert)
		return WAITRESS_RET_TLS_HANDSHAKE_ERR;

	sha1(cert->raw.p, cert->raw.len, fingerprint);

	if (memcmp (fingerprint, waith->tlsFingerprint, sizeof (fingerprint)) != 0)
		return WAITRESS_RET_TLS_FINGERPRINT_MISMATCH;

	return WAITRESS_RET_OK;
#endif

	return 0;
}

static void WaitressDisableBlocking(int sockfd)
{
	#ifdef _WIN32
	u_long iMode = 1;
	ioctlsocket(sockfd, FIONBIO, &iMode);
	#else
	fcntl (sockfd, F_SETFL, O_NONBLOCK);
	#endif
	}

/*	Connect to server
 */
static WaitressReturn_t WaitressConnect (WaitressHandle_t *waith) {
	struct addrinfo hints, *res;
	const int sockopt = 256*1024;
	int pollres;
	socklen_t pollresSize = sizeof (pollres);
	fd_set fds;
	struct timeval tv;

	memset (&hints, 0, sizeof hints);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Use proxy? */
	if (WaitressProxyEnabled (waith)) {
		if (getaddrinfo (waith->proxy.host,
				WaitressDefaultPort (&waith->proxy), &hints, &res) != 0) {
			return WAITRESS_RET_GETADDR_ERR;
		}
	} else {
		if (getaddrinfo (waith->url.host,
				WaitressDefaultPort (&waith->url), &hints, &res) != 0) {
			return WAITRESS_RET_GETADDR_ERR;
		}
	}

	if ((waith->request.sockfd = socket (res->ai_family, res->ai_socktype,
			res->ai_protocol)) == -1) {
		freeaddrinfo (res);
		return WAITRESS_RET_SOCK_ERR;
	}

	/* we need shorter timeouts for connect() */
	WaitressDisableBlocking(waith->request.sockfd);

	/* increase socket receive buffer */
	waitress_setsockopt (waith->request.sockfd, SOL_SOCKET, SO_RCVBUF, &sockopt,
			sizeof (sockopt));

	/* non-blocking connect will return immediately */
	connect (waith->request.sockfd, res->ai_addr, res->ai_addrlen);

	memset (&tv, 0, sizeof (tv));
	tv.tv_sec = waith->timeout / 1000;
	tv.tv_usec = (waith->timeout % 1000) * 1000;

	FD_ZERO (&fds);
	FD_SET (waith->request.sockfd, &fds);

	pollres = select (waith->request.sockfd, &fds, &fds, &fds, &tv);
	freeaddrinfo (res);
	if (pollres == 0) {
		return WAITRESS_RET_TIMEOUT;
	} else if (pollres == -1) {
		return WAITRESS_RET_ERR;
	}
	/* check connect () return value */
	waitress_getsockopt (waith->request.sockfd, SOL_SOCKET, SO_ERROR, &pollres,
			&pollresSize);
	if (pollres != 0) {
		return WAITRESS_RET_CONNECT_REFUSED;
	}

	if (waith->url.tls) {
		WaitressReturn_t wRet;

		/* set up proxy tunnel */
		if (WaitressProxyEnabled (waith)) {
			char buf[256];
			size_t size;
			waitress_snprintf (buf, sizeof (buf), "CONNECT %s:%s HTTP/"
					WAITRESS_HTTP_VERSION "\r\n"
					"Host: %s:%s\r\n"
					"Proxy-Connection: close\r\n",
					waith->url.host, WaitressDefaultPort (&waith->url),
					waith->url.host, WaitressDefaultPort (&waith->url));
			WRITE_RET (buf, strlen (buf));

			/* write authorization headers */
			if (WaitressFormatAuthorization (waith, &waith->proxy, "Proxy-",
					buf, WAITRESS_BUFFER_SIZE)) {
				WRITE_RET (buf, strlen (buf));
			}

			WRITE_RET ("\r\n", 2);

			if ((wRet = WaitressReceiveHeaders (waith, &size)) !=
					WAITRESS_RET_OK) {
				return wRet;
			}
		}

#if WAITRESS_USE_GNUTLS
		if (gnutls_handshake (waith->request.tlsSession) != GNUTLS_E_SUCCESS) {
			return WAITRESS_RET_TLS_HANDSHAKE_ERR;
		}
#endif
#if WAITRESS_USE_POLARSSL
		if (ssl_handshake(&waith->request.sslCtx->ssl) != 0) {
			return WAITRESS_RET_TLS_HANDSHAKE_ERR;
		}
#endif

		if ((wRet = WaitressTlsVerify (waith)) != WAITRESS_RET_OK) {
			return wRet;
		}

		/* now we can talk encrypted */
		waith->request.read = WaitressTlsRead;
		waith->request.write = WaitressTlsWrite;
	}

	return WAITRESS_RET_OK;
}

/*	Write http header/post data to socket
 */
static WaitressReturn_t WaitressSendRequest (WaitressHandle_t *waith) {
	const char *path;
	char * buf;
	WaitressReturn_t wRet = WAITRESS_RET_OK;

	assert (waith != NULL);
	assert (waith->request.buf != NULL);

	path = waith->url.path;
	buf = waith->request.buf;

	if (waith->url.path == NULL) {
		/* avoid NULL pointer deref */
		path = "";
	} else if (waith->url.path[0] == '/') {
		/* most servers don't like "//" */
		++path;
	}

	/* send request */
	if (WaitressProxyEnabled (waith) && !waith->url.tls) {
		waitress_snprintf (buf, WAITRESS_BUFFER_SIZE,
			"%s http://%s:%s/%s HTTP/" WAITRESS_HTTP_VERSION "\r\n",
			(waith->method == WAITRESS_METHOD_GET ? "GET" : "POST"),
			waith->url.host,
			WaitressDefaultPort (&waith->url), path);
	} else {
		waitress_snprintf (buf, WAITRESS_BUFFER_SIZE,
			"%s /%s HTTP/" WAITRESS_HTTP_VERSION "\r\n",
			(waith->method == WAITRESS_METHOD_GET ? "GET" : "POST"),
			path);
	}
	WRITE_RET (buf, strlen (buf));

	waitress_snprintf (buf, WAITRESS_BUFFER_SIZE,
			"Host: %s\r\nUser-Agent: " PACKAGE "\r\nConnection: Close\r\n",
			waith->url.host);
	WRITE_RET (buf, strlen (buf));

	if (waith->method == WAITRESS_METHOD_POST && waith->postData != NULL) {
		waitress_snprintf (buf, WAITRESS_BUFFER_SIZE, "Content-Length: "
				waitress_size_t_spec "\r\n", strlen (waith->postData));
		WRITE_RET (buf, strlen (buf));
	}

	/* write authorization headers */
	if (WaitressFormatAuthorization (waith, &waith->url, "", buf,
			WAITRESS_BUFFER_SIZE)) {
		WRITE_RET (buf, strlen (buf));
	}
	/* don't leak proxy credentials to destination server if tls is used */
	if (!waith->url.tls &&
			WaitressFormatAuthorization (waith, &waith->proxy, "Proxy-",
			buf, WAITRESS_BUFFER_SIZE)) {
		WRITE_RET (buf, strlen (buf));
	}
	
	if (waith->extraHeaders != NULL) {
		WRITE_RET (waith->extraHeaders, strlen (waith->extraHeaders));
	}
	
	WRITE_RET ("\r\n", 2);

	if (waith->method == WAITRESS_METHOD_POST && waith->postData != NULL) {
		WRITE_RET (waith->postData, strlen (waith->postData));
	}

	return WAITRESS_RET_OK;
}

/*	receive response headers
 *	@param Waitress handle
 *	@param return unhandled bytes count in buf
 */
static WaitressReturn_t WaitressReceiveHeaders (WaitressHandle_t *waith,
		size_t *retRemaining) {
	char * buf = waith->request.buf;
	size_t bufFilled = 0, recvSize = 0;
	char *nextLine = NULL, *thisLine = NULL;
	enum {HDRM_HEAD, HDRM_LINES, HDRM_FINISHED} hdrParseMode = HDRM_HEAD;
	WaitressReturn_t wRet = WAITRESS_RET_OK;

	assert (waith != NULL);
	assert (waith->request.buf != NULL);

	buf = waith->request.buf;

	/* receive answer */
	nextLine = buf;
	while (hdrParseMode != HDRM_FINISHED) {
		READ_RET (buf+bufFilled, WAITRESS_BUFFER_SIZE-1 - bufFilled, &recvSize);
		if (recvSize == 0) {
			/* connection closed too early */
			return WAITRESS_RET_CONNECTION_CLOSED;
		}
		bufFilled += recvSize;
		buf[bufFilled] = '\0';
		thisLine = buf;

		/* split */
		while (hdrParseMode != HDRM_FINISHED &&
				(nextLine = WaitressGetline (thisLine)) != NULL) {

			int httpStatusCode = -1;

			switch (hdrParseMode) {
				/* Status code */
				case HDRM_HEAD:
					httpStatusCode = WaitressParseStatusline (thisLine);

					switch (httpStatusCode) {
						case 200:
						case 206:
							hdrParseMode = HDRM_LINES;
							break;

						case 400:
							return WAITRESS_RET_BAD_REQUEST;
							break;

						case 403:
							return WAITRESS_RET_FORBIDDEN;
							break;

						case 404:
							return WAITRESS_RET_NOTFOUND;
							break;

						case 407:
							/* Fix for GlobalPandora.com. Proxy sometimes 'miss' credentials.
							 * Same request send again is handled properly.
							 */
							if (WaitressProxyEnabled (waith))
								return WAITRESS_RET_RETRY;
							else
								return WAITRESS_RET_STATUS_UNKNOWN;
							break;

						case -1:
							/* ignore invalid line */
							break;

						default:
							return WAITRESS_RET_STATUS_UNKNOWN;
							break;
					}
					break;

				/* Everything else, except status code */
				case HDRM_LINES:
					/* empty line => content starts here */
					if (*thisLine == '\0') {
						hdrParseMode = HDRM_FINISHED;
					} else {
						/* parse header: "key: value", ignore invalid lines */
						char *key = thisLine, *val;

						val = strchr (thisLine, ':');
						if (val != NULL) {
							*val++ = '\0';
							while (*val != '\0' && isspace ((unsigned char) *val)) {
								++val;
							}
							WaitressHandleHeader (waith, key, val);
						}
					}
					break;

				default:
					break;
			} /* end switch */
			thisLine = nextLine;
		} /* end while strchr */
		memmove (buf, thisLine, bufFilled-(thisLine-buf));
		bufFilled -= (thisLine-buf);
	} /* end while hdrParseMode */

	*retRemaining = bufFilled;

	return wRet;
}

/*	read response header and data
 */
static WaitressReturn_t WaitressReceiveResponse (WaitressHandle_t *waith) {
	char * buf;
	size_t recvSize = 0;
	WaitressReturn_t wRet = WAITRESS_RET_OK;

	assert (waith != NULL);
	assert (waith->request.buf != NULL);

	buf = waith->request.buf;

	if ((wRet = WaitressReceiveHeaders (waith, &recvSize)) != WAITRESS_RET_OK) {
		return wRet;
	}

	do {
		/* data must be \0-terminated for chunked handler */
		buf[recvSize] = '\0';
		switch (waith->request.dataHandler (waith, buf, recvSize)) {
			case WAITRESS_HANDLER_DONE:
				return WAITRESS_RET_OK;
				break;

			case WAITRESS_HANDLER_ERR:
				return WAITRESS_RET_DECODING_ERR;
				break;

			case WAITRESS_HANDLER_ABORTED:
				return WAITRESS_RET_CB_ABORT;
				break;

			case WAITRESS_HANDLER_CONTINUE:
				/* go on */
				break;
		}
		if (waith->request.contentLengthKnown &&
				waith->request.contentReceived >= waith->request.contentLength) {
			/* don’t call read() again if we know the body’s size and have all
			 * of it already */
			break;
		}
		READ_RET (buf, WAITRESS_BUFFER_SIZE-1, &recvSize);
	} while (recvSize > 0);

	return WAITRESS_RET_OK;
}

/*	Receive data from host and call *callback ()
 *	@param waitress handle
 *	@return WaitressReturn_t
 */
WaitressReturn_t WaitressFetchCall (WaitressHandle_t *waith) {
	WaitressReturn_t wRet = WAITRESS_RET_OK;

	/* initialize */
	memset (&waith->request, 0, sizeof (waith->request));
	waith->request.sockfd = -1;
	waith->request.dataHandler = WaitressHandleIdentity;
	waith->request.read = WaitressOrdinaryRead;
	waith->request.write = WaitressOrdinaryWrite;
	waith->request.contentLengthKnown = false;
	waith->request.retriesLeft = 3;

	if (waith->url.tls) {
#if WAITRESS_USE_GNUTLS
		gnutls_init (&waith->request.tlsSession, GNUTLS_CLIENT);
		gnutls_set_default_priority (waith->request.tlsSession);

		gnutls_certificate_allocate_credentials (&waith->tlsCred);
		if (gnutls_credentials_set (waith->request.tlsSession,
			GNUTLS_CRD_CERTIFICATE,
			waith->tlsCred) != GNUTLS_E_SUCCESS) {
				return WAITRESS_RET_ERR;
		}

		/* set up custom read/write functions */
		gnutls_transport_set_ptr (waith->request.tlsSession,
			(gnutls_transport_ptr_t) waith);
		gnutls_transport_set_pull_function (waith->request.tlsSession,
			WaitressPollRead);
		gnutls_transport_set_push_function (waith->request.tlsSession,
			WaitressPollWrite);
#endif

#if WAITRESS_USE_POLARSSL
		waith->request.sslCtx = malloc(sizeof(polarssl_ctx));
		memset(waith->request.sslCtx, 0, sizeof(polarssl_ctx));
		entropy_init(&waith->request.sslCtx->entrophy);
		ctr_drbg_init(&waith->request.sslCtx->rnd, entropy_func, &waith->request.sslCtx->entrophy, "libwaitress", 11);
		ssl_init(&waith->request.sslCtx->ssl);
		ssl_set_endpoint(&waith->request.sslCtx->ssl, SSL_IS_CLIENT);
		ssl_set_authmode(&waith->request.sslCtx->ssl, SSL_VERIFY_NONE);
		ssl_set_rng(&waith->request.sslCtx->ssl, ctr_drbg_random, &waith->request.sslCtx->rnd);
		ssl_set_ciphersuites(&waith->request.sslCtx->ssl, ssl_default_ciphersuites);
		ssl_set_session(&waith->request.sslCtx->ssl, 1, 600, &waith->request.sslCtx->session);
		ssl_set_bio(&waith->request.sslCtx->ssl,
			WaitressPollRead, waith,
			WaitressPollWrite, waith);
#endif
	}

	/* buffer is required for connect already */
	waith->request.buf = malloc (WAITRESS_BUFFER_SIZE *
			sizeof (*waith->request.buf));

	/* request */
	while (waith->request.retriesLeft-- > 0) {
		if ((wRet = WaitressConnect (waith)) == WAITRESS_RET_OK) {
			if ((wRet = WaitressSendRequest (waith)) == WAITRESS_RET_OK) {
				wRet = WaitressReceiveResponse (waith);
			}
			if (waith->url.tls) {
#if WAITRESS_USE_GNUTLS
				gnutls_bye (waith->request.tlsSession, GNUTLS_SHUT_RDWR);
#endif
			}
		}

		if (wRet != WAITRESS_RET_RETRY)
			break;
	}

	/* cleanup */
	if (waith->url.tls) {
#if WAITRESS_USE_GNUTLS
		gnutls_deinit (waith->request.tlsSession);
		gnutls_certificate_free_credentials (waith->tlsCred);
#endif

#if WAITRESS_USE_POLARSSL
		ssl_free(&waith->request.sslCtx->ssl);
		free(waith->request.sslCtx);
#endif
	}
	if (waith->request.sockfd != -1) {
		waitress_close (waith->request.sockfd);
	}
	free (waith->request.buf);

	if (wRet == WAITRESS_RET_OK &&
			waith->request.contentReceived < waith->request.contentLength) {
		return WAITRESS_RET_PARTIAL_FILE;
	}
	return wRet;
}

const char *WaitressErrorToStr (WaitressReturn_t wRet) {
	switch (wRet) {
		case WAITRESS_RET_OK:
			return "Everything's fine :)";
			break;

		case WAITRESS_RET_ERR:
			return "Unknown.";
			break;

		case WAITRESS_RET_STATUS_UNKNOWN:
			return "Unknown HTTP status code.";
			break;

		case WAITRESS_RET_NOTFOUND:
			return "File not found.";
			break;
		
		case WAITRESS_RET_FORBIDDEN:
			return "Forbidden.";
			break;

		case WAITRESS_RET_CONNECT_REFUSED:
			return "Connection refused.";
			break;

		case WAITRESS_RET_RETRY:
			return "Retry.";
			break;

		case WAITRESS_RET_SOCK_ERR:
			return "Socket error.";
			break;

		case WAITRESS_RET_GETADDR_ERR:
			return "getaddr failed.";
			break;

		case WAITRESS_RET_CB_ABORT:
			return "Callback aborted request.";
			break;

		case WAITRESS_RET_PARTIAL_FILE:
			return "Partial file.";
			break;
	
		case WAITRESS_RET_TIMEOUT:
			return "Timeout.";
			break;

		case WAITRESS_RET_READ_ERR:
			return "Read error.";
			break;

		case WAITRESS_RET_CONNECTION_CLOSED:
			return "Connection closed by remote host.";
			break;

		case WAITRESS_RET_DECODING_ERR:
			return "Invalid encoded data.";
			break;

		case WAITRESS_RET_TLS_WRITE_ERR:
			return "TLS write failed.";
			break;

		case WAITRESS_RET_TLS_READ_ERR:
			return "TLS read failed.";
			break;

		case WAITRESS_RET_TLS_HANDSHAKE_ERR:
			return "TLS handshake failed.";
			break;

		case WAITRESS_RET_TLS_FINGERPRINT_MISMATCH:
			return "TLS fingerprint mismatch.";
			break;

		default:
			{
				static char errorMessage[65];
				waitress_snprintf(errorMessage, 64, "No error message available for code %d.", wRet);
				errorMessage[64] = 0;
				return errorMessage;
			}
			break;
	}
}

#ifdef TEST
/* test cases for libwaitress */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "waitress.h"

#define streq(a,b) (strcmp(a,b) == 0)

/*	string equality test (memory location or content)
 */
static bool streqtest (const char *x, const char *y) {
	return (x == y) || (x != NULL && y != NULL && streq (x, y));
}

/*	test WaitressSplitUrl
 *	@param tested url
 *	@param expected user
 *	@param expected password
 *	@param expected host
 *	@param expected port
 *	@param expected path
 */
static void compareUrl (const char *url, const char *user,
		const char *password, const char *host, const char *port,
		const char *path) {
	WaitressUrl_t splitUrl;
	bool userTest, passwordTest, hostTest, portTest, pathTest, overallTest;

	memset (&splitUrl, 0, sizeof (splitUrl));

	WaitressSplitUrl (url, &splitUrl);

	userTest = streqtest (splitUrl.user, user);
	passwordTest = streqtest (splitUrl.password, password);
	hostTest = streqtest (splitUrl.host, host);
	portTest = streqtest (splitUrl.port, port);
	pathTest = streqtest (splitUrl.path, path);

	overallTest = userTest && passwordTest && hostTest && portTest && pathTest;

	if (!overallTest) {
		printf ("FAILED test(s) for %s\n", url);
		if (!userTest) {
			printf ("user: %s vs %s\n", splitUrl.user, user);
		}
		if (!passwordTest) {
			printf ("password: %s vs %s\n", splitUrl.password, password);
		}
		if (!hostTest) {
			printf ("host: %s vs %s\n", splitUrl.host, host);
		}
		if (!portTest) {
			printf ("port: %s vs %s\n", splitUrl.port, port);
		}
		if (!pathTest) {
			printf ("path: %s vs %s\n", splitUrl.path, path);
		}
	} else {
		printf ("OK for %s\n", url);
	}
}

/*	compare two strings
 */
void compareStr (const char *result, const char *expected) {
	if (!streq (result, expected)) {
		printf ("FAIL for %s, result was %s\n", expected, result);
	} else {
		printf ("OK for %s\n", expected);
	}
}

/*	test entry point
 */
int main () {
	/* WaitressSplitUrl tests */
	compareUrl ("http://www.example.com/", NULL, NULL, "www.example.com", NULL,
			"");
	compareUrl ("http://www.example.com", NULL, NULL, "www.example.com", NULL,
			NULL);
	compareUrl ("http://www.example.com:80/", NULL, NULL, "www.example.com",
			"80", "");
	compareUrl ("http://www.example.com:/", NULL, NULL, "www.example.com", "",
			"");
	compareUrl ("http://:80/", NULL, NULL, "", "80", "");
	compareUrl ("http://www.example.com/foobar/barbaz", NULL, NULL,
			"www.example.com", NULL, "foobar/barbaz");
	compareUrl ("http://www.example.com:80/foobar/barbaz", NULL, NULL,
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo:bar@www.example.com:80/foobar/barbaz", "foo", "bar",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo:@www.example.com:80/foobar/barbaz", "foo", "",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo@www.example.com:80/foobar/barbaz", "foo", NULL,
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://:foo@www.example.com:80/foobar/barbaz", "", "foo",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://:@:80", "", "", "", "80", NULL);
	compareUrl ("http://", NULL, NULL, NULL, NULL, NULL);
	compareUrl ("http:///", NULL, NULL, "", NULL, "");
	compareUrl ("http://foo:bar@", "foo", "bar", "", NULL, NULL);

	/* WaitressBase64Encode tests */
	compareStr (WaitressBase64Encode ("M"), "TQ==");
	compareStr (WaitressBase64Encode ("Ma"), "TWE=");
	compareStr (WaitressBase64Encode ("Man"), "TWFu");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy dog."),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy dog"),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2c=");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy do"),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkbw==");

	return EXIT_SUCCESS;
}
#endif /* TEST */

