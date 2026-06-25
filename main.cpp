#include "ConfigParser.hpp"
#include "Server.hpp"
#include "SignalHandler.hpp"
#include <iostream>

int main(int argc, char** argv)
{
    ConfigParser parser;
    std::vector<ServerConfig> configs;

    if (argc >= 2)
        configs = parser.parse(argv[1]);
    else
        configs = parser.parseFromDefault();

    setupSignalHandlers();

    Server server(configs);
    server.run();
    return 0;
}
