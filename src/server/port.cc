/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "server.h"
#include "http.h"
#include "http2.h"
#include "websocket.h"
#include "mqtt.h"
#include "swoole_redis.h"

using swoole::Server;
using swoole::ListenPort;
using swoole::http::Request;

static int swPort_onRead_raw(swReactor *reactor, ListenPort *lp, swEvent *event);
static int swPort_onRead_check_length(swReactor *reactor, ListenPort *lp, swEvent *event);
static int swPort_onRead_check_eof(swReactor *reactor, ListenPort *lp, swEvent *event);
static int swPort_onRead_http(swReactor *reactor, ListenPort *lp, swEvent *event);
static int swPort_onRead_redis(swReactor *reactor, ListenPort *lp, swEvent *event);

ListenPort::ListenPort() {
    protocol.package_length_type = 'N';
    protocol.package_length_size = 4;
    protocol.package_body_offset = 4;
    protocol.package_max_length = SW_INPUT_BUFFER_SIZE;

    char eof[] = SW_DATA_EOF;
    protocol.package_eof_len = sizeof(SW_DATA_EOF) - 1;
    memcpy(protocol.package_eof, eof, protocol.package_eof_len);
}

#ifdef SW_USE_OPENSSL
int ListenPort::enable_ssl_encrypt() {
    if (ssl_option.cert_file == nullptr || ssl_option.key_file == nullptr) {
        swWarn("SSL error, require ssl_cert_file and ssl_key_file");
        return SW_ERR;
    }
    ssl_context = swSSL_get_context(&ssl_option);
    if (ssl_context == nullptr) {
        swWarn("swSSL_get_context() error");
        return SW_ERR;
    }
    if (ssl_option.client_cert_file &&
        swSSL_set_client_certificate(ssl_context, ssl_option.client_cert_file, ssl_option.verify_depth) ==
            SW_ERR) {
        swWarn("swSSL_set_client_certificate() error");
        return SW_ERR;
    }
    if (open_http_protocol) {
        ssl_config.http = 1;
    }
    if (open_http2_protocol) {
        ssl_config.http_v2 = 1;
        swSSL_server_http_advise(ssl_context, &ssl_config);
    }
    if (swSSL_server_set_cipher(ssl_context, &ssl_config) < 0) {
        swWarn("swSSL_server_set_cipher() error");
        return SW_ERR;
    }
    return SW_OK;
}
#endif

int ListenPort::listen() {
    int sock = socket->fd;
    int option = 1;

    // listen stream socket
    if (::listen(sock, backlog) < 0) {
        swSysWarn("listen(%s:%d, %d) failed", host, port, backlog);
        return SW_ERR;
    }

#ifdef TCP_DEFER_ACCEPT
    if (tcp_defer_accept) {
        if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, (const void *) &tcp_defer_accept, sizeof(int)) != 0) {
            swSysWarn("setsockopt(TCP_DEFER_ACCEPT) failed");
        }
    }
#endif

#ifdef TCP_FASTOPEN
    if (tcp_fastopen) {
        if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN, (const void *) &tcp_fastopen, sizeof(int)) != 0) {
            swSysWarn("setsockopt(TCP_FASTOPEN) failed");
        }
    }
#endif

#ifdef SO_KEEPALIVE
    if (open_tcp_keepalive == 1) {
        if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *) &option, sizeof(option)) != 0) {
            swSysWarn("setsockopt(SO_KEEPALIVE) failed");
        }
#ifdef TCP_KEEPIDLE
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (void *) &tcp_keepidle, sizeof(int)) < 0) {
            swSysWarn("setsockopt(TCP_KEEPIDLE) failed");
        }
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *) &tcp_keepinterval, sizeof(int)) < 0) {
            swSysWarn("setsockopt(TCP_KEEPINTVL) failed");
        }
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (void *) &tcp_keepcount, sizeof(int)) < 0) {
            swSysWarn("setsockopt(TCP_KEEPCNT) failed");
        }
#endif
#ifdef TCP_USER_TIMEOUT
        if (tcp_user_timeout > 0 &&
            setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, (void *) &tcp_user_timeout, sizeof(int)) != 0) {
            swSysWarn("setsockopt(TCP_USER_TIMEOUT) failed");
        }
#endif
    }
#endif

    buffer_high_watermark = socket_buffer_size * 0.8;
    buffer_low_watermark = 0;

    return SW_OK;
}

