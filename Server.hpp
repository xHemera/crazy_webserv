#ifndef SERVER_HPP
#define SERVER_HPP

#include "ServerConfig.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include <vector>
#include <string>
#include <map>
#include <poll.h>

class Server {
public:
    Server(const std::vector<ServerConfig>& configs);
    ~Server();
    void run();

private:
    struct Client {
        int fd;
        HttpRequest request;
        std::string response;
        size_t response_sent;
        bool reading_done;
        const ServerConfig* config;
        const LocationConfig* location;
    };

    void setupListenSockets();
    void cleanupDeadFds();
    void handleAccept(size_t idx);
    void handleRead(size_t idx);
    void handleWrite(size_t idx);
    void closeClient(size_t idx);

    const LocationConfig* matchLocation(const std::string& path, const ServerConfig* config) const;
    void processRequest(Client& client);
    void buildErrorResponse(Client& client, int code, const std::string& msg = "");

    std::string resolvePath(const Client& client) const;
    void handleGet(Client& client);
    void handlePost(Client& client);
    void handleDelete(Client& client);
    void handleCGI(Client& client);
    void serveFile(Client& client, const std::string& path);
    void serveDirectory(Client& client, const std::string& path);
    void serveDirectoryListing(Client& client, const std::string& path);
    std::string getContentType(const std::string& path) const;

    std::vector<ServerConfig> configs_;
    std::vector<struct pollfd> fds_;
    std::map<int, Client> clients_;
    size_t listen_count_;
};

#endif
