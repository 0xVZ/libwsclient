#include "wsclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <signal.h>
#include <pthread.h>

//Define errors
char *errors[] = {
		"Unknown error occured",
		"Error while getting address info",
		"Could connect to any address returned by getaddrinfo",
		"Error receiving data in client run thread",
		"Error during libwsclient_close",
		"Error sending while handling control frame",
		"Received masked frame from server",
		"Got null pointer during message dispatch",
		"Attempted to send after close frame was sent",
		"Attempted to send during connect",
		"Attempted to send null payload",
		"Attempted to send too much data",
		"Error during send in libwsclient_send",
		"Remote end closed connection during handshake",
		"Problem receiving data during handshake",
		NULL
};

int libwsclient_flags; //global flags variable

void libwsclient_run(wsclient *c) {
	if (c->flags & CLIENT_CONNECTING) {
		pthread_join(c->handshake_thread, NULL);
		pthread_mutex_lock(&c->lock);
		c->flags &= ~CLIENT_CONNECTING;
		free(c->URI);
		c->URI = NULL;
		pthread_mutex_unlock(&c->lock);
	}
	if (c->sockfd) {
		pthread_create(&c->run_thread, NULL, libwsclient_run_thread, (void *)c);
	}
}

void *libwsclient_run_thread(void *ptr) {
	wsclient *c = (wsclient *) ptr;
	wsclient_error *err = NULL;
	char buf[1024];
	int n, i;

	do {
		memset(buf, 0, 1024);
		n = _libwsclient_read(c, buf, 1023);
		for (i = 0; i < n; i++) {
			libwsclient_in_data(c, buf[i]);
		}
	} while (n > 0);

	if (n < 0) {
		if (c->onerror) {
			err = libwsclient_new_error(WS_RUN_THREAD_RECV_ERR);
			err->extra_code = n;
			c->onerror(c, err);
			free(err);
			err = NULL;
		}

	}

	if (c->onclose) {
		c->onclose(c);
	}

	close(c->sockfd);
	free(c);
	return NULL;
}

void libwsclient_finish(wsclient *client) {
	if (client->run_thread) {
		pthread_join(client->run_thread, NULL);
	}
}

void libwsclient_onclose(wsclient *client, int (*cb)(wsclient *c)) {
	pthread_mutex_lock(&client->lock);
	client->onclose = cb;
	pthread_mutex_unlock(&client->lock);
}

void libwsclient_onopen(wsclient *client, int (*cb)(wsclient *c)) {
	pthread_mutex_lock(&client->lock);
	client->onopen = cb;
	pthread_mutex_unlock(&client->lock);
}

void libwsclient_onmessage(wsclient *client, int (*cb)(wsclient *c, wsclient_message *msg)) {
	pthread_mutex_lock(&client->lock);
	client->onmessage = cb;
	pthread_mutex_unlock(&client->lock);
}

void libwsclient_onerror(wsclient *client, int (*cb)(wsclient *c, wsclient_error *err)) {
	pthread_mutex_lock(&client->lock);
	client->onerror = cb;
	pthread_mutex_unlock(&client->lock);
}

void libwsclient_close(wsclient *client) {
	wsclient_error *err = NULL;
	char data[6];
	int i = 0, n, mask_int;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	srand(tv.tv_sec * tv.tv_usec);
	mask_int = rand();
	data[0] = 0x88;
	data[1] = 0x80;
	memcpy(data+2, &mask_int, 4);

	pthread_mutex_lock(&client->send_lock);
	do {
		n = _libwsclient_write(client, data, 6);
		i += n;
	} while (i < 6 && n > 0);
	pthread_mutex_unlock(&client->send_lock);

	if (n < 0) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_DO_CLOSE_SEND_ERR);
			err->extra_code = n;
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return;
	}

	pthread_mutex_lock(&client->lock);
	client->flags |= CLIENT_SENT_CLOSE_FRAME;
	pthread_mutex_unlock(&client->lock);
}

