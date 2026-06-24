#include "Server.hpp"
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
    for (size_t i = 0; i < fds_.size(); ++i)
        if (fds_[i].fd >= 0)
            close(fds_[i].fd);
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
    while (true) {
        cleanupDeadFds();
        checkTimeouts();

        int ret = poll(&fds_[0], fds_.size(), -1);
        if (ret < 0) {
            std::cerr << "Error: poll() failed" << std::endl;
            break;
        }

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
    std::string subpath;
    if (client.location->path == "/") {
        subpath = client.request.path;
    } else {
        subpath = client.request.path.substr(client.location->path.size());
        if (subpath.empty() || subpath[0] != '/')
            subpath = client.request.path;
    }
    if (subpath.empty())
        subpath = "/";

    // Reject path traversal attempts
    std::istringstream iss(subpath);
    std::string segment;
    while (std::getline(iss, segment, '/')) {
        if (segment == "..")
            return "";
    }

    return client.location->root + subpath;
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

// Generate an HTML directory listing.
void Server::serveDirectoryListing(Client& client, const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        buildErrorResponse(client, 403);
        return;
    }

    std::ostringstream html;
    html << "<html><head><title>Index of " << client.request.path
         << "</title></head><body>"
         << "<h1>Index of " << client.request.path << "</h1><hr><ul>";

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name(entry->d_name);
        if (name == ".")
            continue;
        html << "<li><a href=\"" << name << "\">" << name << "</a></li>";
    }
    html << "</ul><hr></body></html>";
    closedir(dir);

    HttpResponse resp;
    resp.setStatus(200);
    resp.setContentType("text/html");
    resp.setBody(html.str());
    sendResponse(client, resp);
}

// Handle a directory request: try index file, then autoindex or 403.
void Server::serveDirectory(Client& client, const std::string& path) {
    std::string index_path = path + "/" + client.location->index;
    struct stat st;
    if (stat(index_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        serveFile(client, index_path);
        return;
    }
    if (client.location->autoindex)
        serveDirectoryListing(client, path);
    else
        buildErrorResponse(client, 403);
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

// Handle POST: write body to upload_store or acknowledge.
void Server::handlePost(Client& client) {
    const std::string& upload_store = client.location->upload_store;

    // ponytail: without upload_store, just acknowledge the body
    if (!upload_store.empty()) {
        std::ostringstream filename;
        filename << time(NULL) << "_" << client.request.body.size();

        std::string filepath = upload_store + "/" + filename.str();
        int fd = open(filepath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            buildErrorResponse(client, 500);
            return;
        }

        ssize_t written = write(fd, client.request.body.c_str(), client.request.body.size());
        close(fd);

        if (written < 0 || static_cast<size_t>(written) != client.request.body.size()) {
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
