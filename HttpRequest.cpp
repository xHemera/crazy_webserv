#include "HttpRequest.hpp"
#include <cstdlib>
#include <cctype>
#include <sstream>

HttpRequest::HttpRequest()
    : state_(REQUEST_LINE), chunk_state_(CHUNK_SIZE), content_length_(0),
      chunk_remaining_(0), chunked_(false) {}

void HttpRequest::clear() {
    state_ = REQUEST_LINE;
    chunk_state_ = CHUNK_SIZE;
    buf_.clear();
    method.clear();
    path.clear();
    query_string.clear();
    http_version.clear();
    headers.clear();
    body.clear();
    content_length_ = 0;
    chunk_remaining_ = 0;
    chunked_ = false;
}

bool HttpRequest::isComplete() const {
    return state_ == COMPLETE;
}

bool HttpRequest::feed(const std::string& data) {
    buf_ += data;

    while (state_ != COMPLETE) {
        if (state_ == REQUEST_LINE || state_ == HEADERS) {
            size_t pos = buf_.find("\r\n");
            if (pos == std::string::npos)
                break;

            std::string line = buf_.substr(0, pos);
            buf_.erase(0, pos + 2);

            if (state_ == REQUEST_LINE) {
                parseRequestLine(line);
                state_ = HEADERS;
            } else if (line.empty()) {
                state_ = BODY;
            } else {
                parseHeaderLine(line);
            }
        }

        if (state_ == BODY) {
            if (chunked_) {
                while (state_ == BODY) {
                    if (chunk_state_ == CHUNK_SIZE) {
                        size_t pos = buf_.find("\r\n");
                        if (pos == std::string::npos)
                            break;

                        std::string size_line = buf_.substr(0, pos);
                        buf_.erase(0, pos + 2);

                        char* end = NULL;
                        long raw = std::strtol(size_line.c_str(), &end, 16);
                        if (raw < 0)
                            { state_ = COMPLETE; break; }
                        chunk_remaining_ = static_cast<size_t>(raw);

                        if (chunk_remaining_ == 0) {
                            // last chunk: skip optional trailer until \r\n
                            size_t tpos = buf_.find("\r\n");
                            if (tpos == std::string::npos)
                                break;
                            buf_.erase(0, tpos + 2);
                            state_ = COMPLETE;
                            break;
                        }
                        chunk_state_ = CHUNK_DATA;
                    }

                    if (chunk_state_ == CHUNK_DATA) {
                        if (buf_.size() < chunk_remaining_ + 2)
                            break;
                        body += buf_.substr(0, chunk_remaining_);
                        buf_.erase(0, chunk_remaining_ + 2);
                        chunk_state_ = CHUNK_SIZE;
                    }
                }
            } else if (content_length_ > 0) {
                if (buf_.size() < content_length_)
                    break;
                body = buf_.substr(0, content_length_);
                buf_.erase(0, content_length_);
                state_ = COMPLETE;
            } else {
                state_ = COMPLETE;
            }
        }
    }

    return state_ == COMPLETE;
}

void HttpRequest::parseRequestLine(const std::string& line) {
    std::istringstream iss(line);
    iss >> method >> path >> http_version;

    size_t qpos = path.find('?');
    if (qpos != std::string::npos) {
        query_string = path.substr(qpos + 1);
        path = path.substr(0, qpos);
    }
}

void HttpRequest::parseHeaderLine(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return;

    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);

    size_t start = value.find_first_not_of(" \t");
    if (start != std::string::npos)
        value = value.substr(start);

    for (size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));

    headers[key] = value;

    if (key == "cookie")
        parseCookies(value);

    if (key == "content-length")
        content_length_ = std::atol(value.c_str());
    else if (key == "transfer-encoding" && value.find("chunked") != std::string::npos)
        chunked_ = true;
}

void HttpRequest::parseCookies(const std::string& value) {
    std::istringstream iss(value);
    std::string pair;
    while (std::getline(iss, pair, ';')) {
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string ckey = pair.substr(0, eq);
            std::string cval = pair.substr(eq + 1);
            size_t start = ckey.find_first_not_of(" \t");
            if (start != std::string::npos)
                ckey = ckey.substr(start);
            start = cval.find_first_not_of(" \t");
            if (start != std::string::npos)
                cval = cval.substr(start);
            cookies[ckey] = cval;
        }
    }
}
