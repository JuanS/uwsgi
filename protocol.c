#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

static size_t get_content_length(char *buf, uint16_t size) {
	int i;
	size_t val = 0;
	for(i=0;i<size;i++) {
		if (buf[i] >= '0' && buf[i] <= '9') {
			val = (val*10) + (buf[i] - '0');
			continue;
		}
		break;
	}

	return val;
}

#ifdef UWSGI_UDP
ssize_t send_udp_message(uint8_t modifier1, char *host, char *message, uint16_t message_size) {

	int fd;
	struct sockaddr_in udp_addr;
	char *udp_port;
	ssize_t ret;
	char udpbuff[1024];

	if (message_size + 4 > 1024)
		return -1;

	udp_port = strchr(host, ':');
	if (udp_port == NULL) {
		return -1;
	}

	udp_port[0] = 0; 

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	memset(&udp_addr, 0, sizeof(struct sockaddr_in));
	udp_addr.sin_family = AF_INET;
	udp_addr.sin_port = htons(atoi(udp_port+1));
	udp_addr.sin_addr.s_addr = inet_addr(host);

	udpbuff[0] = modifier1;
#ifdef __BIG_ENDIAN__
	message_size = uwsgi_swap16(message_size);
#endif

	memcpy(udpbuff+1, &message_size, 2);

	udpbuff[3] = 0;

#ifdef __BIG_ENDIAN__
	message_size = uwsgi_swap16(message_size);
#endif

	memcpy(udpbuff+4, message, message_size);

	ret = sendto(fd, udpbuff, message_size+4, 0, (struct sockaddr *) &udp_addr, sizeof(udp_addr));
	if (ret < 0) {
		uwsgi_error("sendto()");
	}
	close(fd);

	return ret;
	
}
#endif

int uwsgi_enqueue_message(char *host, int port, uint8_t modifier1, uint8_t modifier2, char *message, int size, int timeout) {

	struct pollfd uwsgi_poll;
	struct sockaddr_in uws_addr;
	int cnt;
	struct uwsgi_header uh;

	if (!timeout)
		timeout = 1;

	if (size > 0xFFFF) {
		uwsgi_log( "invalid object (marshalled) size\n");
		return -1;
	}

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	memset(&uws_addr, 0, sizeof(struct sockaddr_in));
	uws_addr.sin_family = AF_INET;
	uws_addr.sin_port = htons(port);
	uws_addr.sin_addr.s_addr = inet_addr(host);

	uwsgi_poll.events = POLLIN;

	if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &uws_addr, sizeof(struct sockaddr_in), timeout, 0)) {
		uwsgi_error("connect()");
		close(uwsgi_poll.fd);
		return -1;
	}

	uh.modifier1 = modifier1;
	uh.pktsize = (uint16_t) size;
	uh.modifier2 = modifier2;

	cnt = write(uwsgi_poll.fd, &uh, 4);
	if (cnt != 4) {
		uwsgi_error("write()");
		close(uwsgi_poll.fd);
		return -1;
	}

	cnt = write(uwsgi_poll.fd, message, size);
	if (cnt != size) {
		uwsgi_error("write()");
		close(uwsgi_poll.fd);
		return -1;
	}

	return uwsgi_poll.fd;
}