void libwsclient_handle_control_frame(wsclient *c, wsclient_frame *ctl_frame) {
	wsclient_error *err = NULL;
	wsclient_frame *ptr = NULL;
	int i, n = 0;
	char mask[4];
	int mask_int;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	srand(tv.tv_sec * tv.tv_usec);
	mask_int = rand();
	memcpy(mask, &mask_int, 4);

	pthread_mutex_lock(&c->lock);
	switch (ctl_frame->opcode) {
	case 0x8:
		//close frame
		if ((c->flags & CLIENT_SENT_CLOSE_FRAME) == 0) {
			//server request close.  Send close frame as acknowledgement.
			for (i=0;i<ctl_frame->payload_len;i++)
				*(ctl_frame->rawdata + ctl_frame->payload_offset + i) ^= (mask[i % 4] & 0xff); //mask payload
			*(ctl_frame->rawdata + 1) |= 0x80; //turn mask bit on
			i = 0;
			pthread_mutex_lock(&c->send_lock);
			while (i < ctl_frame->payload_offset + ctl_frame->payload_len && n >= 0) {
				n = _libwsclient_write(c, ctl_frame->rawdata + i, ctl_frame->payload_offset + ctl_frame->payload_len - i);
				i += n;
			}
			pthread_mutex_unlock(&c->send_lock);
			if (n < 0) {
				if (c->onerror) {
					err = libwsclient_new_error(WS_HANDLE_CTL_FRAME_SEND_ERR);
					err->extra_code = n;
					c->onerror(c, err);
					free(err);
					err = NULL;
				}
			}
		}
		c->flags |= CLIENT_SHOULD_CLOSE;
		break;

	case 0x9:
		// Received ping frame
		if ((c->flags & CLIENT_SHOULD_CLOSE) == 0) {
			// Server sent ping.  Send pong frame as acknowledgement.
			for (i=0;i<ctl_frame->payload_len;i++)
				*(ctl_frame->rawdata + ctl_frame->payload_offset + i) ^= (mask[i % 4] & 0xff); //mask payload
			*(ctl_frame->rawdata + 1) |= 0x80; //turn mask bit on
			i = 0;
			// change opcode to 0xA (Pong Frame)
			*(ctl_frame->rawdata) = (*(ctl_frame->rawdata) & 0xf0) | 0xA;
			pthread_mutex_lock(&c->send_lock);
			while (i < ctl_frame->payload_offset + ctl_frame->payload_len && n >= 0) {
				n = _libwsclient_write(c, ctl_frame->rawdata + i, ctl_frame->payload_offset + ctl_frame->payload_len - i);
				i += n;
			}
			pthread_mutex_unlock(&c->send_lock);
			if (n < 0) {
				if (c->onerror) {
					err = libwsclient_new_error(WS_HANDLE_CTL_FRAME_SEND_ERR);
					err->extra_code = n;
					c->onerror(c, err);
					free(err);
					err = NULL;
				}
			}
		}
		break;

	case 0xA:
		// Received pong frame. Nothing to do.
		break;

	default:
		fprintf(stderr, "Unhandled control frame received.  Opcode: %d\n", ctl_frame->opcode);
		break;
	}

	ptr = ctl_frame->prev_frame; //This very well may be a NULL pointer, but just in case we preserve it.
	free(ctl_frame->rawdata);
	memset(ctl_frame, 0, sizeof(wsclient_frame));
	ctl_frame->prev_frame = ptr;
	ctl_frame->rawdata = (char *) malloc(FRAME_CHUNK_LENGTH);
	memset(ctl_frame->rawdata, 0, FRAME_CHUNK_LENGTH);
	pthread_mutex_unlock(&c->lock);
}

