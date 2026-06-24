#ifndef HTTPRESPONSE_HPP
#define HTTPRESPONSE_HPP

#include <string>
#include <map>

class HttpResponse {
public:
    HttpResponse();

    void setStatus(int code);
    void setHeader(const std::string& key, const std::string& value);
    void setBody(const std::string& body);
    void setContentType(const std::string& type);
    void setCookie(const std::string& cookie);

    std::string toString() const;
    void clear();

    static std::string reasonPhrase(int code);
    static std::string errorPage(int code, const std::string& custom_msg = "");

    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
};

#endif
