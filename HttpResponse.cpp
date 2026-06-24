#include "HttpResponse.hpp"
#include <sstream>
#include <cstdlib>

HttpResponse::HttpResponse() : status_code(200) {}

void HttpResponse::clear() {
    status_code = 200;
    body.clear();
    headers.clear();
}

void HttpResponse::setStatus(int code) {
    status_code = code;
}

void HttpResponse::setHeader(const std::string& key, const std::string& value) {
    headers[key] = value;
}

void HttpResponse::setBody(const std::string& b) {
    body = b;
}

void HttpResponse::setContentType(const std::string& type) {
    headers["Content-Type"] = type;
}

void HttpResponse::setCookie(const std::string& c) {
    headers["Set-Cookie"] = c;
}

std::string HttpResponse::toString() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << reasonPhrase(status_code) << "\r\n";

    std::string ct;
    std::map<std::string, std::string>::const_iterator it = headers.find("Content-Type");
    if (it != headers.end())
        ct = it->second;
    else
        ct = "text/html";
    oss << "Content-Type: " << ct << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";

    bool has_connection = false;
    for (it = headers.begin(); it != headers.end(); ++it) {
        if (it->first != "Content-Type" && it->first != "Content-Length") {
            oss << it->first << ": " << it->second << "\r\n";
        }
        if (it->first == "Connection")
            has_connection = true;
    }

    if (!has_connection)
        oss << "Connection: close\r\n";

    oss << "\r\n";
    oss << body;

    return oss.str();
}

std::string HttpResponse::reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string HttpResponse::errorPage(int code, const std::string& custom_msg) {
    std::string msg = custom_msg.empty() ? reasonPhrase(code) : custom_msg;

    std::ostringstream html;
    html << "<html>\n"
         << "<head><title>" << code << " " << msg << "</title></head>\n"
         << "<body>\n"
         << "<h1>" << code << " " << msg << "</h1>\n"
         << "</body>\n"
         << "</html>";

    return html.str();
}
