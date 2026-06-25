#include "Server.hpp"
#include "SignalHandler.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <sstream>
#include <vector>
#include <sys/wait.h>
#include <arpa/inet.h>

Server::Server(const std::vector<ServerConfig>& configs)
    : configs_(configs), listen_count_(0) {
    setupListenSockets();
}

Server::~Server() {
    for (std::map<int, Client>::iterator it = clients_.begin(); it != clients_.end(); ++it)
        if (it->first >= 0)
            close(it->first);
    clients_.clear();

    for (size_t i = 0; i < fds_.size(); ++i)
        if (fds_[i].fd >= 0)
            close(fds_[i].fd);
    fds_.clear();

    listen_configs_.clear();
    sessions_.clear();
}

// Socket creation: one socket per unique host:port, multiple configs per port (virtual hosting).
void Server::setupListenSockets() {
    std::map<std::pair<std::string, int>, size_t> host_port_to_idx;

    for (size_t i = 0; i < configs_.size(); ++i) {
        const ServerConfig& conf = configs_[i];
        std::pair<std::string, int> key(conf.host, conf.port);

        std::map<std::pair<std::string, int>, size_t>::iterator it = host_port_to_idx.find(key);
        if (it != host_port_to_idx.end()) {
            listen_configs_[it->second].push_back(&configs_[i]);
            continue;
        }

        std::ostringstream oss;
        oss << conf.port;
        std::string port_str = oss.str();

        struct addrinfo hints, *res;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        int err = getaddrinfo(conf.host.c_str(), port_str.c_str(), &hints, &res);
        if (err != 0) {
            std::cerr << "Error: " << gai_strerror(err) << std::endl;
            exit(1);
        }

        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) {
            std::cerr << "Error: socket() failed" << std::endl;
            freeaddrinfo(res);
            exit(1);
        }

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
            std::cerr << "Error: bind() on " << conf.host << ":" << conf.port << std::endl;
            close(sock);
            freeaddrinfo(res);
            exit(1);
        }

        if (listen(sock, SOMAXCONN) < 0) {
            std::cerr << "Error: listen() failed" << std::endl;
            close(sock);
            freeaddrinfo(res);
            exit(1);
        }

        fcntl(sock, F_SETFL, O_NONBLOCK);

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        fds_.push_back(pfd);

        size_t idx = fds_.size() - 1;
        host_port_to_idx[key] = idx;
        std::vector<const ServerConfig*> vec;
        vec.push_back(&configs_[i]);
        listen_configs_[idx] = vec;

        std::cout << "Listening on " << conf.host << ":" << conf.port << std::endl;
        freeaddrinfo(res);
    }
    listen_count_ = fds_.size();
}

// Close clients that have been idle for too long.
void Server::checkTimeouts() {
    time_t now = time(NULL);
    for (size_t i = listen_count_; i < fds_.size(); ++i) {
        int fd = fds_[i].fd;
        if (fd < 0)
            continue;
        std::map<int, Client>::iterator it = clients_.find(fd);
        if (it == clients_.end())
            continue;
        if (it->second.reading_done)
            continue;
        if (now - it->second.last_activity > 60) {
            buildErrorResponse(it->second, 408, "Request Timeout");
            it->second.reading_done = true;
            fds_[i].events = POLLOUT;
        }
    }
}

// Remove poll entries whose fd was set to -1 (closed clients).
void Server::cleanupDeadFds() {
    for (size_t i = 0; i < fds_.size(); ) {
        if (fds_[i].fd == -1) {
            fds_.erase(fds_.begin() + i);
            if (i < listen_count_)
                listen_count_--;
        } else {
            ++i;
        }
    }
}

// Main loop: single poll() that monitors read + write readiness.
void Server::run() {
    while (g_running) {
        cleanupDeadFds();
        checkTimeouts();

        int ret = poll(&fds_[0], fds_.size(), 1000);
        if (ret < 0) {
            if (!g_running)
                break;
            continue;
        }
        if (ret == 0)
            continue;

        for (size_t i = 0; i < fds_.size(); ++i) {
            if (fds_[i].revents == 0)
                continue;

            if (i < listen_count_) {
                if (fds_[i].revents & POLLIN)
                    handleAccept(i);
            } else if (fds_[i].revents & POLLIN) {
                handleRead(i);
            } else if (fds_[i].revents & POLLOUT) {
                handleWrite(i);
            } else {
                // POLLHUP, POLLERR, or POLLNVAL -> close client
                closeClient(i);
            }
        }
    }
}

