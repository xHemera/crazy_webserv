#ifndef SERVERCONFIG_HPP
#define SERVERCONFIG_HPP

#include <string>
#include <vector>
#include <map>
#include <set>

struct LocationConfig {
    std::string path;
    std::string root;
    std::string index;
    std::set<std::string> methods;
    bool autoindex;
    std::string upload_store;
    std::vector<std::string> cgi_ext;
    bool has_redirect;
    int redirect_code;
    std::string redirect_url;

    LocationConfig();
};

struct ServerConfig {
    std::string host;
    int port;
    std::string server_name;
    size_t client_max_body_size;
    std::map<int, std::string> error_pages;
    std::vector<LocationConfig> locations;

    ServerConfig();
};

#endif
