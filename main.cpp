#include "ConfigParser.hpp"
#include "ServerConfig.hpp"

#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    ConfigParser parser;
    std::vector<ServerConfig> configs;

    if (argc >= 2)
        configs = parser.parse(argv[1]);
    else
        configs = parser.parseFromDefault();

    for (size_t i = 0; i < configs.size(); ++i)
    {
        const ServerConfig& s = configs[i];
        std::cout << "Server " << (i + 1) << ":" << std::endl;
        std::cout << "  listen:       " << s.host << ":" << s.port << std::endl;
        std::cout << "  server_name:  " << (s.server_name.empty() ? "(none)" : s.server_name) << std::endl;
        std::cout << "  max_body:     " << s.client_max_body_size << " bytes" << std::endl;

        for (std::map<int, std::string>::const_iterator it = s.error_pages.begin();
             it != s.error_pages.end(); ++it)
        {
            std::cout << "  error_page:   " << it->first << " -> " << it->second << std::endl;
        }

        for (size_t l = 0; l < s.locations.size(); ++l)
        {
            const LocationConfig& loc = s.locations[l];
            std::cout << "  Location " << loc.path << ":" << std::endl;
            std::cout << "    root:       " << loc.root << std::endl;
            std::cout << "    index:      " << loc.index << std::endl;
            std::cout << "    autoindex:  " << (loc.autoindex ? "on" : "off") << std::endl;

            std::cout << "    methods:   ";
            for (std::set<std::string>::const_iterator it = loc.methods.begin();
                 it != loc.methods.end(); ++it)
            {
                std::cout << " " << *it;
            }
            std::cout << std::endl;

            if (!loc.upload_store.empty())
                std::cout << "    upload:     " << loc.upload_store << std::endl;

            if (!loc.cgi_ext.empty())
            {
                std::cout << "    cgi_ext:   ";
                for (size_t e = 0; e < loc.cgi_ext.size(); ++e)
                    std::cout << " " << loc.cgi_ext[e];
                std::cout << std::endl;
            }

            if (loc.has_redirect)
                std::cout << "    redirect:   " << loc.redirect_code
                          << " -> " << loc.redirect_url << std::endl;
        }
    }

    return 0;
}