ssize_t uwsgi_send_message(int fd, uint8_t modifier1, uint8_t modifier2, char *message, uint16_t size, int pfd, size_t plen, int timeout) {

	struct pollfd uwsgi_mpoll;
	ssize_t cnt;
	struct uwsgi_header uh;
	char buffer[4096];
	ssize_t ret = 0;
	int pret;


	if (!timeout) timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];

	uh.modifier1 = modifier1;
	uh.pktsize = size;
	uh.modifier2 = modifier2;

	cnt = write(fd, &uh, 4);
	if (cnt != 4) {
		uwsgi_error("write()");
		return -1;
	}

	ret += cnt;

	cnt = write(fd, message, size);
	if (cnt != size) {
		uwsgi_error("write()");
		return -1;
	}

	ret += cnt;

	// transfer data from one socket to another
	if (pfd >= 0 && plen > 0) {
		uwsgi_mpoll.fd = pfd;
		uwsgi_mpoll.events = POLLIN;
		
		while(plen > 0) {
			pret = poll(&uwsgi_mpoll, 1, timeout*1000);
			if (pret < 0) {
				uwsgi_error("poll()");
				return -1;
			}
			else if (pret == 0) {
				uwsgi_log("timeout waiting for socket data\n");
				return -1;
			}
			else {
				cnt = read(pfd, buffer, UMIN(4096, plen));
				if (cnt < 0) {
					uwsgi_error("read()");
					return -1;
				}
				else if (cnt == 0) {
					return ret;
				}	
				// send to peer
				if (write(fd, buffer, cnt) != cnt) {
					uwsgi_error("write()");
					return -1;
				}
				ret += cnt;
				plen -= cnt;	
			}
		}
	}


	return ret;
}


int uwsgi_parse_response(struct pollfd *upoll, int timeout, struct uwsgi_header *uh, char *buffer) {
	int rlen, i;

	if (!timeout)
		timeout = 1;
	/* first 4 byte header */
	rlen = poll(upoll, 1, timeout * 1000);

	if (rlen < 0) {
		uwsgi_error("poll()");
		exit(1);
	}
	else if (rlen == 0) {
		uwsgi_log( "timeout. skip request\n");
		close(upoll->fd);
		return 0;
	}
	rlen = read(upoll->fd, uh, 4);
	if (rlen > 0 && rlen < 4) {
		i = rlen;
		while (i < 4) {
			rlen = poll(upoll, 1, timeout * 1000);
			if (rlen < 0) {
				uwsgi_error("poll()");
				exit(1);
			}
			else if (rlen == 0) {
				uwsgi_log( "timeout waiting for header. skip request.\n");
				close(upoll->fd);
				break;
			}
			rlen = read(upoll->fd, (char *) (uh) + i, 4 - i);
			if (rlen <= 0) {
				uwsgi_log( "broken header. skip request.\n");
				close(upoll->fd);
				break;
			}
			i += rlen;
		}
		if (i < 4) {
			return 0;
		}
	}
	else if (rlen <= 0) {
		uwsgi_log( "invalid request header size: %d...skip\n", rlen);
		close(upoll->fd);
		return 0;
	}
	/* big endian ? */
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif

#ifdef UWSGI_DEBUG
	uwsgi_debug("uwsgi payload size: %d (0x%X) modifier1: %d modifier2: %d\n", uh->pktsize, uh->pktsize, uh->modifier1, uh->modifier2);
#endif

	/* check for max buffer size */
	if (uh->pktsize > uwsgi.buffer_size) {
		uwsgi_log( "invalid request block size: %d...skip\n", uh->pktsize);
		close(upoll->fd);
		return 0;
	}

	//uwsgi_log("ready for reading %d bytes\n", wsgi_req.size);

	i = 0;
	while (i < uh->pktsize) {
		rlen = poll(upoll, 1, timeout * 1000);
		if (rlen < 0) {
			uwsgi_error("poll()");
			exit(1);
		}
		else if (rlen == 0) {
			uwsgi_log( "timeout. skip request. (expecting %d bytes, got %d)\n", uh->pktsize, i);
			close(upoll->fd);
			break;
		}
		rlen = read(upoll->fd, buffer + i, uh->pktsize - i);
		if (rlen <= 0) {
			uwsgi_log( "broken vars. skip request.\n");
			close(upoll->fd);
			break;
		}
		i += rlen;
	}


	if (i < uh->pktsize) {
		return 0;
	}

	return 1;
}