inline void libwsclient_in_data(wsclient *c, char in) {
	wsclient_frame *current = NULL, *new = NULL;

	pthread_mutex_lock(&c->lock);
	if (c->current_frame == NULL) {
		c->current_frame = (wsclient_frame *) malloc(sizeof(wsclient_frame));
		memset(c->current_frame, 0, sizeof(wsclient_frame));
		c->current_frame->payload_len = -1;
		c->current_frame->rawdata_sz = FRAME_CHUNK_LENGTH;
		c->current_frame->rawdata = (char *) malloc(c->current_frame->rawdata_sz);
		memset(c->current_frame->rawdata, 0, c->current_frame->rawdata_sz);
	}

	current = c->current_frame;
	if (current->rawdata_idx >= current->rawdata_sz) {
		current->rawdata_sz += FRAME_CHUNK_LENGTH;
		current->rawdata = (char *)realloc(current->rawdata, current->rawdata_sz);
		memset(current->rawdata + current->rawdata_idx, 0, current->rawdata_sz - current->rawdata_idx);
	}

	*(current->rawdata + current->rawdata_idx++) = in;
	pthread_mutex_unlock(&c->lock);
	if (libwsclient_complete_frame(c, current) == 1) {
		if (current->fin == 1) {
			//is control frame
			if ((current->opcode & 0x08) == 0x08) {
				libwsclient_handle_control_frame(c, current);
			} else {
				libwsclient_dispatch_message(c, current);
				c->current_frame = NULL;
			}
		} else {
			new = (wsclient_frame *) malloc(sizeof(wsclient_frame));
			memset(new, 0, sizeof(wsclient_frame));
			new->payload_len = -1;
			new->rawdata = (char *) malloc(FRAME_CHUNK_LENGTH);
			memset(new->rawdata, 0, FRAME_CHUNK_LENGTH);
			new->prev_frame = current;
			current->next_frame = new;
			c->current_frame = new;
		}
	}
}

void libwsclient_dispatch_message(wsclient *c, wsclient_frame *current) {
	unsigned long long message_payload_len, message_offset;
	int message_opcode, i;
	char *message_payload;
	wsclient_frame *first = NULL;
	wsclient_frame *this = NULL;
	wsclient_frame *next = NULL;
	wsclient_message *msg = NULL;
	wsclient_error *err = NULL;

	if (current == NULL) {
		if (c->onerror) {
			err = libwsclient_new_error(WS_DISPATCH_MESSAGE_NULL_PTR_ERR);
			c->onerror(c, err);
			free(err);
			err = NULL;
		}
		return;
	}

	message_offset = 0;
	message_payload_len = current->payload_len;
	for (;current->prev_frame != NULL;current = current->prev_frame) {
		message_payload_len += current->payload_len;
	}

	first = current;
	message_opcode = current->opcode;

	message_payload = (char *) malloc(message_payload_len + 1);
	memset(message_payload, 0, message_payload_len + 1);
	for (;current != NULL; current = current->next_frame) {
		memcpy(message_payload + message_offset, current->rawdata + current->payload_offset, current->payload_len);
		message_offset += current->payload_len;
	}

	next = first;
	while (next != NULL) {
		this = next;
		next = this->next_frame;
		if (this->rawdata != NULL) {
			free(this->rawdata);
		}

		free(this);
	}
	
	msg = (wsclient_message *) malloc(sizeof(wsclient_message));
	memset(msg, 0, sizeof(wsclient_message));
	msg->opcode = message_opcode;
	msg->payload_len = message_offset;
	msg->payload = message_payload;
	if (c->onmessage != NULL) {
		c->onmessage(c, msg);
	} else {
		fprintf(stderr, "No onmessage call back registered with libwsclient.\n");
	}

	free(msg->payload);
	free(msg);
}

int libwsclient_complete_frame(wsclient *c, wsclient_frame *frame) {
	wsclient_error *err = NULL;
	int payload_len_short, i;
	unsigned long long payload_len = 0;

	if (frame->rawdata_idx < 2) {
		return 0;
	}

	frame->fin = (*(frame->rawdata) & 0x80) == 0x80 ? 1 : 0;
	frame->opcode = *(frame->rawdata) & 0x0f;
	frame->payload_offset = 2;
	if ((*(frame->rawdata+1) & 0x80) != 0x0) {
		if (c->onerror) {
			err = libwsclient_new_error(WS_COMPLETE_FRAME_MASKED_ERR);
			c->onerror(c, err);
			free(err);
			err = NULL;
		}
		pthread_mutex_lock(&c->lock);
		c->flags |= CLIENT_SHOULD_CLOSE;
		pthread_mutex_unlock(&c->lock);
		return 0;
	}
	payload_len_short = *(frame->rawdata+1) & 0x7f;
	switch (payload_len_short) {
	case 126:
		if (frame->rawdata_idx < 4) {
			return 0;
		}

		for (i = 0; i < 2; i++) {
			memcpy((void *)&payload_len+i, frame->rawdata+3-i, 1);
		}

		frame->payload_offset += 2;
		frame->payload_len = payload_len;
		break;

	case 127:
		if (frame->rawdata_idx < 10) {
			return 0;
		}

		for (i = 0; i < 8; i++) {
			memcpy((void *)&payload_len+i, frame->rawdata+9-i, 1);
		}

		frame->payload_offset += 8;
		frame->payload_len = payload_len;
		break;

	default:
		frame->payload_len = payload_len_short;
		break;

	}

	if (frame->rawdata_idx < frame->payload_offset + frame->payload_len) {
		return 0;
	}

	return 1;
}

