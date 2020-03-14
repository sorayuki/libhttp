#include <stdio.h>
#include <stdlib.h>
#include <list>
#include <uv.h>
#include "buffer-pool.h"
#include "common.h"
#include "content-writer.h"
#include "file-reader.h"
#include "parser.h"
#include "reference-count.h"
#include "server.h"
#include "utils.h"

#ifdef _MSC_VER
#define ssize_t intptr_t
#undef min
#endif

#define _ENABLE_KEEP_ALIVE_

namespace http
{

static const size_t _max_request_body_ = 8 * 1024 * 1024;

static int _responser_count_ = 0;

static const char* status_message(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 414: return "Request-URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 503: return "Service Unavailable";

    default:
    case 500: return "Internal Server Error";
    }
}

class _responser : public parser, public content_writer
{
    define_reference_count(_responser)

    friend class server;

    enum _end_reason
    {
        reason_start_failed,
        reason_read_done,
        reason_write_done,
        reason_write_done2
    };

private:
    const std::unordered_map<std::string, router>& router_map_;
    const std::vector<std::pair<std::regex, router>> router_list_;

    // for input
    request2 request_;
    std::string peer_address_;

    // for output
    response2 response_;
    router router_ = {};

    bool keep_alive_ = false;

protected:
    _responser(uv_loop_t* loop, uv_stream_t* socket, std::shared_ptr<buffer_pool> buffer_pool,
            const std::unordered_map<std::string, router>& router_map, const std::vector<std::pair<std::regex, router>>& router_list) :
        parser(true, buffer_pool), router_map_(router_map), router_list_(router_list),
        content_writer(loop)
    {
        socket_ = socket;
        uv_handle_set_data((uv_handle_t*)socket, this);

        sockaddr_in addr = {};
        int len = sizeof(addr);
        int r = uv_tcp_getpeername((uv_tcp_t*)socket, (sockaddr*)&addr, &len);
        if (r == 0)
        {
            char name[256] = {};
            r = uv_inet_ntop(addr.sin_family, &addr.sin_addr, name, sizeof(name));
            peer_address_ = name;
        }

        _responser_count_++;
    }

    virtual ~_responser()
    {
        printf("%d alive responsers\n", --_responser_count_);
    }

    void start()
    {
        int r = start_read(socket_);
        if (r != 0)
            on_end(r, reason_start_failed);
    }

    virtual request_base* on_get_request()
    {
        return &request_;
    }

    virtual response* on_get_response()
    {
        assert(!"should not be called!");
        return &response_;
    }

    virtual bool on_headers_parsed(std::optional<int64_t> content_length)
    {
        auto end = request_.headers.cend();
        auto p = end;

        p = request_.headers.find(HEADER_CONNECTION);
        keep_alive_ = p != end && case_equals(p->second, "Keep-Alive");

        request_.range_begin.reset();
        request_.range_end.reset();
        p = request_.headers.find(HEADER_RANGE);
        if (p != end)
            parse_range(p->second, request_.range_begin, request_.range_end);

        if (auto p = router_map_.find(request_.url); p != router_map_.cend())
        {
            router_ = p->second;
        }
        else for (auto& p : router_list_)
        {
            if (std::regex_match(request_.url, p.first))
            {
                router_ = p.second;
                break;
            }
        }

        printf("%p:%p begin: %s\n", this, socket_, request_.url.c_str());
        return !router_.on_start || router_.on_start(request_);
    }

    virtual bool on_content_received(const char* data, size_t size)
    {
        return !router_.on_data || router_.on_data(data, size);
    }

    virtual void on_read_end(int error_code)
    {
        if (error_code < 0 && socket_ != nullptr)
            uv_read_stop(socket_);

        if (state_ != state_outputing)
        {
            if (error_code >= 0)
                on_route();
            else
                on_end(error_code, reason_read_done);
        }
    }

    void on_route()
    {
        // set default status
        response_.status_msg.clear();
        response_.headers.clear();
        response_.content_length.reset();
        response_.releaser = nullptr;

        if (router_.on_router)
        {
            request_.headers[HEADER_REMOTE_ADDRESS] = peer_address_;
            response_.status_code = 200;
            router_.on_router(request_, response_);
            if (response_.is_ok())
            {
#ifdef _ENABLE_KEEP_ALIVE_
                if (keep_alive_ && !response_.headers.count(HEADER_CONNECTION))
                    response_.headers[HEADER_CONNECTION] = "Keep-Alive";
#endif
            }
            else
                response_.content_length = 0;
        }
        else
        {
            response_.content_length = 0;
            response_.status_code = 404;
        }
        if (response_.status_msg.empty())
            response_.status_msg = status_message(response_.status_code);

        start_write();
    }

