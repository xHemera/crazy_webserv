#include "ServerConfig.hpp"

LocationConfig::LocationConfig()
    : path("/")
    , root("./www")
    , index("index.html")
    , autoindex(false)
    , has_redirect(false)
    , redirect_code(0)
{
    methods.insert("GET");
    methods.insert("POST");
    methods.insert("DELETE");
}

ServerConfig::ServerConfig()
    : host("127.0.0.1")
    , port(8080)
    , client_max_body_size(1048576)
{
}