void Server::init_port_protocol(ListenPort *ls) {
    ls->protocol.private_data_2 = this;
    // Thread mode must copy the data.
    // will free after onFinish
    if (ls->open_eof_check) {
        if (ls->protocol.package_eof_len > SW_DATA_EOF_MAXLEN) {
            ls->protocol.package_eof_len = SW_DATA_EOF_MAXLEN;
        }
        ls->protocol.onPackage = Server::dispatch_task;
        ls->onRead = swPort_onRead_check_eof;
    } else if (ls->open_length_check) {
        if (ls->protocol.package_length_type != '\0') {
            ls->protocol.get_package_length = Protocol::default_length_func;
        }
        ls->protocol.onPackage = Server::dispatch_task;
        ls->onRead = swPort_onRead_check_length;
    } else if (ls->open_http_protocol) {
#ifdef SW_USE_HTTP2
        if (ls->open_http2_protocol && ls->open_websocket_protocol) {
            ls->protocol.get_package_length = swHttpMix_get_package_length;
            ls->protocol.get_package_length_size = swHttpMix_get_package_length_size;
            ls->protocol.onPackage = swHttpMix_dispatch_frame;
        } else if (ls->open_http2_protocol) {
            ls->protocol.package_length_size = SW_HTTP2_FRAME_HEADER_SIZE;
            ls->protocol.get_package_length = swHttp2_get_frame_length;
            ls->protocol.onPackage = Server::dispatch_task;
        } else
#endif
            if (ls->open_websocket_protocol) {
            ls->protocol.package_length_size = SW_WEBSOCKET_HEADER_LEN + SW_WEBSOCKET_MASK_LEN + sizeof(uint64_t);
            ls->protocol.get_package_length = swWebSocket_get_package_length;
            ls->protocol.onPackage = swWebSocket_dispatch_frame;
        }
        ls->protocol.package_length_offset = 0;
        ls->protocol.package_body_offset = 0;
        ls->onRead = swPort_onRead_http;
    } else if (ls->open_mqtt_protocol) {
        swMqtt_set_protocol(&ls->protocol);
        ls->protocol.onPackage = Server::dispatch_task;
        ls->onRead = swPort_onRead_check_length;
    } else if (ls->open_redis_protocol) {
        ls->protocol.onPackage = Server::dispatch_task;
        ls->onRead = swPort_onRead_redis;
    } else {
        ls->onRead = swPort_onRead_raw;
    }
}

/**
 * @description: set the ListenPort.host and ListenPort.port in ListenPort from sock
 */
int ListenPort::set_address(int sock) {
    socklen_t optlen;
    swSocketAddress address;
    int sock_type, sock_family;
    char tmp[INET6_ADDRSTRLEN];

    // get socket type
    optlen = sizeof(sock_type);
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE, &sock_type, &optlen) < 0) {
        swWarn("getsockopt(%d, SOL_SOCKET, SO_TYPE) failed", sock);
        return -1;
    }
    // get socket family
#ifndef SO_DOMAIN
    swWarn("no getsockopt(SO_DOMAIN) supports");
    return -1;
#else
    optlen = sizeof(sock_family);
    if (getsockopt(sock, SOL_SOCKET, SO_DOMAIN, &sock_family, &optlen) < 0) {
        swWarn("getsockopt(%d, SOL_SOCKET, SO_DOMAIN) failed", sock);
        return -1;
    }
#endif

    // get address info
    address.len = sizeof(address.addr);
    if (getsockname(sock, (struct sockaddr *) &address.addr, &address.len) < 0) {
        swWarn("getsockname(%d) failed", sock);
        return -1;
    }

    switch (sock_family) {
    case AF_INET:
        if (sock_type == SOCK_STREAM) {
            type = SW_SOCK_TCP;
        } else {
            type = SW_SOCK_UDP;
        }
        port = ntohs(address.addr.inet_v4.sin_port);
        strncpy(host, inet_ntoa(address.addr.inet_v4.sin_addr), SW_HOST_MAXSIZE - 1);
        break;
    case AF_INET6:
        if (sock_type == SOCK_STREAM) {
            type = SW_SOCK_TCP6;
        } else {
            type = SW_SOCK_UDP6;
        }
        port = ntohs(address.addr.inet_v6.sin6_port);
        inet_ntop(AF_INET6, &address.addr.inet_v6.sin6_addr, tmp, sizeof(tmp));
        strncpy(host, tmp, SW_HOST_MAXSIZE - 1);
        break;
    case AF_UNIX:
        type = sock_type == SOCK_STREAM ? SW_SOCK_UNIX_STREAM : SW_SOCK_UNIX_DGRAM;
        port = 0;
        strncpy(host, address.addr.un.sun_path, SW_HOST_MAXSIZE);
        break;
    default:
        swWarn("Unknown socket family[%d]", sock_family);
        break;
    }

    return 0;
}

