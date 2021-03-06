#ifndef __CW_HTTP_RESPONSE_H_
#define __CW_HTTP_RESPONSE_H_

#include <functional>
#include <map>
#include "Utils.h"
#include "HttpStatusCodes_C++.h"

namespace cW {

class HttpResponse {
    friend class HttpSession;

    typedef std::function<void(void)>   AbortHandler;
    typedef std::function<void(size_t)> WriteHandler;

    WriteHandler onWritableCallback = nullptr;
    AbortHandler onAbortCallback    = nullptr;

    HttpStatus::Code statusCode = HttpStatus::OK;

    std::string_view buffer;
    // send buffer should persist after send call
    std::string sendBuffer;

    bool wroteContentLength = false;
    bool close              = false;

    size_t contentLength = __INF__;

    std::multimap<std::string_view, std::string> headers;

    HttpResponse();

  public:
    template <typename T>
        requires std::is_convertible_v<T, std::string> || requires(T a)
    {
        std::to_string(a);
    }
    HttpResponse* setHeader(const std::string_view& name, const T& value);
    HttpResponse* setStatus(HttpStatus::Code statusCode);
    void          write(const char* buf, size_t size, size_t contentSize = __INF__);
    void          write(const std::string_view& data, size_t contentSize = __INF__);
    void          send(const std::string& data);
    void          end();
    HttpResponse* onAborted(AbortHandler&& handler);
    HttpResponse* onWritable(WriteHandler&& handler);
    inline bool   headerSet(const std::string_view& name);
};

bool HttpResponse::headerSet(const std::string_view& name)
{
    return headers.find(name) != headers.end();
}

template <typename T>
    requires std::is_convertible_v<T, std::string> || requires(T a)
{
    std::to_string(a);
}
HttpResponse* HttpResponse::setHeader(const std::string_view& name, const T& value)
{
    // std::string key = to_lower(name);
    if (ci_match<true>(name, "content-length")) {
        wroteContentLength = true;
        contentLength      = (size_t)value;
    }
    if constexpr (std::is_convertible_v<T, std::string>)
        headers.insert({name, value});
    else
        headers.insert({name, std::to_string(value)});
    return this;
}
}; // namespace cW

#endif