// Accept a new client connection and register it with poll().
void Server::handleAccept(size_t idx) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept(fds_[idx].fd, (struct sockaddr*)&addr, &addrlen);
    if (client_fd < 0)
        return;

    fcntl(client_fd, F_SETFL, O_NONBLOCK);

    Client client;
    client.fd = client_fd;
    client.response_sent = 0;
    client.reading_done = false;
    client.config = listen_configs_[idx][0];
    client.location = NULL;
    client.last_activity = time(NULL);
    client.server_candidates = listen_configs_[idx];

    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str)))
        client.remote_addr = ip_str;
    else
        client.remote_addr = "127.0.0.1";

    clients_[client_fd] = client;

    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    fds_.push_back(pfd);
}

// Read data from a client and feed it to the HTTP request parser.
void Server::handleRead(size_t idx) {
    int fd = fds_[idx].fd;
    Client& client = clients_[fd];

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n <= 0) {
        closeClient(idx);
        return;
    }

    client.last_activity = time(NULL);
    client.request.feed(std::string(buf, static_cast<size_t>(n)));

    if (client.request.isComplete()) {
        client.reading_done = true;
        processRequest(client);
        fds_[idx].events = POLLOUT;
    }
}

// Write buffered response data to a client.
void Server::handleWrite(size_t idx) {
    int fd = fds_[idx].fd;
    Client& client = clients_[fd];

    size_t remaining = client.response.size() - client.response_sent;
    ssize_t n = write(fd, client.response.c_str() + client.response_sent, remaining);

    if (n <= 0) {
        closeClient(idx);
        return;
    }

    client.response_sent += static_cast<size_t>(n);

    if (client.response_sent >= client.response.size())
        closeClient(idx);
}

// Match the request path to the most specific location block.
const LocationConfig* Server::matchLocation(const std::string& path, const ServerConfig* config) const {
    const LocationConfig* best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < config->locations.size(); ++i) {
        const LocationConfig& loc = config->locations[i];
        if (path.compare(0, loc.path.size(), loc.path) == 0) {
            if (loc.path.size() > best_len) {
                best = &loc;
                best_len = loc.path.size();
            }
        }
    }
    return best;
}

// Build an error response using configured error pages or a default HTML fallback.
void Server::buildErrorResponse(Client& client, int code, const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(code);

    std::map<int, std::string>::const_iterator it = client.config->error_pages.find(code);
    if (it != client.config->error_pages.end()) {
        std::string root = client.location ? client.location->root : "./www";
        std::string error_path = root + it->second;
        struct stat st;
        if (stat(error_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            int fd = open(error_path.c_str(), O_RDONLY);
            if (fd >= 0) {
                std::vector<char> buf(static_cast<size_t>(st.st_size));
                ssize_t n = read(fd, &buf[0], static_cast<size_t>(st.st_size));
                close(fd);
                if (n > 0) {
                    resp.setBody(std::string(buf.begin(), buf.end()));
                    sendResponse(client, resp);
                    return;
                }
            }
        }
    }

    resp.setBody(HttpResponse::errorPage(code, msg));
    sendResponse(client, resp);
}

// Route the completed request: match location, check method, produce a response.
void Server::processRequest(Client& client) {
    const std::string& method = client.request.method;
    const std::string& path = client.request.path;

    // 0. Session / cookie handling
    handleSession(client);

    // 0.5 Virtual host routing: choose config based on Host header
    std::string host_header;
    std::map<std::string, std::string>::const_iterator hit = client.request.headers.find("host");
    if (hit != client.request.headers.end()) {
        host_header = hit->second;
        size_t colon = host_header.find(':');
        if (colon != std::string::npos)
            host_header = host_header.substr(0, colon);
    }
    const ServerConfig* chosen = client.server_candidates[0];
    for (size_t i = 0; i < client.server_candidates.size(); ++i) {
        if (client.server_candidates[i]->server_name == host_header) {
            chosen = client.server_candidates[i];
            break;
        }
    }
    client.config = chosen;

    // 0.5 Validate HTTP version
    if (client.request.http_version != "HTTP/1.1") {
        buildErrorResponse(client, 505);
        return;
    }

    // 1. Match location
    client.location = matchLocation(path, client.config);
    if (!client.location) {
        buildErrorResponse(client, 404);
        return;
    }

    // 1.5 Check redirect
    if (client.location->has_redirect) {
        HttpResponse resp;
        resp.setStatus(client.location->redirect_code);
        resp.setHeader("Location", client.location->redirect_url);
        resp.setBody(HttpResponse::errorPage(client.location->redirect_code, "Redirect"));
        sendResponse(client, resp);
        return;
    }

    // 2. Check allowed methods
    if (method != "GET" && method != "POST" && method != "DELETE") {
        buildErrorResponse(client, 501);
        return;
    }

    std::set<std::string>::const_iterator mit = client.location->methods.find(method);
    if (mit == client.location->methods.end()) {
        buildErrorResponse(client, 405);
        return;
    }

    // 3. Check body size against client_max_body_size
    if (client.request.body.size() > client.config->client_max_body_size) {
        buildErrorResponse(client, 413);
        return;
    }

    // 4. Check CGI extension
    bool is_cgi = false;
    if (!client.location->cgi_ext.empty()) {
        size_t dot = client.request.path.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = client.request.path.substr(dot);
            for (size_t i = 0; i < client.location->cgi_ext.size(); ++i) {
                if (client.location->cgi_ext[i] == ext) {
                    is_cgi = true;
                    break;
                }
            }
        }
    }

    // 5. Delegate to handler
    if (is_cgi) {
        handleCGI(client);
    } else if (method == "GET") {
        handleGet(client);
    } else if (method == "POST") {
        handlePost(client);
    } else if (method == "DELETE") {
        handleDelete(client);
    } else {
        buildErrorResponse(client, 501);
    }
}