void ListenPort::clear_protocol() {
    open_eof_check = 0;
    open_length_check = 0;
    open_http_protocol = 0;
    open_websocket_protocol = 0;
#ifdef SW_USE_HTTP2
    open_http2_protocol = 0;
#endif
    open_mqtt_protocol = 0;
    open_redis_protocol = 0;
}

static int swPort_onRead_raw(swReactor *reactor, ListenPort *port, swEvent *event) {
    ssize_t n;
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swServer *serv = (swServer *) reactor->ptr;

    swString *buffer = serv->get_recv_buffer(_socket);
    if (!buffer) {
        return SW_ERR;
    }

    n = _socket->recv(buffer->str, buffer->size, 0);
    if (n < 0) {
        switch (_socket->catch_error(errno)) {
        case SW_ERROR:
            swSysWarn("recv from connection#%d failed", event->fd);
            return SW_OK;
        case SW_CLOSE:
            conn->close_errno = errno;
            goto _close_fd;
        default:
            return SW_OK;
        }
    } else if (n == 0) {
    _close_fd:
        reactor->trigger_close_event(event);
        return SW_OK;
    } else {
        buffer->offset = buffer->length = n;
        return Server::dispatch_task(&port->protocol, _socket, buffer->str, n);
    }
}

static int swPort_onRead_check_length(swReactor *reactor, ListenPort *port, swEvent *event) {
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;
    swServer *serv = (swServer *) reactor->ptr;

    swString *buffer = serv->get_recv_buffer(_socket);
    if (!buffer) {
        reactor->trigger_close_event(event);
        return SW_ERR;
    }

    if (protocol->recv_with_length_protocol(_socket, buffer) < 0) {
        swTrace("Close Event.FD=%d|From=%d", event->fd, event->reactor_id);
        conn->close_errno = errno;
        reactor->trigger_close_event(event);
    }

    return SW_OK;
}

#define CLIENT_INFO_FMT " from session#%u on %s:%d"
#define CLIENT_INFO_ARGS conn->session_id, port->host, port->port

/**
 * For Http Protocol
 */
