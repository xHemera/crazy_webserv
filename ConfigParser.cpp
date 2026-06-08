#include "ConfigParser.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

ConfigParser::ConfigParser()
    : pos_(0)
    , token_pos_(0)
{
}


ConfigParser::~ConfigParser()
{
}

std::vector<ServerConfig> ConfigParser::parse(const std::string& filename)
{
    loadFile(filename);
    tokenize();
    std::vector<ServerConfig> configs = parseConfig();
    checkDuplicatePorts(configs);
    return configs;
}

std::vector<ServerConfig> ConfigParser::parseFromDefault()
{
    return parse("conf/default.conf");
}

void ConfigParser::loadFile(const std::string& filename)
{
    std::ifstream file(filename.c_str());
    if (!file.is_open())
    {
        std::cerr << "Error: Cannot open config file: " << filename << std::endl;
        exit(1);
    }
    std::stringstream ss;
    ss << file.rdbuf();
    content_ = ss.str();
    file.close();
}

void ConfigParser::skipWhitespaceAndComments()
{
    while (pos_ < content_.size())
    {
        char c = content_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            pos_++;
        }
        else if (c == '#')
        {
            while (pos_ < content_.size() && content_[pos_] != '\n')
            {
                pos_++;
            }
        }
        else
        {
            break;
        }
    }
}

ConfigParser::Token ConfigParser::nextToken()
{
    if (token_pos_ >= tokens_.size())
    {
        Token t;
        t.type = TOKEN_EOF;
        return t;
    }
    return tokens_[token_pos_++];
}

ConfigParser::Token ConfigParser::peekToken()
{
    if (token_pos_ >= tokens_.size())
    {
        Token t;
        t.type = TOKEN_EOF;
        return t;
    }
    return tokens_[token_pos_];
}

void ConfigParser::tokenize()
{
    pos_ = 0;
    tokens_.clear();

    while (pos_ < content_.size())
    {
        skipWhitespaceAndComments();
        if (pos_ >= content_.size())
            break;

        char c = content_[pos_];
        if (c == '{')
        {
            Token t;
            t.type = TOKEN_LEFT_BRACE;
            t.value = "{";
            tokens_.push_back(t);
            pos_++;
        }
        else if (c == '}')
        {
            Token t;
            t.type = TOKEN_RIGHT_BRACE;
            t.value = "}";
            tokens_.push_back(t);
            pos_++;
        }
        else if (c == ';')
        {
            Token t;
            t.type = TOKEN_SEMICOLON;
            t.value = ";";
            tokens_.push_back(t);
            pos_++;
        }
        else
        {
            std::string word;
            while (pos_ < content_.size()
                   && content_[pos_] != ' ' && content_[pos_] != '\t'
                   && content_[pos_] != '\n' && content_[pos_] != '\r'
                   && content_[pos_] != '{' && content_[pos_] != '}'
                   && content_[pos_] != ';' && content_[pos_] != '#')
            {
                word += content_[pos_];
                pos_++;
            }
            if (!word.empty())
            {
                Token t;
                t.type = TOKEN_WORD;
                t.value = word;
                tokens_.push_back(t);
            }
        }
    }
}

std::vector<ServerConfig> ConfigParser::parseConfig()
{
    std::vector<ServerConfig> configs;
    while (peekToken().type != TOKEN_EOF)
    {
        Token t = nextToken();
        if (t.type == TOKEN_WORD && t.value == "server")
        {
            configs.push_back(parseServer());
        }
        else
        {
            std::cerr << "Error: Expected 'server' at top level" << std::endl;
            exit(1);
        }
    }
    return configs;
}

ServerConfig ConfigParser::parseServer()
{
    ServerConfig server;

    Token t = nextToken();
    if (t.type != TOKEN_LEFT_BRACE)
    {
        std::cerr << "Error: Expected '{' after 'server'" << std::endl;
        exit(1);
    }

    while (peekToken().type != TOKEN_RIGHT_BRACE && peekToken().type != TOKEN_EOF)
    {
        Token word = nextToken();
        if (word.type != TOKEN_WORD)
        {
            std::cerr << "Error: Expected directive name in server block" << std::endl;
            exit(1);
        }

        if (word.value == "location")
        {
            LocationConfig loc = parseLocation();
            server.locations.push_back(loc);
        }
        else
        {
            std::vector<std::string> args;
            args.push_back(word.value);
            while (peekToken().type == TOKEN_WORD)
            {
                args.push_back(nextToken().value);
            }
            t = nextToken();
            if (t.type != TOKEN_SEMICOLON)
            {
                std::cerr << "Error: Expected ';' after directive" << std::endl;
                exit(1);
            }

            if (args[0] == "listen")
                parseListen(server, args);
            else if (args[0] == "server_name")
                parseServerName(server, args);
            else if (args[0] == "client_max_body_size")
                parseClientMaxBodySize(server, args);
            else if (args[0] == "error_page")
                parseErrorPage(server, args);
            else
            {
                std::cerr << "Error: Unknown directive '" << args[0] << "'" << std::endl;
                exit(1);
            }
        }
    }

    t = nextToken();
    if (t.type != TOKEN_RIGHT_BRACE)
    {
        std::cerr << "Error: Expected '}' to close server block" << std::endl;
        exit(1);
    }

    return server;
}

