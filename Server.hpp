#ifndef SERVER_HPP
#define SERVER_HPP

#include "ServerConfig.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include <vector>
#include <string>
#include <map>
#include <poll.h>
#include <ctime>

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
        time_t last_activity;
        std::vector<const ServerConfig*> server_candidates;
        std::string remote_addr;
        std::string session_cookie;
    };

    void setupListenSockets();
    void cleanupDeadFds();
    void checkTimeouts();
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
    void sendResponse(Client& client, HttpResponse& resp);

    struct Session {
        int count;
        Session() : count(0) {}
    };

    std::string generateSessionId() const;
    void handleSession(Client& client);

    std::vector<ServerConfig> configs_;
    std::vector<struct pollfd> fds_;
    std::map<int, Client> clients_;
    size_t listen_count_;
    std::map<size_t, std::vector<const ServerConfig*> > listen_configs_;
    std::map<std::string, Session> sessions_;
};

#endif
