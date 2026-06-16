#include "ConfigParser.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <set>

// ---------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------

namespace {

void error(const std::string& msg)
{
    std::cerr << "Error: " << msg << std::endl;
    exit(1);
}

void expectArgCount(const std::vector<std::string>& args, size_t n)
{
    if (args.size() != n)
    {
        std::ostringstream oss;
        oss << "Directive '" << args[0] << "' expects " << (n - 1) << " argument(s)";
        error(oss.str());
    }
}

size_t parseSize(const std::string& str)
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
        default: error("Invalid size suffix '" + std::string(1, suffix) + "'");
    }
    return value;
}

// --- location directive handlers --------------------------------

void handleRoot(LocationConfig& loc, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    loc.root = args[1];
}

void handleIndex(LocationConfig& loc, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    loc.index = args[1];
}

void handleMethods(LocationConfig& loc, const std::vector<std::string>& args)
{
    loc.methods.clear();
    for (size_t i = 1; i < args.size(); ++i)
        loc.methods.insert(args[i]);
}

void handleAutoindex(LocationConfig& loc, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    if (args[1] == "on")
        loc.autoindex = true;
    else if (args[1] == "off")
        loc.autoindex = false;
    else
        error("autoindex must be 'on' or 'off'");
}

void handleUploadStore(LocationConfig& loc, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    loc.upload_store = args[1];
}

void handleCgiExt(LocationConfig& loc, const std::vector<std::string>& args)
{
    for (size_t i = 1; i < args.size(); ++i)
        loc.cgi_ext.push_back(args[i]);
}

void handleReturn(LocationConfig& loc, const std::vector<std::string>& args)
{
    if (args.size() < 3)
        error("return needs code and URL");
    loc.has_redirect = true;
    loc.redirect_code = std::atoi(args[1].c_str());
    loc.redirect_url = args[2];
}

} // anonymous namespace

// ---------------------------------------------------------------
//  ConfigParser implementation
// ---------------------------------------------------------------

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
        error("Cannot open config file: " + filename);
    std::stringstream ss;
    ss << file.rdbuf();
    content_ = ss.str();
}

