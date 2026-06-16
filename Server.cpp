#include "Server.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sstream>

Server::Server(const std::vector<ServerConfig>& configs)
    : configs_(configs), listen_count_(0) {
    setupListenSockets();
}

Server::~Server() {
    for (size_t i = 0; i < fds_.size(); ++i)
        if (fds_[i].fd >= 0)
            close(fds_[i].fd);
}

// Socket creation for each listen directive in the config.
void Server::setupListenSockets() {
    for (size_t i = 0; i < configs_.size(); ++i) {
        const ServerConfig& conf = configs_[i];

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

        std::cout << "Listening on " << conf.host << ":" << conf.port << std::endl;
        freeaddrinfo(res);
    }
    listen_count_ = fds_.size();
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
    client.config = &configs_[idx];
    client.location = NULL;
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
        // ponytail: file-based error pages deferred — serve hardcoded for now
        resp.setBody(HttpResponse::errorPage(code, msg));
    } else {
        resp.setBody(HttpResponse::errorPage(code, msg));
    }

    client.response = resp.toString();
}

// Route the completed request: match location, check method, produce a response.
void Server::processRequest(Client& client) {
    const std::string& method = client.request.method;
    const std::string& path = client.request.path;

    // 1. Match location
    client.location = matchLocation(path, client.config);
    if (!client.location) {
        buildErrorResponse(client, 404);
        return;
    }

    // 2. Check allowed methods
    // ponytail: only GET/POST/DELETE are handled; everything else is 501
    if (method != "GET" && method != "POST" && method != "DELETE") {
        buildErrorResponse(client, 501);
        return;
    }

    std::set<std::string>::const_iterator mit = client.location->methods.find(method);
    if (mit == client.location->methods.end()) {
        buildErrorResponse(client, 405);
        return;
    }

    // 3. Delegate to handler based on method
    if (method == "GET") {
        HttpResponse resp;
        resp.setStatus(200);
        std::ostringstream body;
        body << "<html><body>"
             << "<h1>" << client.request.method << " " << client.request.path << "</h1>"
             << "<p>Location: " << client.location->path << "</p>"
             << "<p>Root: " << client.location->root << "</p>"
             << "</body></html>";
        resp.setBody(body.str());
        client.response = resp.toString();
    } else if (method == "POST") {
        HttpResponse resp;
        resp.setStatus(200);
        std::ostringstream body;
        body << "<html><body>"
             << "<h1>POST " << path << "</h1>"
             << "<p>Location: " << client.location->path << "</p>"
             << "<p>Body size: " << client.request.body.size() << "</p>"
             << "</body></html>";
        resp.setBody(body.str());
        client.response = resp.toString();
    } else if (method == "DELETE") {
        HttpResponse resp;
        resp.setStatus(204);
        resp.setBody("");
        client.response = resp.toString();
    } else {
        buildErrorResponse(client, 501);
    }
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