int uwsgi_parse_array(char *buffer, uint16_t size, char **argv, uint8_t *argc) {

	char *ptrbuf, *bufferend;
	uint16_t strsize = 0;
	int i;
	
	*argc = 0;

        ptrbuf = buffer;
        bufferend = ptrbuf + size;

	for(i=0;i<size;i++) {
		uwsgi_log("%x %c\n", buffer[i], buffer[i]);
	}

	while (ptrbuf < bufferend) {
                if (ptrbuf + 2 < bufferend) {
                        memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                        strsize = uwsgi_swap16(strsize);
#endif

                        ptrbuf += 2;
                        /* item cannot be null */
                        if (!strsize) continue;

                        if (ptrbuf + strsize <= bufferend) {
                                // item
				argv[*argc] = uwsgi_cheap_string(ptrbuf, strsize);
                                ptrbuf += strsize;
				*argc = *argc + 1;
			}
			else {
				uwsgi_log( "invalid uwsgi array. skip this request.\n");
                        	return -1;
			}
		}
		else {
			uwsgi_log( "invalid uwsgi array. skip this request.\n");
                        return -1;
		}
	}
	

	return 0;
}

int uwsgi_parse_vars(struct wsgi_request *wsgi_req) {

	char *buffer = wsgi_req->buffer;

	char *ptrbuf, *bufferend;

	uint16_t strsize = 0;

	ptrbuf = buffer;
	bufferend = ptrbuf + wsgi_req->uh.pktsize;
	int i, script_name= -1, path_info= -1;

	/* set an HTTP 500 status as default */
	wsgi_req->status = 500;

	while (ptrbuf < bufferend) {
		if (ptrbuf + 2 < bufferend) {
			memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
			strsize = uwsgi_swap16(strsize);
#endif
			/* key cannot be null */
                        if (!strsize) {
                                uwsgi_log( "uwsgi key cannot be null. skip this request.\n");
                                return -1;
                        }
			
			ptrbuf += 2;
			if (ptrbuf + strsize < bufferend) {
				// var key
				wsgi_req->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
				wsgi_req->hvec[wsgi_req->var_cnt].iov_len = strsize;
				ptrbuf += strsize;
				// value can be null (even at the end) so use <=
				if (ptrbuf + 2 <= bufferend) {
					memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
					strsize = uwsgi_swap16(strsize);
#endif
					ptrbuf += 2;
					if (ptrbuf + strsize <= bufferend) {
						//uwsgi_log("uwsgi %.*s = %.*s\n", wsgi_req->hvec[wsgi_req->var_cnt].iov_len, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, strsize, ptrbuf);
						if (!uwsgi_strncmp("SCRIPT_NAME", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->script_name = ptrbuf;
							wsgi_req->script_name_len = strsize;
							script_name = wsgi_req->var_cnt + 1;
#ifdef UWSGI_DEBUG
							uwsgi_debug("SCRIPT_NAME=%.*s\n", wsgi_req->script_name_len, wsgi_req->script_name);
#endif
						}
						else if (!uwsgi_strncmp("PATH_INFO", 9, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->path_info = ptrbuf;
							wsgi_req->path_info_len = strsize;
							path_info = wsgi_req->var_cnt + 1;
#ifdef UWSGI_DEBUG
							uwsgi_debug("PATH_INFO=%.*s\n", wsgi_req->path_info_len, wsgi_req->path_info);
#endif
						}
						else if (!uwsgi_strncmp("SERVER_PROTOCOL", 15, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->protocol = ptrbuf;
							wsgi_req->protocol_len = strsize;
						}
						else if (!uwsgi_strncmp("REQUEST_URI", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->uri = ptrbuf;
							wsgi_req->uri_len = strsize;
						}
						else if (!uwsgi_strncmp("QUERY_STRING", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->query_string = ptrbuf;
							wsgi_req->query_string_len = strsize;
						}
						else if (!uwsgi_strncmp("REQUEST_METHOD", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->method = ptrbuf;
							wsgi_req->method_len = strsize;
						}
						else if (!uwsgi_strncmp("REMOTE_ADDR", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->remote_addr = ptrbuf;
							wsgi_req->remote_addr_len = strsize;
						}
						else if (!uwsgi_strncmp("REMOTE_USER", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->remote_user = ptrbuf;
							wsgi_req->remote_user_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_SCHEME", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->scheme = ptrbuf;
							wsgi_req->scheme_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_SCRIPT", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len )) {
							wsgi_req->script = ptrbuf;
							wsgi_req->script_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_MODULE", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->module = ptrbuf;
							wsgi_req->module_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_CALLABLE", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->callable = ptrbuf;
							wsgi_req->callable_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_PYHOME", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->pyhome = ptrbuf;
							wsgi_req->pyhome_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_CHDIR", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->chdir = ptrbuf;
							wsgi_req->chdir_len = strsize;
						}
						else if (!uwsgi_strncmp("SERVER_NAME", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len) && !uwsgi.vhost_host) {
							wsgi_req->host = ptrbuf;
							wsgi_req->host_len = strsize;
#ifdef UWSGI_DEBUG
							uwsgi_debug("SERVER_NAME=%.*s\n", wsgi_req->host_len, wsgi_req->host);
#endif
						}
						else if (!uwsgi_strncmp("HTTP_HOST", 9, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len) && uwsgi.vhost_host) {
							wsgi_req->host = ptrbuf;
							wsgi_req->host_len = strsize;
#ifdef UWSGI_DEBUG
							uwsgi_debug("HTTP_HOST=%.*s\n", wsgi_req->host_len, wsgi_req->host);
#endif
						}
						else if (!uwsgi_strncmp("HTTPS", 5, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->https = ptrbuf;
							wsgi_req->https_len = strsize;
						}
						else if (!uwsgi_strncmp("CONTENT_LENGTH", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->post_cl = get_content_length(ptrbuf, strsize);
						}

						if (wsgi_req->var_cnt < uwsgi.vec_size - (4 + 1)) {
							wsgi_req->var_cnt++;
						}
						else {
							uwsgi_log( "max vec size reached. skip this header.\n");
							return -1;
						}
						// var value
						wsgi_req->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
						wsgi_req->hvec[wsgi_req->var_cnt].iov_len = strsize;
						//uwsgi_log("%.*s = %.*s\n", wsgi_req->hvec[wsgi_req->var_cnt-1].iov_len, wsgi_req->hvec[wsgi_req->var_cnt-1].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len, wsgi_req->hvec[wsgi_req->var_cnt].iov_base);
						if (wsgi_req->var_cnt < uwsgi.vec_size - (4 + 1)) {
							wsgi_req->var_cnt++;
						}
						else {
							uwsgi_log( "max vec size reached. skip this header.\n");
							return -1;
						}
						ptrbuf += strsize;
					}
					else {
						return -1;
					}
				}
				else {
					return -1;
				}
			}
		}
		else {
			return -1;
		}
	}


	if (uwsgi.manage_script_name) {
		if (uwsgi.apps_cnt > 0 && wsgi_req->path_info_len > 1) {
			// starts with 1 as the 0 app is the default (/) one
			int best_found = 0;
			char *orig_path_info = wsgi_req->path_info;
			int orig_path_info_len = wsgi_req->path_info_len;
			// if SCRIPT_NAME is not allocated, add a slot for it
			if (script_name == -1) {
				if (wsgi_req->var_cnt >= uwsgi.vec_size - (4 + 2)) {
					uwsgi_log( "max vec size reached. skip this header.\n");
                                        return -1;
				}
				wsgi_req->var_cnt++;
				wsgi_req->hvec[wsgi_req->var_cnt].iov_base = "SCRIPT_NAME";
                                wsgi_req->hvec[wsgi_req->var_cnt].iov_len = 11;
				wsgi_req->var_cnt++;
				script_name = wsgi_req->var_cnt;
			}
			for(i=0;i<uwsgi.apps_cnt;i++) {
				//uwsgi_log("app mountpoint = %.*s\n", uwsgi.apps[i].mountpoint_len, uwsgi.apps[i].mountpoint);
				if (orig_path_info_len >= uwsgi.apps[i].mountpoint_len) {
					if (!uwsgi_startswith(orig_path_info, uwsgi.apps[i].mountpoint, uwsgi.apps[i].mountpoint_len) && uwsgi.apps[i].mountpoint_len > best_found) {
						best_found = uwsgi.apps[i].mountpoint_len;
						wsgi_req->script_name = uwsgi.apps[i].mountpoint;
						wsgi_req->script_name_len = uwsgi.apps[i].mountpoint_len;
						wsgi_req->path_info = orig_path_info+wsgi_req->script_name_len;
						wsgi_req->path_info_len = orig_path_info_len-wsgi_req->script_name_len;

						wsgi_req->hvec[script_name].iov_base = wsgi_req->script_name;
						wsgi_req->hvec[script_name].iov_len = wsgi_req->script_name_len;

						wsgi_req->hvec[path_info].iov_base = wsgi_req->path_info;
						wsgi_req->hvec[path_info].iov_len = wsgi_req->path_info_len;
#ifdef UWSGI_DEBUG
						uwsgi_log("managed SCRIPT_NAME = %.*s PATH_INFO = %.*s\n", wsgi_req->script_name_len, wsgi_req->script_name, wsgi_req->path_info_len, wsgi_req->path_info);
#endif
					} 
				}
			}
		}
	}

	if (uwsgi.check_static) {
		struct stat st;
		char *filename = uwsgi_concat2n(uwsgi.check_static, uwsgi.check_static_len, wsgi_req->path_info, wsgi_req->path_info_len);
		uwsgi_log("checking for %s\n", filename);
		if (!stat(filename, &st)) {
			uwsgi_log("file %s found\n", filename);
			wsgi_req->sendfile_fd = open(filename, O_RDONLY);
			wsgi_req->response_size = write(wsgi_req->poll.fd, wsgi_req->protocol, wsgi_req->protocol_len);
			wsgi_req->response_size += write(wsgi_req->poll.fd, " 200 OK\r\n\r\n", 11);
			wsgi_req->response_size += uwsgi_sendfile(wsgi_req);
			free(filename);
			return -1;
		}
		free(filename);
	}

	return 0;
}

int uwsgi_ping_node(int node, struct wsgi_request *wsgi_req) {


	struct pollfd uwsgi_poll;

	struct uwsgi_cluster_node *ucn = &uwsgi.shared->nodes[node];

	if (ucn->name[0] == 0) {
		return 0;
	}

	if (ucn->status == UWSGI_NODE_OK) {
		return 0;
	}

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &ucn->ucn_addr, sizeof(struct sockaddr_in), uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], 0)) {
		close(uwsgi_poll.fd);
		return -1;
	}

	wsgi_req->uh.modifier1 = UWSGI_MODIFIER_PING;
	wsgi_req->uh.pktsize = 0;
	wsgi_req->uh.modifier2 = 0;
	if (write(uwsgi_poll.fd, wsgi_req, 4) != 4) {
		uwsgi_error("write()");
		return -1;
	}

	uwsgi_poll.events = POLLIN;
	if (!uwsgi_parse_response(&uwsgi_poll, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], (struct uwsgi_header *) wsgi_req, wsgi_req->buffer)) {
		return -1;
	}

	return 0;
}

ssize_t uwsgi_send_empty_pkt(int fd, char *socket_name, uint8_t modifier1, uint8_t modifier2) {

	struct uwsgi_header uh;
	char *port;
	uint16_t s_port;
	struct sockaddr_in uaddr;
	int ret;

	uh.modifier1 = modifier1;
	uh.pktsize = 0;
	uh.modifier2 = modifier2;

	if (socket_name) {
		port = strchr(socket_name, ':');
		if (!port) return -1;
		s_port = atoi(port+1);
		port[0] = 0;
		memset(&uaddr, 0, sizeof(struct sockaddr_in));
		uaddr.sin_family = AF_INET;
		uaddr.sin_addr.s_addr = inet_addr(socket_name);
		uaddr.sin_port = htons(s_port);

		port[0] = ':';

		ret = sendto(fd, &uh, 4, 0, (struct sockaddr *) &uaddr, sizeof(struct sockaddr_in));
	}
	else {
		ret = send(fd, &uh, 4, 0);
	}

	if (ret < 0) {
		uwsgi_error("sendto()");
	}

	return ret;
}

int uwsgi_get_dgram(int fd, struct wsgi_request *wsgi_req) {

	ssize_t rlen;
	struct uwsgi_header *uh;
	static char *buffer = NULL;

	struct sockaddr_in sin;
	socklen_t sin_len = sizeof(struct sockaddr_in);

	if (!buffer) {
		buffer = uwsgi_malloc(uwsgi.buffer_size + 4);
	}
		

	rlen = recvfrom(fd, buffer, uwsgi.buffer_size + 4, 0, (struct sockaddr *) &sin, &sin_len);

        if (rlen < 0) {
                uwsgi_error("recvfrom");
                return -1;
        }

	uwsgi_log("recevied request from %s\n", inet_ntoa(sin.sin_addr));

        if (rlen < 4) {
                uwsgi_log("invalid uwsgi packet\n");
                return -1;
        }

	uh = (struct uwsgi_header *) buffer;

	wsgi_req->uh.modifier1 = uh->modifier1;
	/* big endian ? */
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif
	wsgi_req->uh.pktsize = uh->pktsize;
	wsgi_req->uh.modifier2 = uh->modifier2;

	if (wsgi_req->uh.pktsize > uwsgi.buffer_size) {
		uwsgi_log("invalid uwsgi packet size, probably you need to increase buffer size\n");
		return -1;
	}

	wsgi_req->buffer = buffer+4;

	uwsgi_log("request received %d %d\n", wsgi_req->uh.modifier1, wsgi_req->uh.modifier2);

	return 0;

}

int uwsgi_hooked_parse(char *buffer, size_t len, void (*hook)(char *, uint16_t, char *, uint16_t, void*), void *data) {

	char *ptrbuf, *bufferend;
        uint16_t keysize = 0, valsize = 0;
        char *key;

	ptrbuf = buffer;
	bufferend = buffer + len;

	while (ptrbuf < bufferend) {
                if (ptrbuf + 2 >= bufferend) return -1;
                memcpy(&keysize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                keysize = uwsgi_swap16(keysize);
#endif
                /* key cannot be null */
                if (!keysize)  return -1;

                ptrbuf += 2;
                if (ptrbuf + keysize > bufferend) return -1;

                // key
                key = ptrbuf;
                ptrbuf += keysize;
                // value can be null
                if (ptrbuf + 2 > bufferend) return -1;

                memcpy(&valsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                valsize = uwsgi_swap16(valsize);
#endif
                ptrbuf += 2;
                if (ptrbuf + valsize > bufferend) return -1;

                // now call the hook
                hook(key, keysize, ptrbuf, valsize, data);
                ptrbuf += valsize;
        }

        return 0;

}

int uwsgi_hooked_parse_dict_dgram(int fd, char *buffer, size_t len, uint8_t modifier1, uint8_t modifier2, void (*hook)(char *, uint16_t, char *, uint16_t, void *), void *data) {

	struct uwsgi_header *uh;
	ssize_t rlen;

	struct sockaddr_in sin;
	socklen_t sin_len = sizeof(struct sockaddr_in);

	char *ptrbuf, *bufferend;

	rlen = recvfrom(fd, buffer, len, 0, (struct sockaddr *) &sin, &sin_len);

	if (rlen < 0) {
		uwsgi_error("recvfrom()");
		return -1;
	}

	uwsgi_log("recevied request from %s\n", inet_ntoa(sin.sin_addr));

	uwsgi_log("RLEN: %d\n", rlen);

	// check for valid dict 4(header) 2(non-zero key)+1 2(value)
	if (rlen < (4+2+1+2)) {
		uwsgi_log("invalid uwsgi dictionary\n");
		return -1;
	}
	
	uh = (struct uwsgi_header *) buffer;

	if (uh->modifier1 != modifier1 || uh->modifier2 != modifier2) {
		uwsgi_log("invalid uwsgi dictionary received, modifier1: %d modifier2: %d\n", uh->modifier1, uh->modifier2);
		return -1;
	}

        ptrbuf = buffer + 4;

	/* big endian ? */
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif

	if (uh->pktsize > len) {
		uwsgi_log("* WARNING * the uwsgi dictionary received is too big, data will be truncated\n");
		bufferend = ptrbuf + len;
	}
	else {
        	bufferend = ptrbuf + uh->pktsize;
	}

	
	uwsgi_log("%p %p %d\n", ptrbuf, bufferend, bufferend-ptrbuf);

	uwsgi_hooked_parse(ptrbuf, bufferend-ptrbuf, hook, data);

	return 0;

}

int uwsgi_string_sendto(int fd, uint8_t modifier1, uint8_t modifier2, struct sockaddr *sa, socklen_t sa_len, char *message, size_t len) {

	ssize_t rlen ;
	struct uwsgi_header *uh;
	char *upkt = uwsgi_malloc(len + 4);

	uh = (struct uwsgi_header *) upkt;

	uh->modifier1 = modifier1;
	uh->pktsize = len;
	uh->modifier2 = modifier2;

	memcpy(upkt+4, message, len);

	rlen = sendto(fd, upkt, len+4, 0, sa, sa_len);

	if (rlen < 0) {
		uwsgi_error("sendto()");
	}

	free(upkt);

	return rlen;
}

ssize_t fcgi_send_param(int fd, char *key, uint16_t keylen, char *val, uint16_t vallen) {

	struct fcgi_record fr;
	struct iovec iv[5];

	uint8_t ks1 = 0;
	uint32_t ks4 = 0;

	uint8_t vs1 = 0;
	uint32_t vs4 = 0;

	uint16_t size = keylen+vallen;

	if (keylen > 127) {
		size += 4;
		ks4 = htonl(keylen) | 0x80000000;
		iv[1].iov_base = &ks4;
		iv[1].iov_len = 4;
	}
	else {
		size += 1;
		ks1 = keylen;
		iv[1].iov_base = &ks1;
		iv[1].iov_len = 1;
	}

	if (vallen > 127) {
		size += 4;
		vs4 = htonl(vallen) | 0x80000000;
		iv[2].iov_base = &vs4;
		iv[2].iov_len = 4;
	}
	else {
		size += 1;
		vs1 = vallen;
		iv[2].iov_base = &vs1;
		iv[2].iov_len = 1;
	}

	iv[3].iov_base = key;
	iv[3].iov_len = keylen;
	iv[4].iov_base = val;
	iv[4].iov_len = vallen;

	fr.version = 1;
	fr.type = 4;
	fr.req1 = 0;
	fr.req0 = 1;
	fr.cl1 = (uint8_t) ((size >> 8) & 0xff);
	fr.cl0 = (uint8_t) (size &0xff);
	fr.pad = 0;
	fr.reserved = 0;

	iv[0].iov_base = &fr;
	iv[0].iov_len = 8;

	return writev(fd, iv, 5);
	
}

ssize_t fcgi_send_record(int fd, uint8_t type, uint16_t size, char *buffer) {

	struct fcgi_record fr;
	struct iovec iv[2];

	fr.version = 1;
	fr.type = type;
	fr.req1 = 0;
	fr.req0 = 1;
	fr.cl1 = (uint8_t) ((size >> 8) & 0xff);
	fr.cl0 = (uint8_t) (size &0xff);
	fr.pad = 0;
	fr.reserved = 0;

	iv[0].iov_base = &fr;
	iv[0].iov_len = 8;

	iv[1].iov_base = buffer;
	iv[1].iov_len = size;

	return writev(fd, iv, 2);

}

uint16_t fcgi_get_record(int fd, char *buf) {

	struct fcgi_record fr;
	uint16_t remains = 8;
	char *ptr = (char *) &fr;
	ssize_t len;
	uint16_t *rs;

        while(remains) {
        	uwsgi_waitfd(fd, -1);
                len = read(fd, ptr, remains);
                if (len <= 0) return 0;
                remains -= len;
                ptr += len;
        }

        rs = (uint16_t *) &fr.cl1;

        remains = ntohs(*rs) + fr.pad;
        ptr = buf;

        while(remains) {
        	uwsgi_waitfd(fd, -1);
                len = read(fd, ptr, remains);
                if (len <= 0) return 0;
                remains -= len;
                ptr += len;
        }

	return ntohs(*rs);

}