// Resolve the filesystem path for a request: root + subpath after location prefix.
std::string Server::resolvePath(const Client& client) const {
    const std::string& uri = client.request.path;

    // Reject path traversal attempts
    std::istringstream iss(uri);
    std::string segment;
    while (std::getline(iss, segment, '/')) {
        if (segment == "..")
            return "";
    }

    return client.location->root + uri;
}

// Map file extension to MIME type.
std::string Server::getContentType(const std::string& path) const {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm")  return "text/html";
    if (ext == ".css")                    return "text/css";
    if (ext == ".js")                     return "application/javascript";
    if (ext == ".png")                    return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")  return "image/jpeg";
    if (ext == ".gif")                    return "image/gif";
    if (ext == ".svg")                    return "image/svg+xml";
    if (ext == ".txt")                    return "text/plain";
    if (ext == ".json")                   return "application/json";
    if (ext == ".pdf")                    return "application/pdf";
    if (ext == ".ico")                    return "image/x-icon";
    return "application/octet-stream";
}

// Read a file from disk and serve it.
void Server::serveFile(Client& client, const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        buildErrorResponse(client, 403);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        buildErrorResponse(client, 500);
        return;
    }

    size_t size = static_cast<size_t>(st.st_size);
    std::vector<char> buf(size);
    ssize_t n = 0;
    if (size > 0)
        n = read(fd, &buf[0], size);
    close(fd);

    if (n < 0 || static_cast<size_t>(n) != size) {
        buildErrorResponse(client, 500);
        return;
    }

    HttpResponse resp;
    resp.setStatus(200);
    resp.setContentType(getContentType(path));
    resp.setBody(std::string(buf.begin(), buf.end()));
    sendResponse(client, resp);
}

static std::string escapeHtml(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += s[i];
        }
    }
    return out;
}

// Format a file size in human-readable form.
static std::string formatSize(off_t size) {
    std::ostringstream oss;
    if (size < 1024)
        oss << size << " B";
    else if (size < 1024 * 1024)
        oss << (size / 1024) << " KB";
    else
        oss << (size / (1024 * 1024)) << " MB";
    return oss.str();
}