int libwsclient_open_connection(const char *host, const char *port) {
	struct addrinfo hints, *servinfo, *p;
	int rv, sockfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
		return WS_OPEN_CONNECTION_ADDRINFO_ERR;
	}

	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol)) == -1) {
			continue;
		}
		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			continue;
		}
		break;
	}
	
	freeaddrinfo(servinfo);

	if (p == NULL) {
		return WS_OPEN_CONNECTION_ADDRINFO_EXHAUSTED_ERR;
	}

	return sockfd;
}

wsclient *libwsclient_new(const char *URI) {
	wsclient *client = (wsclient *) malloc(sizeof(wsclient));
	if (!client) {
		fprintf(stderr, "Unable to allocate memory in libwsclient_new.\n");
		exit(WS_EXIT_MALLOC);
	}

	memset(client, 0, sizeof(wsclient));
	if (pthread_mutex_init(&client->lock, NULL) != 0) {
		fprintf(stderr, "Unable to init mutex in libwsclient_new.\n");
		exit(WS_EXIT_PTHREAD_MUTEX_INIT);
	}
	
	if (pthread_mutex_init(&client->send_lock, NULL) != 0) {
		fprintf(stderr, "Unable to init send lock in libwsclient_new.\n");
		exit(WS_EXIT_PTHREAD_MUTEX_INIT);
	}
	
	pthread_mutex_lock(&client->lock);
	client->URI = (char *) malloc(strlen(URI) + 1);
	if (!client->URI) {
		fprintf(stderr, "Unable to allocate memory in libwsclient_new.\n");
		exit(WS_EXIT_MALLOC);
	}
	
	memset(client->URI, 0, strlen(URI) + 1);
	strncpy(client->URI, URI, strlen(URI));
	client->flags |= CLIENT_CONNECTING;
	pthread_mutex_unlock(&client->lock);

	if (pthread_create(&client->handshake_thread, NULL, libwsclient_handshake_thread, (void *)client)) {
		fprintf(stderr, "Unable to create handshake thread.\n");
		exit(WS_EXIT_PTHREAD_CREATE);
	}
	
	return client;
}