static int swPort_onRead_http(swReactor *reactor, ListenPort *port, swEvent *event) {
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swServer *serv = (swServer *) reactor->ptr;

    if (conn->websocket_status >= WEBSOCKET_STATUS_HANDSHAKE) {
        if (conn->http_upgrade == 0) {
            serv->destroy_http_request(conn);
            conn->websocket_status = WEBSOCKET_STATUS_ACTIVE;
            conn->http_upgrade = 1;
        }
        return swPort_onRead_check_length(reactor, port, event);
    }

#ifdef SW_USE_HTTP2
    if (conn->http2_stream) {
        return swPort_onRead_check_length(reactor, port, event);
    }
#endif

    Request *request = nullptr;
    swProtocol *protocol = &port->protocol;

    if (conn->object == nullptr) {
        request = new Request();
        conn->object = request;
    } else {
        request = reinterpret_cast<Request *>(conn->object);
    }

    if (!request->buffer_) {
        request->buffer_ = serv->get_recv_buffer(_socket);
        if (!request->buffer_) {
            reactor->trigger_close_event(event);
            return SW_ERR;
        }
    }

    swString *buffer = request->buffer_;

_recv_data:
    ssize_t n = _socket->recv(buffer->str + buffer->length, buffer->size - buffer->length, 0);
    if (n < 0) {
        switch (_socket->catch_error(errno)) {
        case SW_ERROR:
            swSysWarn("recv from connection#%d failed", event->fd);
            return SW_OK;
        case SW_CLOSE:
            conn->close_errno = errno;
            goto _close_fd;
        default:
            return SW_OK;
        }
    }

    if (n == 0) {
        if (0) {
        _bad_request:
#ifdef SW_HTTP_BAD_REQUEST_PACKET
            _socket->send(SW_STRL(SW_HTTP_BAD_REQUEST_PACKET), 0);
#endif
        }
        if (0) {
        _too_large:
#ifdef SW_HTTP_REQUEST_ENTITY_TOO_LARGE_PACKET
            _socket->send(SW_STRL(SW_HTTP_REQUEST_ENTITY_TOO_LARGE_PACKET), 0);
#endif
        }
        if (0) {
        _unavailable:
#ifdef SW_HTTP_SERVICE_UNAVAILABLE_PACKET
            _socket->send(SW_STRL(SW_HTTP_SERVICE_UNAVAILABLE_PACKET), 0);
#endif
        }
    _close_fd:
        serv->destroy_http_request(conn);
        reactor->trigger_close_event(event);
        return SW_OK;
    }

    buffer->length += n;

_parse:
    if (request->method == 0 && request->get_protocol() < 0) {
        if (!request->excepted && buffer->length < SW_HTTP_HEADER_MAX_SIZE) {
            return SW_OK;
        }
        swoole_error_log(SW_LOG_TRACE,
                         SW_ERROR_HTTP_INVALID_PROTOCOL,
                         "Bad Request: unknown protocol" CLIENT_INFO_FMT,
                         CLIENT_INFO_ARGS);
        goto _bad_request;
    }

    if (request->method > SW_HTTP_PRI) {
        swoole_error_log(SW_LOG_TRACE,
                         SW_ERROR_HTTP_INVALID_PROTOCOL,
                         "Bad Request: got unsupported HTTP method" CLIENT_INFO_FMT,
                         CLIENT_INFO_ARGS);
        goto _bad_request;
    } else if (request->method == SW_HTTP_PRI) {
#ifdef SW_USE_HTTP2
        if (sw_unlikely(!port->open_http2_protocol)) {
#endif
            swoole_error_log(SW_LOG_TRACE,
                             SW_ERROR_HTTP_INVALID_PROTOCOL,
                             "Bad Request: can not handle HTTP2 request" CLIENT_INFO_FMT,
                             CLIENT_INFO_ARGS);
            goto _bad_request;
#ifdef SW_USE_HTTP2
        }
        conn->http2_stream = 1;
        swHttp2_send_setting_frame(protocol, _socket);
        if (buffer->length == sizeof(SW_HTTP2_PRI_STRING) - 1) {
            serv->destroy_http_request(conn);
            swString_clear(buffer);
            return SW_OK;
        }
        buffer->reduce(buffer->offset);
        serv->destroy_http_request(conn);
        conn->socket->skip_recv = 1;
        return swPort_onRead_check_length(reactor, port, event);
#endif
    }

    // http header is not the end
    if (request->header_length_ == 0) {
        if (request->get_header_length() < 0) {
            if (buffer->size == buffer->length) {
                swoole_error_log(SW_LOG_TRACE,
                                 SW_ERROR_HTTP_INVALID_PROTOCOL,
                                 "Bad Request: request header is too long" CLIENT_INFO_FMT,
                                 CLIENT_INFO_ARGS);
                goto _bad_request;
            }
            goto _recv_data;
        }
    }

    // parse http header and got http body length
    if (!request->header_parsed) {
        request->parse_header_info();
        swTraceLog(SW_TRACE_SERVER,
                   "content-length=%u, keep-alive=%u, chunked=%u",
                   request->content_length_,
                   request->keep_alive,
                   request->chunked);
    }

    // content length (equal to 0) or (field not found but not chunked)
    if (!request->tried_to_dispatch) {
        // recv nobody_chunked eof
        if (request->nobody_chunked) {
            if (buffer->length < request->header_length_ + (sizeof("0\r\n\r\n") - 1)) {
                goto _recv_data;
            }
            request->header_length_ += (sizeof("0\r\n\r\n") - 1);
        }
        request->tried_to_dispatch = 1;
        // (know content-length is equal to 0) or (no content-length field and no chunked)
        if (request->content_length_ == 0 && (request->known_length || !request->chunked)) {
            buffer->offset = request->header_length_;
            // send static file content directly in the reactor thread
            if (!serv->enable_static_handler || !serv->select_static_handler(request, conn)) {
                // dynamic request, dispatch to worker
                Server::dispatch_task(protocol, _socket, buffer->str, request->header_length_);
            }
            if (!conn->active || _socket->removed) {
                return SW_OK;
            }
            if (buffer->length > request->header_length_) {
                // http pipeline, multi requests, parse the next one
                buffer->reduce(request->header_length_);
                request->clean();
                goto _parse;
            } else {
                serv->destroy_http_request(conn);
                swString_clear(buffer);
                return SW_OK;
            }
        }
    }

    size_t request_length;
    if (request->chunked) {
        /* unknown length, should find chunked eof */
        if (request->get_chunked_body_length() < 0) {
            if (request->excepted) {
                swoole_error_log(SW_LOG_TRACE,
                                 SW_ERROR_HTTP_INVALID_PROTOCOL,
                                 "Bad Request: protocol error when parse chunked length" CLIENT_INFO_FMT,
                                 CLIENT_INFO_ARGS);
                goto _bad_request;
            }
            request_length = request->header_length_ + request->content_length_;
            if (request_length > protocol->package_max_length) {
                swoole_error_log(SW_LOG_TRACE,
                                 SW_ERROR_HTTP_INVALID_PROTOCOL,
                                 "Request Entity Too Large: request length (chunked) has already been greater than the "
                                 "package_max_length(%u)" CLIENT_INFO_FMT,
                                 protocol->package_max_length,
                                 CLIENT_INFO_ARGS);
                goto _too_large;
            }
            if (buffer->length == buffer->size && swString_extend(buffer, buffer->size * 2) < 0) {
                goto _unavailable;
            }
            if (request_length > buffer->size && swString_extend_align(buffer, request_length) < 0) {
                goto _unavailable;
            }
            goto _recv_data;
        } else {
            request_length = request->header_length_ + request->content_length_;
        }
        swTraceLog(SW_TRACE_SERVER, "received chunked eof, real content-length=%u", request->content_length_);
    } else {
        request_length = request->header_length_ + request->content_length_;
        if (request_length > protocol->package_max_length) {
            swoole_error_log(SW_LOG_TRACE,
                             SW_ERROR_HTTP_INVALID_PROTOCOL,
                             "Request Entity Too Large: header-length (%u) + content-length (%u) is greater than the "
                             "package_max_length(%u)" CLIENT_INFO_FMT,
                             request->header_length_,
                             request->content_length_,
                             protocol->package_max_length,
                             CLIENT_INFO_ARGS);
            goto _too_large;
        }

        if (request_length > buffer->size && swString_extend(buffer, request_length) < 0) {
            goto _unavailable;
        }

        if (buffer->length < request_length) {
#ifdef SW_HTTP_100_CONTINUE
            // Expect: 100-continue
            if (request->has_expect_header()) {
                _socket->send(SW_STRL(SW_HTTP_100_CONTINUE_PACKET), 0);
            } else {
                swTraceLog(SW_TRACE_SERVER,
                           "PostWait: request->content_length=%d, buffer->length=%zu, request->header_length=%d\n",
                           request->content_length,
                           buffer_->length,
                           request->header_length);
            }
#endif
            goto _recv_data;
        }
    }

    // discard the redundant data
    if (buffer->length > request_length) {
        swoole_error_log(SW_LOG_TRACE,
                         SW_ERROR_HTTP_INVALID_PROTOCOL,
                         "Invalid Request: %zu bytes has been disacard" CLIENT_INFO_FMT,
                         buffer->length - request_length,
                         CLIENT_INFO_ARGS);
        buffer->length = request_length;
    }

    buffer->offset = request_length;
    Server::dispatch_task(protocol, _socket, buffer->str, buffer->length);
    if (conn->active && !_socket->removed) {
        serv->destroy_http_request(conn);
        swString_clear(buffer);
    }

    return SW_OK;
}

