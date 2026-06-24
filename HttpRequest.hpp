#ifndef HTTPREQUEST_HPP
#define HTTPREQUEST_HPP

#include <string>
#include <map>

class HttpRequest {
public:
    HttpRequest();

    bool feed(const std::string& data);
    bool isComplete() const;
    void clear();

    std::string method;
    std::string path;
    std::string query_string;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> cookies;
    std::string body;

private:
    void parseRequestLine(const std::string& line);
    void parseHeaderLine(const std::string& line);
    void parseCookies(const std::string& value);

    enum State { REQUEST_LINE, HEADERS, BODY, COMPLETE };
    enum ChunkState { CHUNK_SIZE, CHUNK_DATA };
    State state_;
    ChunkState chunk_state_;
    std::string buf_;
    size_t content_length_;
    size_t chunk_remaining_;
    bool chunked_;
};

#endif