// Generate an HTML directory listing with delete buttons.
void Server::serveDirectoryListing(Client& client, const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        buildErrorResponse(client, 403);
        return;
    }

    std::ostringstream html;
    html << "<!DOCTYPE html>\n"
         << "<html lang=\"en\">\n"
         << "<head>\n"
         << "    <meta charset=\"UTF-8\">\n"
         << "    <title>Index of " << escapeHtml(client.request.path) << "</title>\n"
         << "    <link rel=\"stylesheet\" href=\"/style.css\">\n"
         << "    <link rel=\"icon\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==\">\n"
         << "    <style>\n"
         << "        .file-list { background: white; border-radius: 12px; padding: 25px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); text-align: left; margin-top: 30px; }\n"
         << "        table { width: 100%; border-collapse: collapse; }\n"
         << "        th, td { padding: 12px; border-bottom: 1px solid #eee; }\n"
         << "        th { text-align: left; color: #7f8c8d; font-weight: 600; }\n"
         << "        a { color: #3498db; text-decoration: none; }\n"
         << "        a:hover { text-decoration: underline; }\n"
         << "        .delete-btn { background: #e74c3c; color: white; border: none; padding: 6px 14px; border-radius: 6px; cursor: pointer; font-size: 0.9em; }\n"
         << "        .delete-btn:hover { background: #c0392b; }\n"
         << "        .back { display: inline-block; margin-top: 20px; color: #3498db; }\n"
         << "    </style>\n"
         << "</head>\n"
         << "<body>\n"
         << "    <div class=\"container\">\n"
         << "        <h1>Index of " << escapeHtml(client.request.path) << "</h1>\n"
         << "        <p class=\"subtitle\">Directory listing</p>\n"
         << "        <div class=\"file-list\">\n"
         << "            <table>\n"
         << "                <tr><th>Name</th><th>Size</th><th>Modified</th><th>Action</th></tr>\n";

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name(entry->d_name);
        if (name == ".")
            continue;

        std::string fullpath = path + "/" + name;
        struct stat st;
        std::string size_str = "-";
        std::string mtime_str = "-";
        if (stat(fullpath.c_str(), &st) == 0) {
            if (S_ISREG(st.st_mode))
                size_str = formatSize(st.st_size);
            else if (S_ISDIR(st.st_mode))
                size_str = "directory";

            char time_buf[64];
            struct tm* tm_info = localtime(&st.st_mtime);
            if (tm_info && strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm_info) > 0)
                mtime_str = time_buf;
        }

        std::string display_name = escapeHtml(name);
        std::string href_name = escapeHtml(name);
        std::string uri = escapeHtml(client.request.path);
        if (uri.empty() || uri[uri.size() - 1] != '/')
            uri += "/";

        html << "                <tr>\n"
             << "                    <td><a href=\"" << href_name << "\">" << display_name << "</a></td>\n"
             << "                    <td>" << size_str << "</td>\n"
             << "                    <td>" << mtime_str << "</td>\n";

        if (S_ISREG(st.st_mode)) {
            html << "                    <td><button class=\"delete-btn\" onclick=\"deleteFile('" << href_name << "')\">Delete</button></td>\n";
        } else {
            html << "                    <td></td>\n";
        }
        html << "                </tr>\n";
    }
    closedir(dir);

    html << "            </table>\n"
         << "        </div>\n"
         << "        <p><a href=\"/\" class=\"back\">&larr; Back to home</a></p>\n"
         << "    </div>\n"
         << "    <script>\n"
         << "        function deleteFile(filename) {\n"
         << "            if (!confirm('Delete ' + filename + '?')) return;\n"
         << "            fetch(filename, { method: 'DELETE' })\n"
         << "                .then(r => {\n"
         << "                    if (r.ok) location.reload();\n"
         << "                    else alert('Delete failed (status ' + r.status + ')');\n"
         << "                })\n"
         << "                .catch(e => alert('Error: ' + e));\n"
         << "        }\n"
         << "    </script>\n"
         << "</body>\n"
         << "</html>\n";

    HttpResponse resp;
    resp.setStatus(200);
    resp.setContentType("text/html");
    resp.setBody(html.str());
    sendResponse(client, resp);
}

