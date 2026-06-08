#ifndef CONFIGPARSER_HPP
#define CONFIGPARSER_HPP

#include <string>
#include <vector>
#include "ServerConfig.hpp"

class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    std::vector<ServerConfig> parse(const std::string& filename);
    std::vector<ServerConfig> parseFromDefault();

private:
    enum TokenType {
        TOKEN_WORD,
        TOKEN_LEFT_BRACE,
        TOKEN_RIGHT_BRACE,
        TOKEN_SEMICOLON,
        TOKEN_EOF
    };

    struct Token {
        TokenType type;
        std::string value;
    };

    std::string content_;
    size_t pos_;
    std::vector<Token> tokens_;
    size_t token_pos_;

    void loadFile(const std::string& filename);
    void tokenize();
    Token nextToken();
    Token peekToken();
    void skipWhitespaceAndComments();

    std::vector<ServerConfig> parseConfig();
    ServerConfig parseServer();
    LocationConfig parseLocation();
    void parseListen(ServerConfig& server, const std::vector<std::string>& args);
    void parseServerName(ServerConfig& server, const std::vector<std::string>& args);
    void parseClientMaxBodySize(ServerConfig& server, const std::vector<std::string>& args);
    void parseErrorPage(ServerConfig& server, const std::vector<std::string>& args);

    void checkDuplicatePorts(const std::vector<ServerConfig>& configs);

    static size_t parseSize(const std::string& str);
};

#endif
