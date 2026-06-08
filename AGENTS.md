# webserv — Agent guide

42 school project: write an HTTP/1.1 server in C++ 98.

## Build & run

```bash
make            # c++ -Wall -Wextra -Werror -std=c++98
make re         # full rebuild
./webserv [config_file]   # default config path if none given
```

No relinking allowed (standard `.o` → `.cpp` deps handle this).

## Non-negotiable rules (grade = 0 if broken)

- **Exactly one** `poll()`/`select()`/`epoll()`/`kqueue()` in the entire program, in the main loop.
- That single call monitors **both read and write readiness simultaneously.**
- At most **one `read`/`recv` or `write`/`send` per client per `poll()` call.**
- Never read/write a socket without going through `poll()` first.
- **Never check `errno`** after `read`/`recv`/`write`/`send`.
- `read`/`recv` return value must check both -1 **and** 0 (not just one).
- On any error, remove the client immediately.
- `fork()` is only allowed for CGI — nothing else.
- All sockets must be `O_NONBLOCK`.
- No segfaults, no crashes, no hangs (any → 0).

## Configuration file (NGINX-like)

Each `server { listen …; location / { … } }` block supports:
- `listen` — `host:port` (duplicate port across servers → error at startup)
- `server_name`, `client_max_body_size`, `error_page <code> <path>`
- `location <path>`: `root`, `index`, `methods`, `autoindex on/off`, `upload_store`, `cgi_ext` (`.py`), `return <code> <url>`

## Architecture in one pass

```
main loop (1x poll) → accept → parse HTTP → match route → handle
  GET    → static file / directory listing / CGI
  POST   → file upload / CGI
  DELETE → unlink
  else   → 501
```

- Disk file reads (`open`/`read`) are exempt from the `poll()` requirement.
- CGI uses `fork()` + `execve()` + `pipe()`; body sent via stdin, output read from stdout (blocking read on pipe is OK).
- Chunked requests must be dechunked before CGI sees them.
- If CGI omits `Content-Length`, EOF marks end of output.
- CGI gets full environment: `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `HTTP_*`, `SERVER_PORT`, `SCRIPT_NAME`, `PATH_INFO`, `PATH_TRANSLATED`, `REMOTE_ADDR`.

## Testing

```bash
curl -v http://localhost:8080/                      # GET
curl -X POST -d "body" http://localhost:8080/       # POST
curl -X DELETE http://localhost:8080/file           # DELETE
curl --resolve example.com:80:127.0.0.1 http://example.com/  # virtual host
siege -b http://localhost:8080/                     # stress (avail > 99.5%)
```

Attachments provided by 42: `tester`, `ubuntu_cgi_tester`, `cgi_tester`, `ubuntu_tester`.

## README.md (required)

First line italic: `*This project has been created as part of the 42 curriculum by <login>.*`
Sections: Description, Instructions, Resources (include how AI was used).

## Git

No commits yet — first commit should contain Makefile + sources + config files + README.md.
