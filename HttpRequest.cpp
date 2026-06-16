#include "HttpRequest.hpp"
#include <cstdlib>
#include <cctype>
#include <sstream>

HttpRequest::HttpRequest()
    : state_(REQUEST_LINE), content_length_(0), chunked_(false) {}

void HttpRequest::clear() {
    state_ = REQUEST_LINE;
    buf_.clear();
    method.clear();
    path.clear();
    query_string.clear();
    http_version.clear();
    headers.clear();
    body.clear();
    content_length_ = 0;
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
                // ponytail: chunked decoding deferred
                state_ = COMPLETE;
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

    if (key == "content-length")
        content_length_ = std::atol(value.c_str());
    else if (key == "transfer-encoding" && value.find("chunked") != std::string::npos)
        chunked_ = true;
}