    void start_write()
    {
        string_map& headers = response_.headers;

        if (case_equals(request_.method, "HEAD"))
        {
            content_written_ = content_to_write_ = 0; // to be done
            response_.content_length = 0;
        }
        else if (response_.content_length)
        {
            int64_t length = response_.content_length.value();
            int64_t length_1 = length - 1;
            if (request_.has_range())
            {
                int64_t range_end = std::min(request_.range_end.value_or(length_1), length_1);
                headers[HEADER_CONTENT_RANGE] = std::string("bytes ") + std::to_string(request_.range_begin.value()) + '-' + std::to_string(range_end) + '/' + std::to_string(length);
                response_.status_code = 206;
            }

            int64_t read_end = request_.range_end.value_or(length_1) + 1;
            content_written_ = request_.range_begin.value_or(0);
            content_to_write_ = std::min(read_end, length);
            response_.content_length = content_to_write_ - content_written_;
        }
        else
        {
            content_written_ = 0;
            content_to_write_ = INT64_MAX;
        }

        if (response_.content_length)
        {
            if (!headers.count(HEADER_CONTENT_LENGTH))
                headers[HEADER_CONTENT_LENGTH] = std::to_string(response_.content_length.value());
            if (!headers.count(HEADER_ACCEPT_RANGES))
                headers[HEADER_ACCEPT_RANGES] = "bytes";
        }

        std::string* pstr = new std::string();
        pstr->reserve(4096);

        pstr->append("HTTP/1.1 ", 9);
        pstr->append(std::to_string(response_.status_code));
        pstr->append(" ", 1);
        pstr->append(!response_.status_msg.empty() ? response_.status_msg : "done");
        pstr->append(" \r\n", 3);
        for (auto& p : headers)
        {
            pstr->append(p.first);
            pstr->append(": ", 2);
            pstr->append(p.second);
            pstr->append("\r\n", 2);
        }
        pstr->append("\r\n", 2);

        auto holder = std::make_shared<_content_holder>(pstr->c_str(), pstr->size(), [=]() { delete pstr; });

        state_ = state_outputing;
        content_writer::start_write(holder, response_.provider);
    }

    virtual void on_write_end(int error_code)
    {
        on_end(error_code, reason_write_done);
    }

    void on_end(int error_code, _end_reason reason)
    {
        if (response_.releaser)
        {
            response_.releaser();
            response_.releaser = nullptr;
        }

        if (error_code != 0)
            keep_alive_ = false;
#ifdef _ENABLE_KEEP_ALIVE_
        if (keep_alive_)
        {
            printf("%p:%p alive%d: %s, %s, %d\n", this, socket_, reason, error_code == 0 ? "DONE" : uv_err_name(error_code), request_.url.c_str(), ref_count_);

            uv_read_stop(socket_);            
            start_read(socket_);
            return;
        }
#endif
        printf("%p:%p end%d: %s, %s, %d\n", this, socket_, reason, error_code == 0 ? "DONE" : uv_err_name(error_code), request_.url.c_str(), ref_count_);

        state_ = state_parsing;
        assert(ref_count_ == 1);
        release();
    }
};

server::server(uv_loop_t* loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
    loop_ = loop != nullptr ? loop : uv_default_loop();
    socket_ = new uv_stream_t{};
    uv_tcp_init(loop_, (uv_tcp_t*)socket_);
}

server::~server()
{
    uv_close((uv_handle_t*)socket_, [](uv_handle_t* handle) {
        delete handle;
    });
}

bool server::listen(const std::string& address, int port)
{
    sockaddr_in addr;
    uv_ip4_addr(address.c_str(), port, &addr);

    uv_tcp_bind((uv_tcp_t*)socket_, (const sockaddr*)&addr, 0);
    uv_handle_set_data((uv_handle_t*)socket_, this);
    int r = uv_listen(socket_, 128, on_connection_cb);
    return r == 0;
}

void server::serve(const std::string& pattern, on_router on_router)
{
    router router = {};
    router.on_router = on_router;
    serve(pattern, router);
}

void server::serve(const std::string& pattern, router router)
{
    auto p = router_map_.insert_or_assign(pattern, router);
    if (p.second)
        router_list_.push_back(std::make_pair(std::regex(pattern), router));
}

bool server::serve_file(const std::string& path, const request2& req, response2& res)
{
    uv_fs_t fs_req{};
    int r = uv_fs_stat(loop_, &fs_req, path.c_str(), nullptr);
    uint64_t length = fs_req.statbuf.st_size;
    uv_fs_req_cleanup(&fs_req);
    if (r != 0 || fs_req.result != 0 || !(fs_req.statbuf.st_mode & S_IFREG) || length == 0)
    {
        res.status_code = 404;
        return false;
    }

    auto reader = std::make_shared<file_reader>(loop_, path, buffer_pool_);
    if (reader->get_fd() < 0)
    {
        res.status_code = 403;
        return false;
    }

    res.content_length = length;
    res.provider = [=](int64_t offset, int64_t length, content_sink sink) {
        size_t size = std::min(buffer_pool::buffer_size, (size_t)(length - offset));
        int r = reader->request_chunk(offset, size, sink);
        if (r < 0 && r != UV_EAGAIN)
            sink(nullptr, r, nullptr);
    };
    return true;
}

int server::run_loop()
{
    return uv_run(loop_, UV_RUN_DEFAULT);
}

void server::stop_loop()
{
    uv_stop(loop_);
}

void server::on_connection(uv_stream_t* socket)
{
    uv_tcp_t* tcp = new uv_tcp_t{};
    uv_tcp_init(loop_, tcp);
    if (uv_accept(socket, (uv_stream_t*)tcp) == 0)
    {
        uv_tcp_keepalive(tcp, 1, 30); // in seconds
        (new _responser(loop_, (uv_stream_t*)tcp, buffer_pool_, router_map_, router_list_))->start();
    }
    else
    {
        uv_close((uv_handle_t*)tcp, parser::on_closed_and_delete_cb);
    }
}

void server::on_connection_cb(uv_stream_t* socket, int status)
{
    server* p_this = (server*)uv_handle_get_data((uv_handle_t*)socket);
    if (status == 0)
        p_this->on_connection(socket);
    else
        printf("accept error: %d\n", status);
}

} // namespace http