static int swPort_onRead_redis(swReactor *reactor, ListenPort *port, swEvent *event) {
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;
    swServer *serv = (swServer *) reactor->ptr;

    swString *buffer = serv->get_recv_buffer(_socket);
    if (!buffer) {
        reactor->trigger_close_event(event);
        return SW_ERR;
    }

    if (swServer_recv_redis_packet(protocol, conn, buffer) < 0) {
        conn->close_errno = errno;
        reactor->trigger_close_event(event);
    }

    return SW_OK;
}

static int swPort_onRead_check_eof(swReactor *reactor, ListenPort *port, swEvent *event) {
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;
    swServer *serv = (swServer *) reactor->ptr;

    swString *buffer = serv->get_recv_buffer(_socket);
    if (!buffer) {
        reactor->trigger_close_event(event);
        return SW_ERR;
    }

    if (protocol->recv_with_eof_protocol(_socket, buffer) < 0) {
        conn->close_errno = errno;
        reactor->trigger_close_event(event);
    }

    return SW_OK;
}

void ListenPort::close() {
#ifdef SW_USE_OPENSSL
    if (ssl) {
        if (ssl_context) {
            swSSL_free_context(ssl_context);
        }
        sw_free(ssl_option.cert_file);
        sw_free(ssl_option.key_file);
        if (ssl_option.client_cert_file) {
            sw_free(ssl_option.client_cert_file);
        }
#ifdef SW_SUPPORT_DTLS
        if (dtls_sessions) {
            delete dtls_sessions;
        }
#endif
    }
#endif

    if (socket) {
        socket->free();
        socket = nullptr;
    }

    // remove unix socket file
    if (type == SW_SOCK_UNIX_STREAM || type == SW_SOCK_UNIX_DGRAM) {
        unlink(host);
    }
}