void *libwsclient_handshake_thread(void *ptr) {
	wsclient *client = (wsclient *) ptr;
	wsclient_error *err = NULL;
	char request_headers[1024];
	char scheme[10];
	char host[255];
	char port[10];
	char path[255];
	char recv_buf[1024];
	char *URI_copy = NULL, *p = NULL, *rcv = NULL, *tok = NULL;
	int i, z, sockfd, n, flags = 0;
	
	URI_copy = (char *) malloc(strlen(client->URI)+1);
	if (!URI_copy) {
		fprintf(stderr, "Unable to allocate memory in libwsclient_handshake_thread.\n");
		exit(WS_EXIT_MALLOC);
	}

	memset(URI_copy, 0, strlen(client->URI)+1);
	strncpy(URI_copy, client->URI, strlen(client->URI));
	p = strstr(URI_copy, "://");
	if (p == NULL) {
		fprintf(stderr, "Malformed or missing scheme for URI.\n");
		exit(WS_EXIT_BAD_SCHEME);
	}

	strncpy(scheme, URI_copy, p-URI_copy);
	scheme[p-URI_copy] = '\0';
	if (strcmp(scheme, "ws") != 0 && strcmp(scheme, "wss") != 0) {
		fprintf(stderr, "Invalid scheme for URI: %s\n", scheme);
		exit(WS_EXIT_BAD_SCHEME);
	}
	
	if (strcmp(scheme, "ws") == 0) {
		strncpy(port, "80", 9);
	} else {
		strncpy(port, "443", 9);
		pthread_mutex_lock(&client->lock);
		client->flags |= CLIENT_IS_SSL;
		pthread_mutex_unlock(&client->lock);
	}
	
	for (i=p-URI_copy+3,z=0;*(URI_copy+i) != '/' && *(URI_copy+i) != ':' && *(URI_copy+i) != '\0';i++,z++) {
		host[z] = *(URI_copy+i);
	}
	
	host[z] = '\0';
	if (*(URI_copy+i) == ':') {
		i++;
		p = strchr(URI_copy+i, '/');
		if (!p) {
			p = strchr(URI_copy+i, '\0');
		}

		strncpy(port, URI_copy+i, (p - (URI_copy+i)));
		port[p-(URI_copy+i)] = '\0';
		i += p-(URI_copy+i);
	}

	if (*(URI_copy+i) == '\0') {
		//end of URI request path will be /
		strncpy(path, "/", 1);
	} else {
		strncpy(path, URI_copy+i, 254);
	}
	
	free(URI_copy);
	sockfd = libwsclient_open_connection(host, port);

	if (sockfd < 0) {
		if (client->onerror) {
			err = libwsclient_new_error(sockfd);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return NULL;
	}

	if (client->flags & CLIENT_IS_SSL) {
		if ((libwsclient_flags & WS_FLAGS_SSL_INIT) == 0) {
			SSL_library_init();
			SSL_load_error_strings();
			libwsclient_flags |= WS_FLAGS_SSL_INIT;
		}
		client->ssl_ctx = SSL_CTX_new(TLS_client_method());
		client->ssl = SSL_new(client->ssl_ctx);
		SSL_set_fd(client->ssl, sockfd);
		SSL_connect(client->ssl);
	}

	pthread_mutex_lock(&client->lock);
	client->sockfd = sockfd;
	pthread_mutex_unlock(&client->lock);

	snprintf(request_headers, 1024, "GET %s HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: %s\r\nSec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host);
	n = _libwsclient_write(client, request_headers, strlen(request_headers));
	z = 0;
	memset(recv_buf, 0, 1024);

	do {
		n = _libwsclient_read(client, recv_buf + z, 1023 - z);
		z += n;
	} while ((z < 4 || strstr(recv_buf, "\r\n\r\n") == NULL) && n > 0);

	if (n == 0) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_HANDSHAKE_REMOTE_CLOSED_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return NULL;
	}

	if (n < 0) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_HANDSHAKE_RECV_ERR);
			err->extra_code = n;
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return NULL;
	}

	pthread_mutex_lock(&client->lock);
	client->flags &= ~CLIENT_CONNECTING;
	pthread_mutex_unlock(&client->lock);
	if (client->onopen != NULL) {
		client->onopen(client);
	}

	return NULL;
}

wsclient_error *libwsclient_new_error(int errcode) {
	wsclient_error *err = (wsclient_error *) malloc(sizeof(wsclient_error));
	if (!err) {
		fprintf(stderr, "Unable to allocate memory in libwsclient_new_error.\n");
		exit(errcode);
	}

	memset(err, 0, sizeof(wsclient_error));
	err->code = errcode;
	switch (err->code) {
	case WS_OPEN_CONNECTION_ADDRINFO_ERR:
		err->str = *(errors + 1);
		break;
	case WS_OPEN_CONNECTION_ADDRINFO_EXHAUSTED_ERR:
		err->str = *(errors + 2);
		break;
	case WS_RUN_THREAD_RECV_ERR:
		err->str = *(errors + 3);
		break;
	case WS_DO_CLOSE_SEND_ERR:
		err->str = *(errors + 4);
		break;
	case WS_HANDLE_CTL_FRAME_SEND_ERR:
		err->str = *(errors + 5);
		break;
	case WS_COMPLETE_FRAME_MASKED_ERR:
		err->str = *(errors + 6);
		break;
	case WS_DISPATCH_MESSAGE_NULL_PTR_ERR:
		err->str = *(errors + 7);
		break;
	case WS_SEND_AFTER_CLOSE_FRAME_ERR:
		err->str = *(errors + 8);
		break;
	case WS_SEND_DURING_CONNECT_ERR:
		err->str = *(errors + 9);
		break;
	case WS_SEND_NULL_DATA_ERR:
		err->str = *(errors + 10);
		break;
	case WS_SEND_DATA_TOO_LARGE_ERR:
		err->str = *(errors + 11);
		break;
	case WS_SEND_SEND_ERR:
		err->str = *(errors + 12);
		break;
	case WS_HANDSHAKE_REMOTE_CLOSED_ERR:
		err->str = *(errors + 13);
		break;
	case WS_HANDSHAKE_RECV_ERR:
		err->str = *(errors + 14);
		break;
	default:
		err->str = *errors;
		break;
	}

	return err;
}