// Handle a directory request: try index file, then directory listing.
void Server::serveDirectory(Client& client, const std::string& path) {
    std::string index_path = path + "/" + client.location->index;
    struct stat st;
    if (stat(index_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        serveFile(client, index_path);
        return;
    }
    serveDirectoryListing(client, path);
}

// Handle GET: resolve path, stat, then delegate to file or directory handler.
void Server::handleGet(Client& client) {
    std::string fullpath = resolvePath(client);
    if (fullpath.empty()) {
        buildErrorResponse(client, 403);
        return;
    }

    struct stat st;
    if (stat(fullpath.c_str(), &st) < 0) {
        buildErrorResponse(client, 404);
        return;
    }

    if (S_ISDIR(st.st_mode))
        serveDirectory(client, fullpath);
    else if (S_ISREG(st.st_mode))
        serveFile(client, fullpath);
    else
        buildErrorResponse(client, 403);
}

// Extract boundary value from a multipart/form-data Content-Type header.
static std::string extractBoundary(const std::string& contentType) {
    std::string boundary;
    size_t pos = contentType.find("boundary=");
    if (pos == std::string::npos)
        return boundary;
    pos += std::string("boundary=").size();
    if (pos < contentType.size() && contentType[pos] == '"') {
        ++pos;
        size_t end = contentType.find('"', pos);
        if (end == std::string::npos)
            return boundary;
        boundary = contentType.substr(pos, end - pos);
    } else {
        size_t end = contentType.find(';', pos);
        if (end == std::string::npos)
            boundary = contentType.substr(pos);
        else
            boundary = contentType.substr(pos, end - pos);
    }
    return boundary;
}

// Parse a multipart/form-data body and extract the first uploaded file.
bool Server::parseMultipartFile(const std::string& contentType, const std::string& body,
                                std::string& filename, std::string& fileContent) const {
    std::string boundary = extractBoundary(contentType);
    if (boundary.empty())
        return false;

    std::string marker = "--" + boundary;

    size_t partStart = body.find(marker);
    if (partStart == std::string::npos)
        return false;

    // Skip the boundary line itself
    size_t lineEnd = body.find("\r\n", partStart);
    size_t nlLen = 2;
    if (lineEnd == std::string::npos) {
        lineEnd = body.find('\n', partStart);
        nlLen = 1;
    }
    if (lineEnd == std::string::npos)
        return false;
    lineEnd += nlLen;

    // Find end of part headers
    size_t headerEnd = body.find("\r\n\r\n", lineEnd);
    size_t headerSepLen = 4;
    if (headerEnd == std::string::npos) {
        headerEnd = body.find("\n\n", lineEnd);
        headerSepLen = 2;
    }
    if (headerEnd == std::string::npos)
        return false;

    std::string headers = body.substr(lineEnd, headerEnd - lineEnd);

    // Extract filename from Content-Disposition header
    size_t fnPos = headers.find("filename=\"");
    if (fnPos == std::string::npos) {
        fnPos = headers.find("filename='");
        if (fnPos == std::string::npos)
            return false;
        fnPos += std::string("filename='").size();
    } else {
        fnPos += std::string("filename=\"").size();
    }
    size_t fnEnd = headers.find(headers[fnPos - 1], fnPos);
    if (fnEnd == std::string::npos)
        return false;
    filename = headers.substr(fnPos, fnEnd - fnPos);

    size_t contentStart = headerEnd + headerSepLen;

    // Find next boundary marker to know where the file content ends
    size_t nextMarker = body.find(marker, contentStart);
    if (nextMarker == std::string::npos)
        return false;

    // Strip the CRLF/LF that separates content from the boundary
    size_t contentEnd = nextMarker;
    if (contentEnd >= 2 && body.compare(contentEnd - 2, 2, "\r\n") == 0)
        contentEnd -= 2;
    else if (contentEnd >= 1 && body[contentEnd - 1] == '\n')
        contentEnd -= 1;

    if (contentEnd < contentStart)
        contentEnd = contentStart;

    fileContent = body.substr(contentStart, contentEnd - contentStart);
    return true;
}

// Sanitize an uploaded filename for safe storage.
std::string Server::sanitizeUploadFilename(const std::string& filename) const {
    if (filename.empty())
        return "";

    // Keep only the base name, strip any path components
    size_t pos = filename.find_last_of("/\\");
    std::string base = (pos == std::string::npos) ? filename : filename.substr(pos + 1);

    // Trim whitespace
    size_t start = base.find_first_not_of(" \t\r\n");
    size_t end = base.find_last_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    base = base.substr(start, end - start + 1);

    // Remove leading dots to avoid hidden/special files
    while (!base.empty() && base[0] == '.')
        base.erase(0, 1);

    if (base.empty())
        return "";

    return base;
}

// Handle POST: write body to upload_store or acknowledge.
void Server::handlePost(Client& client) {
    const std::string& upload_store = client.location->upload_store;

    std::string filename;
    std::string fileContent;
    bool isMultipart = false;

    std::map<std::string, std::string>::const_iterator it = client.request.headers.find("content-type");
    if (it != client.request.headers.end() && it->second.find("multipart/form-data") != std::string::npos) {
        isMultipart = parseMultipartFile(it->second, client.request.body, filename, fileContent);
    }

    if (!upload_store.empty()) {
        std::string dataToWrite;
        size_t dataSize = 0;

        if (isMultipart) {
            dataToWrite = fileContent;
            dataSize = fileContent.size();
        } else {
            dataToWrite = client.request.body;
            dataSize = client.request.body.size();
        }

        std::string saveName;
        if (isMultipart && !filename.empty()) {
            std::string safe = sanitizeUploadFilename(filename);
            if (!safe.empty()) {
                std::ostringstream oss;
                oss << time(NULL) << "_" << safe;
                saveName = oss.str();
            }
        }
        if (saveName.empty()) {
            std::ostringstream oss;
            oss << time(NULL) << "_" << dataSize;
            saveName = oss.str();
        }

        std::string filepath = upload_store + "/" + saveName;
        int fd = open(filepath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            buildErrorResponse(client, 500);
            return;
        }

        ssize_t written = 0;
        if (dataSize > 0)
            written = write(fd, dataToWrite.c_str(), dataSize);
        close(fd);

        if (written < 0 || static_cast<size_t>(written) != dataSize) {
            unlink(filepath.c_str());
            buildErrorResponse(client, 500);
            return;
        }

        HttpResponse resp;
        resp.setStatus(201);
        resp.setContentType("text/html");
        std::string body = "<html><body><h1>201 Created</h1><p>File: " + filepath + "</p></body></html>";
        resp.setBody(body);
        sendResponse(client, resp);
    } else {
        HttpResponse resp;
        resp.setStatus(200);
        std::ostringstream body;
        body << "<html><body>"
             << "<h1>POST " << client.request.path << "</h1>"
             << "<p>Body size: " << client.request.body.size() << "</p>"
             << "</body></html>";
        resp.setBody(body.str());
        sendResponse(client, resp);
    }
}

// Handle DELETE: remove file from disk.
void Server::handleDelete(Client& client) {
    std::string fullpath = resolvePath(client);
    if (fullpath.empty()) {
        buildErrorResponse(client, 403);
        return;
    }

    struct stat st;
    if (stat(fullpath.c_str(), &st) < 0) {
        buildErrorResponse(client, 404);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        buildErrorResponse(client, 403);
        return;
    }

    if (unlink(fullpath.c_str()) < 0) {
        buildErrorResponse(client, 403);
        return;
    }

    HttpResponse resp;
    resp.setStatus(204);
    resp.setBody("");
    sendResponse(client, resp);
}

// Handle CGI: fork + execve with pipes and environment.
void Server::handleCGI(Client& client) {
    std::string script_path = resolvePath(client);
    if (script_path.empty()) {
        buildErrorResponse(client, 403);
        return;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        buildErrorResponse(client, 500);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        buildErrorResponse(client, 500);
        return;
    }

    if (pid == 0) {
        // Child process
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);

        // Build environment
        std::vector<std::string> env_strings;
        std::vector<char*> envp;

        env_strings.push_back("REQUEST_METHOD=" + client.request.method);
        env_strings.push_back("QUERY_STRING=" + client.request.query_string);
        {
            std::ostringstream ps; ps << client.config->port;
            env_strings.push_back("SERVER_PORT=" + ps.str());
        }
        {
            std::string sn = client.config->server_name;
            if (sn.empty()) sn = "localhost";
            env_strings.push_back("SERVER_NAME=" + sn);
        }
        env_strings.push_back("SERVER_PROTOCOL=" + client.request.http_version);

        std::ostringstream cl;
        cl << client.request.body.size();
        env_strings.push_back("CONTENT_LENGTH=" + cl.str());

        std::map<std::string, std::string>::const_iterator hit;
        hit = client.request.headers.find("content-type");
        if (hit != client.request.headers.end())
            env_strings.push_back("CONTENT_TYPE=" + hit->second);

        env_strings.push_back("SCRIPT_NAME=" + client.request.path);
        env_strings.push_back("PATH_INFO=");
        env_strings.push_back("PATH_TRANSLATED=" + script_path);
        env_strings.push_back("REMOTE_ADDR=" + client.remote_addr);
        env_strings.push_back("SERVER_SOFTWARE=webserv/1.0");
        env_strings.push_back("GATEWAY_INTERFACE=CGI/1.1");

        hit = client.request.headers.find("host");
        if (hit != client.request.headers.end())
            env_strings.push_back("HTTP_HOST=" + hit->second);
        hit = client.request.headers.find("user-agent");
        if (hit != client.request.headers.end())
            env_strings.push_back("HTTP_USER_AGENT=" + hit->second);
        hit = client.request.headers.find("accept");
        if (hit != client.request.headers.end())
            env_strings.push_back("HTTP_ACCEPT=" + hit->second);

        for (size_t i = 0; i < env_strings.size(); ++i)
            envp.push_back(const_cast<char*>(env_strings[i].c_str()));
        envp.push_back(NULL);

        // Determine interpreter for .py scripts
        char* argv[3];
        argv[0] = const_cast<char*>("/usr/bin/python3");
        argv[1] = const_cast<char*>(script_path.c_str());
        argv[2] = NULL;

        alarm(5);
        execve(argv[0], argv, &envp[0]);
        // If execve fails, try direct execution (for compiled CGI)
        argv[0] = const_cast<char*>(script_path.c_str());
        execve(argv[0], argv, &envp[0]);
        exit(1);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!client.request.body.empty())
        write(stdin_pipe[1], client.request.body.c_str(), client.request.body.size());
    close(stdin_pipe[1]);

    // Read CGI output (blocking read on pipe is OK per subject)
    std::string cgi_output;
    char buf[4096];
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0)
        cgi_output.append(buf, static_cast<size_t>(n));
    close(stdout_pipe[0]);

    int status;
    waitpid(pid, &status, 0);

    // Parse CGI output: headers separated from body by \n\n or \r\n\r\n
    HttpResponse resp;
    resp.setStatus(200);

    // Find header/body boundary
    size_t header_end = std::string::npos;
    size_t sep_len = 0;
    size_t pos_rnrn = cgi_output.find("\r\n\r\n");
    size_t pos_nn  = cgi_output.find("\n\n");
    if (pos_rnrn != std::string::npos) {
        header_end = pos_rnrn;
        sep_len = 4;
    } else if (pos_nn != std::string::npos) {
        header_end = pos_nn;
        sep_len = 2;
    }

    if (header_end != std::string::npos) {
        std::string header_section = cgi_output.substr(0, header_end);
        resp.setBody(cgi_output.substr(header_end + sep_len));

        // Parse CGI response headers
        std::istringstream hss(header_section);
        std::string hline;
        while (std::getline(hss, hline)) {
            if (!hline.empty() && hline[hline.size() - 1] == '\r')
                hline.erase(hline.size() - 1);
            size_t colon = hline.find(':');
            if (colon != std::string::npos) {
                std::string key = hline.substr(0, colon);
                std::string val = hline.substr(colon + 1);
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos)
                    val = val.substr(start);
                if (key == "Content-Type" || key == "content-type")
                    resp.setContentType(val);
                else if (key == "Location" || key == "location") {
                    resp.setStatus(302);
                    resp.setHeader("Location", val);
                } else if (key == "Status" || key == "status") {
                    int sc = 200;
                    std::istringstream(val) >> sc;
                    resp.setStatus(sc);
                }
            }
        }
    } else {
        // No headers — entire output is body
        resp.setBody(cgi_output);
    }

    sendResponse(client, resp);
}

// Close a client connection and mark its poll entry as dead.
void Server::closeClient(size_t idx) {
    int fd = fds_[idx].fd;
    if (fd >= 0) {
        close(fd);
        clients_.erase(fd);
        fds_[idx].fd = -1;
        fds_[idx].events = 0;
    }
}

std::string Server::generateSessionId() const {
    std::ostringstream oss;
    oss << time(NULL) << "_" << rand();
    return oss.str();
}

void Server::handleSession(Client& client) {
    std::map<std::string, std::string>::const_iterator it = client.request.cookies.find("session_id");
    std::string sid;
    if (it != client.request.cookies.end()) {
        sid = it->second;
    } else {
        sid = generateSessionId();
        client.session_cookie = "session_id=" + sid + "; Path=/; HttpOnly";
    }
    sessions_[sid].count++;
}

void Server::sendResponse(Client& client, HttpResponse& resp) {
    if (!client.session_cookie.empty())
        resp.setCookie(client.session_cookie);
    client.response = resp.toString();
}