void ConfigParser::skipWhitespaceAndComments()
{
    const std::string& s = content_;
    while (pos_ < s.size())
    {
        char c = s[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            pos_++;
        else if (c == '#')
        {
            while (pos_ < s.size() && s[pos_] != '\n')
                pos_++;
        }
        else
            break;
    }
}

ConfigParser::Token ConfigParser::nextToken()
{
    Token t = peekToken();
    if (t.type != TOKEN_EOF)
        token_pos_++;
    return t;
}

ConfigParser::Token ConfigParser::peekToken()
{
    if (token_pos_ < tokens_.size())
        return tokens_[token_pos_];
    return Token();
}

void ConfigParser::tokenize()
{
    pos_ = 0;
    token_pos_ = 0;
    tokens_.clear();

    while (pos_ < content_.size())
    {
        skipWhitespaceAndComments();
        if (pos_ >= content_.size())
            break;

        char c = content_[pos_];
        if (c == '{')
        {
            tokens_.push_back(Token(TOKEN_LEFT_BRACE, "{"));
            pos_++;
        }
        else if (c == '}')
        {
            tokens_.push_back(Token(TOKEN_RIGHT_BRACE, "}"));
            pos_++;
        }
        else if (c == ';')
        {
            tokens_.push_back(Token(TOKEN_SEMICOLON, ";"));
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
                tokens_.push_back(Token(TOKEN_WORD, word));
        }
    }
}

std::vector<ServerConfig> ConfigParser::parseConfig()
{
    std::vector<ServerConfig> configs;
    while (peekToken().type != TOKEN_EOF)
    {
        Token token = nextToken();
        if (token.type == TOKEN_WORD && token.value == "server")
            configs.push_back(parseServer());
        else
            error("Expected 'server' at top level");
    }
    return configs;
}

ServerConfig ConfigParser::parseServer()
{
    ServerConfig server;

    if (nextToken().type != TOKEN_LEFT_BRACE)
        error("Expected '{' after 'server'");

    while (peekToken().type != TOKEN_RIGHT_BRACE && peekToken().type != TOKEN_EOF)
    {
        Token name = nextToken();
        if (name.type != TOKEN_WORD)
            error("Expected directive name in server block");

        if (name.value == "location")
        {
            server.locations.push_back(parseLocation());
        }
        else
        {
            std::vector<std::string> args = readDirectiveArgs(name.value);

            if (args[0] == "listen")
                parseListen(server, args);
            else if (args[0] == "server_name")
                parseServerName(server, args);
            else if (args[0] == "client_max_body_size")
                parseClientMaxBodySize(server, args);
            else if (args[0] == "error_page")
                parseErrorPage(server, args);
            else
                error("Unknown directive '" + args[0] + "'");
        }
    }

    if (nextToken().type != TOKEN_RIGHT_BRACE)
        error("Expected '}' to close server block");

    return server;
}

LocationConfig ConfigParser::parseLocation()
{
    LocationConfig loc;

    Token name = nextToken();
    if (name.type != TOKEN_WORD)
        error("Expected path after 'location'");
    loc.path = name.value;

    if (nextToken().type != TOKEN_LEFT_BRACE)
        error("Expected '{' after location path");

    while (peekToken().type != TOKEN_RIGHT_BRACE && peekToken().type != TOKEN_EOF)
    {
        Token name = nextToken();
        if (name.type != TOKEN_WORD)
            error("Expected directive in location block");

        std::vector<std::string> args = readDirectiveArgs(name.value);

        if (args[0] == "root")
            handleRoot(loc, args);
        else if (args[0] == "index")
            handleIndex(loc, args);
        else if (args[0] == "methods")
            handleMethods(loc, args);
        else if (args[0] == "autoindex")
            handleAutoindex(loc, args);
        else if (args[0] == "upload_store")
            handleUploadStore(loc, args);
        else if (args[0] == "cgi_ext")
            handleCgiExt(loc, args);
        else if (args[0] == "return")
            handleReturn(loc, args);
        else
            error("Unknown location directive '" + args[0] + "'");
    }

    if (nextToken().type != TOKEN_RIGHT_BRACE)
        error("Expected '}' to close location block");

    return loc;
}

std::vector<std::string> ConfigParser::readDirectiveArgs(const std::string& name)
{
    std::vector<std::string> args;
    args.push_back(name);
    while (peekToken().type == TOKEN_WORD)
        args.push_back(nextToken().value);
    if (nextToken().type != TOKEN_SEMICOLON)
        error("Expected ';' after directive '" + name + "'");
    return args;
}

void ConfigParser::parseListen(ServerConfig& server, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    std::string value = args[1];
    size_t colon = value.find(':');
    if (colon != std::string::npos)
    {
        server.host = value.substr(0, colon);
        std::istringstream(value.substr(colon + 1)) >> server.port;
    }
    else
    {
        std::istringstream(value) >> server.port;
    }
    if (server.port <= 0 || server.port > 65535)
    {
        std::ostringstream oss;
        oss << "Invalid port number: " << server.port;
        error(oss.str());
    }
}

void ConfigParser::parseServerName(ServerConfig& server, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    server.server_name = args[1];
}

void ConfigParser::parseClientMaxBodySize(ServerConfig& server, const std::vector<std::string>& args)
{
    expectArgCount(args, 2);
    server.client_max_body_size = parseSize(args[1]);
}

void ConfigParser::parseErrorPage(ServerConfig& server, const std::vector<std::string>& args)
{
    expectArgCount(args, 3);
    int code = std::atoi(args[1].c_str());
    server.error_pages[code] = args[2];
}

void ConfigParser::checkDuplicatePorts(const std::vector<ServerConfig>& configs)
{
    std::set<int> ports;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        int port = configs[i].port;
        if (ports.find(port) != ports.end())
        {
            std::ostringstream oss;
            oss << "Duplicate port " << port;
            error(oss.str());
        }
        ports.insert(port);
    }
}