int libwsclient_send(wsclient *client, char *strdata)  {
	wsclient_error *err = NULL;
	struct timeval tv;
	unsigned char mask[4];
	unsigned int mask_int;
	unsigned long long payload_len;
	unsigned char finNopcode;
	unsigned int payload_len_small;
	unsigned int payload_offset = 6;
	unsigned int len_size;
	unsigned int sent = 0;
	int i;
	unsigned int frame_size;
	char *data;

	if (client->flags & CLIENT_SENT_CLOSE_FRAME) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_SEND_AFTER_CLOSE_FRAME_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return 0;
	}

	if (client->flags & CLIENT_CONNECTING) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_SEND_DURING_CONNECT_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return 0;
	}

	if (strdata == NULL) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_SEND_NULL_DATA_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
		return 0;
	}

	gettimeofday(&tv, NULL);
	srand(tv.tv_usec * tv.tv_sec);
	mask_int = rand();
	memcpy(mask, &mask_int, 4);

	payload_len = strlen(strdata);
	finNopcode = 0x81; //FIN and text opcode.
	if (payload_len <= 125) {
		frame_size = 6 + payload_len;
		payload_len_small = payload_len;
	} else if (payload_len > 125 && payload_len <= 0xffff) {
		frame_size = 8 + payload_len;
		payload_len_small = 126;
		payload_offset += 2;
	} else if (payload_len > 0xffff && payload_len <= 0xffffffffffffffffLL) {
		frame_size = 14 + payload_len;
		payload_len_small = 127;
		payload_offset += 8;
	} else {
		if (client->onerror) {
			err = libwsclient_new_error(WS_SEND_DATA_TOO_LARGE_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}

		return -1;
	}

	data = (char *) malloc(frame_size);
	memset(data, 0, frame_size);
	*data = finNopcode;
	*(data + 1) = payload_len_small | 0x80; //payload length with mask bit on
	if (payload_len_small == 126) {
		payload_len &= 0xffff;
		len_size = 2;
		for (i = 0; i < len_size; i++) {
			*(data + 2 + i) = *((char *) &payload_len + (len_size - i - 1));
		}
	}

	if (payload_len_small == 127) {
		payload_len &= 0xffffffffffffffffLL;
		len_size = 8;
		for (i = 0; i < len_size; i++) {
			*(data+2+i) = *((char *) &payload_len + (len_size - i - 1));
		}
	}

	for (i = 0; i < 4; i++) {
		*(data + (payload_offset - 4) + i) = mask[i];
	}

	memcpy(data + payload_offset, strdata, strlen(strdata));
	for (i = 0; i < strlen(strdata); i++) {
		*(data + payload_offset + i) ^= mask[i % 4] & 0xff;
	}

	sent = 0;
	i = 0;

	pthread_mutex_lock(&client->send_lock);
	while (sent < frame_size && i >= 0) {
		i = _libwsclient_write(client, data+sent, frame_size - sent);
		sent += i;
	}

	pthread_mutex_unlock(&client->send_lock);

	if (i < 0) {
		if (client->onerror) {
			err = libwsclient_new_error(WS_SEND_SEND_ERR);
			client->onerror(client, err);
			free(err);
			err = NULL;
		}
	}

	free(data);
	return sent;
}

ssize_t _libwsclient_read(wsclient *c, void *buf, size_t length) {
	if (c->flags & CLIENT_IS_SSL) {
		return (ssize_t)SSL_read(c->ssl, buf, length);
	} else {
		return recv(c->sockfd, buf, length, 0);
	}
}

ssize_t _libwsclient_write(wsclient *c, const void *buf, size_t length) {
	if (c->flags & CLIENT_IS_SSL) {
		return (ssize_t)SSL_write(c->ssl, buf, length);
	} else {
		return send(c->sockfd, buf, length, 0);
	}
}