LocationConfig ConfigParser::parseLocation()
{
    LocationConfig loc;

    Token pathToken = nextToken();
    if (pathToken.type != TOKEN_WORD)
    {
        std::cerr << "Error: Expected path after 'location'" << std::endl;
        exit(1);
    }
    loc.path = pathToken.value;

    Token t = nextToken();
    if (t.type != TOKEN_LEFT_BRACE)
    {
        std::cerr << "Error: Expected '{' after location path" << std::endl;
        exit(1);
    }

    while (peekToken().type != TOKEN_RIGHT_BRACE && peekToken().type != TOKEN_EOF)
    {
        Token word = nextToken();
        if (word.type != TOKEN_WORD)
        {
            std::cerr << "Error: Expected directive in location block" << std::endl;
            exit(1);
        }

        std::vector<std::string> args;
        args.push_back(word.value);
        while (peekToken().type == TOKEN_WORD)
        {
            args.push_back(nextToken().value);
        }
        t = nextToken();
        if (t.type != TOKEN_SEMICOLON)
        {
            std::cerr << "Error: Expected ';' after location directive" << std::endl;
            exit(1);
        }

        if (args[0] == "root")
        {
            if (args.size() != 2)
            {
                std::cerr << "Error: root needs exactly 1 argument" << std::endl;
                exit(1);
            }
            loc.root = args[1];
        }
        else if (args[0] == "index")
        {
            if (args.size() != 2)
            {
                std::cerr << "Error: index needs exactly 1 argument" << std::endl;
                exit(1);
            }
            loc.index = args[1];
        }
        else if (args[0] == "methods")
        {
            loc.methods.clear();
            for (size_t i = 1; i < args.size(); ++i)
                loc.methods.insert(args[i]);
        }
        else if (args[0] == "autoindex")
        {
            if (args.size() != 2)
            {
                std::cerr << "Error: autoindex needs exactly 1 argument" << std::endl;
                exit(1);
            }
            if (args[1] == "on")
                loc.autoindex = true;
            else if (args[1] == "off")
                loc.autoindex = false;
            else
            {
                std::cerr << "Error: autoindex must be 'on' or 'off'" << std::endl;
                exit(1);
            }
        }
        else if (args[0] == "upload_store")
        {
            if (args.size() != 2)
            {
                std::cerr << "Error: upload_store needs exactly 1 argument" << std::endl;
                exit(1);
            }
            loc.upload_store = args[1];
        }
        else if (args[0] == "cgi_ext")
        {
            for (size_t i = 1; i < args.size(); ++i)
                loc.cgi_ext.push_back(args[i]);
        }
        else if (args[0] == "return")
        {
            if (args.size() < 3)
            {
                std::cerr << "Error: return needs code and URL" << std::endl;
                exit(1);
            }
            loc.has_redirect = true;
            loc.redirect_code = std::atoi(args[1].c_str());
            loc.redirect_url = args[2];
        }
        else
        {
            std::cerr << "Error: Unknown location directive '" << args[0] << "'" << std::endl;
            exit(1);
        }
    }

    t = nextToken();
    if (t.type != TOKEN_RIGHT_BRACE)
    {
        std::cerr << "Error: Expected '}' to close location block" << std::endl;
        exit(1);
    }

    return loc;
}

void ConfigParser::parseListen(ServerConfig& server, const std::vector<std::string>& args)
{
    if (args.size() != 2)
    {
        std::cerr << "Error: listen needs exactly 1 argument (host:port or port)" << std::endl;
        exit(1);
    }

    std::string arg = args[1];
    size_t colon = arg.find(':');
    if (colon != std::string::npos)
    {
        server.host = arg.substr(0, colon);
        server.port = std::atoi(arg.substr(colon + 1).c_str());
    }
    else
    {
        server.port = std::atoi(arg.c_str());
    }

    if (server.port <= 0 || server.port > 65535)
    {
        std::cerr << "Error: Invalid port number: " << server.port << std::endl;
        exit(1);
    }
}

void ConfigParser::parseServerName(ServerConfig& server, const std::vector<std::string>& args)
{
    if (args.size() != 2)
    {
        std::cerr << "Error: server_name needs exactly 1 argument" << std::endl;
        exit(1);
    }
    server.server_name = args[1];
}

void ConfigParser::parseClientMaxBodySize(ServerConfig& server, const std::vector<std::string>& args)
{
    if (args.size() != 2)
    {
        std::cerr << "Error: client_max_body_size needs exactly 1 argument" << std::endl;
        exit(1);
    }
    server.client_max_body_size = parseSize(args[1]);
}

void ConfigParser::parseErrorPage(ServerConfig& server, const std::vector<std::string>& args)
{
    if (args.size() != 3)
    {
        std::cerr << "Error: error_page needs code and path" << std::endl;
        exit(1);
    }
    int code = std::atoi(args[1].c_str());
    server.error_pages[code] = args[2];
}

size_t ConfigParser::parseSize(const std::string& str)
{
    std::string num;
    char suffix = 0;
    for (size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] >= '0' && str[i] <= '9')
            num += str[i];
        else
        {
            suffix = str[i];
            break;
        }
    }
    size_t value = static_cast<size_t>(std::atol(num.c_str()));
    switch (suffix)
    {
        case 'K': value *= 1024; break;
        case 'M': value *= 1024 * 1024; break;
        case 'G': value *= 1024 * 1024 * 1024; break;
        case 0: break;
        default:
            std::cerr << "Error: Invalid size suffix '" << suffix << "'" << std::endl;
            exit(1);
    }
    return value;
}

void ConfigParser::checkDuplicatePorts(const std::vector<ServerConfig>& configs)
{
    for (size_t i = 0; i < configs.size(); ++i)
    {
        for (size_t j = i + 1; j < configs.size(); ++j)
        {
            if (configs[i].port == configs[j].port)
            {
                std::cerr << "Error: Duplicate port " << configs[i].port << std::endl;
                exit(1);
            }
        }
    }
}
