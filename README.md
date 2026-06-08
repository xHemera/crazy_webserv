*This project has been created as part of the 42 curriculum by <login>.*

## Description

webserv is a HTTP/1.1 server written in C++98, inspired by NGINX. It handles GET, POST, and DELETE methods, serves static files, supports CGI execution, directory listing, file upload, and virtual hosting.

## Instructions

### Compilation

```bash
make
```

### Usage

```bash
./webserv [config_file]
```

If no config file is provided, `conf/default.conf` is used.

### Testing

```bash
curl -v http://localhost:8080/
```

### Cleanup

```bash
make clean   # remove object files
make fclean  # remove object files and binary
make re      # full rebuild
```

## Resources

- [RFC 7230 - HTTP/1.1 Message Syntax](https://tools.ietf.org/html/rfc7230)
- [RFC 7231 - HTTP/1.1 Semantics](https://tools.ietf.org/html/rfc7231)
- [NGINX documentation](https://nginx.org/en/docs/)
- [CGI/1.1 specification](https://tools.ietf.org/html/rfc3875)
