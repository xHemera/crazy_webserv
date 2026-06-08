# Webserv — Roadmap complète et détaillée

> Basée sur le **sujet v24.0** et la **grille de correction** (peer-evaluation).
> Lire et relire ces deux documents AVANT de commencer. Un détail oublié = 0.

---

## Architecture générale (à comprendre avant d'écrire une ligne)

```
┌──────────────────────────────────────────────────────────────┐
│                       webserv (C++ 98)                        │
│                                                              │
│  1. Parse config file  →  ServerConfig[]                     │
│  2. Create sockets      →  bind() + listen() sur chaque port │
│  3. Main loop:          →  poll() / epoll() / select()       │
│     - attendre lecture ET écriture SIMULTANÉMENT             │
│     - 1 seul poll() pour TOUT (listen + clients)             │
│  4. Handler:            →  parse HTTP request                │
│                          →  router (match route config)       │
│                          →  exécuter: GET / POST / DELETE     │
│                             ou CGI / upload / directory_list  │
│                          →  envoyer réponse HTTP              │
│  5. CGI:                →  fork() + execve()                 │
│                          →  pipe() pour stdin/stdout          │
│                          →  pas de poll() pour les pipes CGI  │
│                             (fork interdit dans le main loop) │
└──────────────────────────────────────────────────────────────┘
```

---

## Étape 0 : Mise en place du projet

### 0.1 Structure de fichiers attendue

```
webserv/
├── Makefile
├── *.cpp, *.hpp, *.h, *.tpp, *.ipp
├── conf/                     # Fichiers de configuration
│   └── default.conf
├── www/                      # Site statique de test
│   ├── index.html
│   ├── error/
│   │   ├── 404.html
│   │   └── 500.html
│   └── uploads/              # Dossier où les fichiers uploadés atterrissent
├── cgi-bin/                  # Scripts CGI de test
│   ├── test.py
│   └── test.php
└── README.md                 # OBLIGATOIRE (voir section spéciale)
```

### 0.2 Makefile

```makefile
CXX = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98
NAME = webserv
SRCS = main.cpp Server.cpp Config.cpp HttpRequest.cpp HttpResponse.cpp \
       Router.cpp CGIHandler.cpp PollManager.cpp ...
OBJS = $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(NAME) $(OBJS)

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
```

**Regle CRITIQUE** : pas de relink. Le Makefile ne doit pas recompiler si rien n'a change (les regles ci-dessus le gerent via les dependances .o -> .cpp).

### 0.3 Compilation

```bash
make          # compile tout
make re       # clean + compile
```

---

## Etape 1 : Parseur de fichier de configuration

### 1.1 Format du fichier (inspire de NGINX)

```
server {
    listen 127.0.0.1:8080;
    server_name example.com;
    client_max_body_size 10M;
    error_page 404 /error/404.html;
    error_page 500 /error/500.html;

    location / {
        root /var/www/html;
        index index.html;
        methods GET POST;
        autoindex on;
    }

    location /upload {
        root /var/www/uploads;
        methods POST;
        upload_store /var/www/uploads;
    }

    location /cgi-bin {
        root ./cgi-bin;
        methods GET POST;
        cgi_ext .py .php;
    }

    location /redirect {
        return 301 http://example.com/new;
    }
}

server {
    listen 127.0.0.1:8081;
    server_name autre.com;
    ...
}
```

### 1.2 Ce que le parseur doit extraire (PAR server)

| Champ | Description | Obligatoire |
|--------|-------------|-------------|
| `listen` | adresse:port (ou juste port) | OUI |
| `server_name` | nom d'hote virtuel | non (optionnel) |
| `client_max_body_size` | taille max body client | defaut = 1M |
| `error_page` | code -> chemin page d'erreur | defaut = page par defaut |
| `location` | bloc de route (chemin) | OUI (au moins /) |

**Par location** :

| Champ | Description |
|--------|-------------|
| `root` | repertoire racine pour cette route |
| `index` | fichier par defaut pour un dossier |
| `methods` | methodes HTTP autorisees (GET POST DELETE) |
| `autoindex` | on/off listing de dossier |
| `upload_store` | dossier de stockage upload |
| `cgi_ext` | extensions CGI (.py, .php) |
| `return` | redirection HTTP (code + URL) |

### 1.3 Gestion des erreurs de config

- Fichier inexistant -> erreur, exit
- Syntaxe invalide -> erreur, exit
- Meme port declare 2x dans le meme fichier -> erreur, exit (correction le demande)
- Si aucune config fournie en argument -> chercher un chemin par defaut (sujet: "available in a default path")

---

## Etape 2 : Initialisation des sockets

### 2.1 Creation des sockets d'ecoute

Pour chaque `listen` dans la config :

```cpp
// 1. getaddrinfo() pour resoudre l'adresse/port
struct addrinfo hints, *res;
memset(&hints, 0, sizeof(hints));
hints.ai_family = AF_UNSPEC;       // IPv4 ou IPv6
hints.ai_socktype = SOCK_STREAM;   // TCP
hints.ai_flags = AI_PASSIVE;
getaddrinfo(host, port_str, &hints, &res);

// 2. socket()
int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

// 3. setsockopt() SO_REUSEADDR (fortement recommande)
int opt = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

// 4. bind()
bind(sock, res->ai_addr, res->ai_addrlen);

// 5. listen()
listen(sock, SOMAXCONN);   // ou 128

// 6. fcntl() NON-BLOCKING
fcntl(sock, F_SETFL, O_NONBLOCK);

// 7. Stocker le socket + la config associee
freeaddrinfo(res);
```

**IMPORTANT** : Chaque socket d'ecoute doit etre associe a sa configuration `ServerConfig` pour savoir quel contenu servir.

### 2.2 Cas particulier : plusieurs serveurs sur le meme port

Si 2 blocs `server` ont le meme port mais des `server_name` differents, on les fusionne : un seul socket listen, mais quand un client arrive, on utilise le `Host` header pour choisir la bonne config.

---

## Etape 3 : La boucle principale - LE COEUR DU PROJET

### 3.1 LE POINT LE PLUS CRITIQUE (correction le verifie en premier)

- **1 seul** `poll()` (ou `select()`/`epoll()`/`kqueue()`) dans tout le programme.
- Ce `poll()` est dans la **boucle principale**.
- Il surveille **lecture ET ecriture simultanement**.
- **1 lecture ou 1 ecriture max par client par appel a poll()**.

### 3.2 Structure de la boucle

```cpp
// File descriptor sets
// Linux -> on peut utiliser poll() ou epoll()
// MacOS -> on peut utiliser poll() ou kqueue()

struct Client {
    int fd;
    ServerConfig* server;          // config associee
    HttpRequest request;           // requete en cours de reception
    HttpResponse response;         // reponse en cours d'envoi
    bool reading_done;             // requete completement recue ?
    bool writing_done;             // reponse completement envoyee ?
    time_t last_activity;          // pour timeout
};

// Conteneurs
std::vector<int> listen_sockets;
std::map<int, Client> clients;     // fd -> Client

while (true) {
    // 1. Preparer les fd_set / pollfd
    //    - Ajouter TOUS les listen sockets (en lecture)
    //    - Ajouter les clients qui veulent lire (en lecture)
    //    - Ajouter les clients qui veulent ecrire (en ecriture)

    // 2. Appeler poll() / select() / epoll_wait() / kevent()
    //    -> timeout optionnel (pour gerer les timeout clients)

    // 3. Parcourir les resultats :
    //    a) Si listen socket pret -> accept() -> nouveau client
    //       -> fcntl(client_fd, F_SETFL, O_NONBLOCK)
    //       -> l'associer a la bonne ServerConfig (via le port + Host header)

    //    b) Si client socket pret en lecture -> read()/recv()
    //       -> stocker les donnees dans client.request
    //       -> si requete complete -> passer a la phase "handler"
    //       -> si erreur/fermeture -> close() et remove client

    //    c) Si client socket pret en ecriture -> write()/send()
    //       -> envoyer les donnees de client.response
    //       -> si tout est envoye -> close() (ou keep-alive) et remove client

    // 4. Verifier les timeouts (pas de requete qui hang)
}
```

### 3.3 Regles ABSOLUES a respecter

| Regle | Verifie par la correction |
|-------|--------------------------|
| Un seul `select()`/`poll()` dans le main loop | Question directe |
| Lecture ET ecriture surveilles en meme temps | Si non -> **0** |
| 1 read/recv ou 1 write/send max par client par poll() | |
| read/recv/write/send jamais sans passer par poll() | **FORBIDDEN** -> 0 |
| `errno` JAMAIS verifie apres read/write | **FORBIDDEN** -> 0 |
| `read()`/`recv()` valeur de retour verifiee correctement (pas juste -1 ou 0) | Les DEUX doivent etre checkes |
| Si erreur -> client retire immediatement | |
| Pas de `fork()` sauf pour CGI | |
| Non-blocking tout le temps | |

---

## Etape 4 : Parseur de requete HTTP

### 4.1 Ce qu'il faut parser

```
GET /path/to/resource?query=string HTTP/1.1\r\n
Host: example.com\r\n
Content-Type: application/x-www-form-urlencoded\r\n
Content-Length: 27\r\n
Transfer-Encoding: chunked\r\n
\r\n
body...
```

**Elements** :
- **Method** : GET, POST, DELETE, ou UNKNOWN
- **Path** : /path/to/resource
- **Query string** : ?key=value (optionnel)
- **HTTP version** : HTTP/1.0 ou HTTP/1.1
- **Headers** (map<string, string>) : Host, Content-Type, Content-Length, Transfer-Encoding, Cookie, etc.
- **Body** : brut

### 4.2 Chunked encoding (sujet l'exige)

```
POST /cgi HTTP/1.1\r\n
Transfer-Encoding: chunked\r\n
\r\n
7\r\n
Mozilla\r\n
9\r\n
Developer\r\n
0\r\n
\r\n
```

-> **Il faut dechunker** le body. Le CGI attend un body complet avec EOF a la fin. Sujet : "for chunked requests, your server needs to un-chunk them, the CGI will expect EOF as the end of the body."

### 4.3 Buffering

- On peut recevoir la requete en plusieurs fois (plusieurs appels a `read()` via `poll()`).
- Stocker dans un `std::string` buffer.
- Detecter la fin :
  - Si `Content-Length` present -> body complet = length
  - Si `Transfer-Encoding: chunked` -> fin = dernier chunk de taille 0
  - Si GET/DELETE -> pas de body (fin = `\r\n\r\n`)

### 4.4 Limite de body

- Comparer avec `client_max_body_size` de la config.
- Si body > limite -> repondre **413 Request Entity Too Large**.

---

## Etape 5 : Routeur et matching

### 5.1 Algorithme de matching

```
URL recue: /kapouet/pouic/toto/pouet

Locations configurees:
  /kapouet          root = /tmp/www
  /                 root = /var/www/html

-> Match le PLUS SPECIFIQUE : /kapouet
-> Ressource = /tmp/www/pouic/toto/pouet
```

### 5.2 Verifications

1. **Methode autorisee** ? Si non -> **405 Method Not Allowed**
2. **Redirection** configuree (`return`) -> envoyer 301/302 avec Location
3. **CGI** si l'extension du fichier correspond a `cgi_ext` -> handler CGI
4. **Upload** si methode POST + `upload_store` configure -> handler upload
5. **Fichier statique** -> servir le fichier
6. **Dossier** -> chercher `index` OU `autoindex` (directory listing)
7. **Rien trouve** -> **404 Not Found**

---

## Etape 6 : Gestion des reponses HTTP

### 6.1 Status codes a connaitre (correction verifie)

| Code | Signification |
|------|---------------|
| 200 | OK |
| 201 | Created |
| 204 | No Content |
| 301 | Moved Permanently |
| 302 | Found |
| 400 | Bad Request |
| 403 | Forbidden |
| 404 | Not Found |
| 405 | Method Not Allowed |
| 413 | Payload Too Large |
| 500 | Internal Server Error |
| 501 | Not Implemented |
| 505 | HTTP Version Not Supported |

**Correction** : "Search for the HTTP response status codes list on the internet. During this evaluation, if any status codes is wrong, don't give any related points." -> Les codes DOIVENT etre corrects.

### 6.2 Format de la reponse

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 128\r\n
Connection: close\r\n
\r\n
<html>...
```

**Headers a envoyer** :
- `Content-Type` (base sur l'extension du fichier)
- `Content-Length` (taille du body)
- `Connection: close` (ou `keep-alive` si tu geres)
- `Location` (pour les redirections)
- `Set-Cookie` (bonus cookies)
- `Date` (optionnel mais bien)
- `Server` (optionnel)

### 6.3 Envoi de la reponse

- Bufferiser la reponse.
- Envoyer via `write()`/`send()` dans le main loop (quand `poll()` dit que le fd est pret en ecriture).
- Si tout n'a pas ete envoye en un `write()`, garder le reste dans un buffer et continuer au prochain `poll()`.
- **1 seul write/send par client par poll()**.

### 6.4 Error pages

- Si config a `error_page 404 /error/404.html` -> servir ce fichier.
- Sinon -> page par defaut integree au code (hardcodee) :
  ```html
  <html><body><h1>404 Not Found</h1></body></html>
  ```

---

## Etape 7 : Methodes HTTP

### 7.1 GET

- Lire le fichier sur le disque.
- Envoyer le contenu avec `Content-Type` approprie.
- Si dossier -> `index` ou `autoindex` ou 403.
- Si CGI -> passer au handler CGI.

### 7.2 POST

- Lire le body de la requete.
- Si `upload_store` configure -> sauver le body dans un fichier dans ce dossier.
  - Generer un nom de fichier unique (timestamp).
  - Repondre **201 Created** avec le chemin du fichier.
- Si CGI -> passer au handler CGI (le body va dans stdin du CGI).

### 7.3 DELETE

- Supprimer le fichier sur le disque.
- Si succes -> **204 No Content**.
- Si fichier inexistant -> **404 Not Found**.
- Si pas de permission -> **403 Forbidden**.

### 7.4 UNKNOWN methods

- Repondre **501 Not Implemented**.
- NE JAMAIS crasher. Correction : "UNKNOWN requests should not result in a crash."

---

## Etape 8 : CGI Handler

### 8.1 Creation du processus CGI

```cpp
// 1. Preparer les pipes
int stdin_pipe[2];   // pour envoyer le body au CGI (optionnel)
int stdout_pipe[2];  // pour lire la sortie du CGI
pipe(stdin_pipe);
pipe(stdout_pipe);

// 2. fork()
pid_t pid = fork();

if (pid == 0) {
    // --- Processus enfant (CGI) ---

    // Rediriger stdin/stdout
    dup2(stdin_pipe[0], STDIN_FILENO);   // lire depuis le pipe stdin
    dup2(stdout_pipe[1], STDOUT_FILENO); // ecrire vers le pipe stdout

    // Fermer les extremites inutilisees
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    // Preparer les variables d'environnement
    // (voir 8.2)
    char **env = build_cgi_env(request, server_config);

    // chdir() dans le bon repertoire
    // Sujet: "CGI should be run in the correct directory for relative path file access"
    chdir(cgi_directory);

    // execve()
    execve(script_path, args, env);
    // Si on arrive ici -> erreur
    exit(1);
}

// --- Processus parent (serveur) ---
close(stdin_pipe[0]);  // on n'ecrit que dans stdin du CGI
close(stdout_pipe[1]); // on ne lit que depuis stdout du CGI

// Envoyer le body au CGI via stdin_pipe[1]
write(stdin_pipe[1], request.body.c_str(), request.body.size());
close(stdin_pipe[1]);  // EOF -> le CGI comprend que le body est fini

// Lire la sortie du CGI via stdout_pipe[0]
// ATTENTION : on ne peut PAS utiliser poll() pour les pipes
// car le sujet dit: "You are not required to use poll() for regular disk files"
// et les pipes sont aussi exclues du poll() obligatoire.
// On peut donc faire read() bloquant ou utiliser un timeout avec alarm()
char buffer[4096];
std::string cgi_output;
int ret;
while ((ret = read(stdout_pipe[0], buffer, sizeof(buffer))) > 0) {
    cgi_output.append(buffer, ret);
}
close(stdout_pipe[0]);

// Attendre la fin du processus enfant
int status;
waitpid(pid, &status, 0);

// Parser la sortie du CGI (headers + body separes par \r\n\r\n)
// Construire la reponse HTTP
build_response_from_cgi_output(cgi_output);
```

### 8.2 Variables d'environnement CGI

Le sujet dit: "Have a careful look at the environment variables involved in the web server-CGI communication. The full request and arguments provided by the client must be available to the CGI."

Variables a passer (norme CGI/1.1) :

| Variable | Valeur | Exemple |
|----------|--------|---------|
| `SERVER_SOFTWARE` | webserv/1.0 | |
| `SERVER_NAME` | hostname du server | localhost |
| `GATEWAY_INTERFACE` | CGI/1.1 | |
| `SERVER_PROTOCOL` | HTTP/1.1 | |
| `SERVER_PORT` | port | 8080 |
| `REQUEST_METHOD` | GET/POST/DELETE | GET |
| `PATH_INFO` | chemin de la requete | /cgi-bin/test.py |
| `PATH_TRANSLATED` | chemin physique complet | |
| `SCRIPT_NAME` | chemin du script | /cgi-bin/test.py |
| `QUERY_STRING` | partie apres ? | key=value |
| `CONTENT_TYPE` | Content-Type header | text/plain |
| `CONTENT_LENGTH` | Content-Length header | 27 |
| `HTTP_*` | tous les headers HTTP | HTTP_HOST, HTTP_USER_AGENT... |
| `REMOTE_ADDR` | IP du client | 127.0.0.1 |
| `REMOTE_HOST` | hostname du client | |

### 8.3 Cas particuliers CGI

- **GET CGI** : pas de body, `QUERY_STRING` contient les parametres.
- **POST CGI** : body envoye via stdin du CGI, `CONTENT_LENGTH` defini.
- **Chunked request** : dechunker AVANT d'envoyer au CGI. Le CGI attend EOF comme fin du body.
- **Output CGI sans Content-Length** : Si le CGI ne renvoie pas d'en-tete `Content-Length`, EOF marque la fin. Sujet : "If no content_length is returned from the CGI, EOF will mark the end of the returned data."
- **Script qui boucle infiniment** : le serveur ne doit pas crasher. Gerer un timeout (avec `alarm()` ou en non-blocking sur le pipe).
- **Script qui produit une erreur** : le serveur doit renvoyer une erreur 500 ou appropriee.
- **Le serveur ne doit jamais crasher** quoi qu'il arrive.

---

## Etape 9 : Directory listing (autoindex)

- Si `autoindex on` et que l'URL pointe vers un dossier sans `index` :
  - Utiliser `opendir()` + `readdir()` pour lister les fichiers.
  - Generer une page HTML avec les liens vers chaque fichier/dossier.
  - Format : simple tableau HTML avec nom, taille, date.

```cpp
DIR* dir = opendir(path.c_str());
struct dirent* entry;
std::string listing = "<html><body><ul>";
while ((entry = readdir(dir)) != NULL) {
    listing += "<li><a href=\"" + std::string(entry->d_name) + "\">"
               + entry->d_name + "</a></li>";
}
closedir(dir);
listing += "</ul></body></html>";
```

- Si `autoindex off` et pas de `index` -> **403 Forbidden**.

---

## Etape 10 : File upload

- Quand `upload_store` est configure dans la location et que la methode est POST :
  - Lire le body de la requete (deja fait dans le parseur).
  - Generer un nom de fichier unique.
  - Ecrire le body dans `upload_store/nom_unique`.
  - Repondre **201 Created** avec un body contenant le chemin du fichier uploade.
- Sujet : "Uploading files from the clients to the server is authorized, and storage location is provided."

---

## Etape 11 : Static file serving

- Utiliser `stat()` pour verifier l'existence et le type du fichier.
- Si c'est un fichier regulier :
  - Utiliser `open()` + `read()` pour lire le contenu.
  - NOTA : la lecture de fichiers disques n'a PAS besoin de passer par poll(). Sujet : "You are not required to use poll() for regular disk files."
- Determiner le `Content-Type` via l'extension :
  - .html -> text/html
  - .css -> text/css
  - .js -> text/javascript
  - .png -> image/png
  - .jpg -> image/jpeg
  - .gif -> image/gif
  - .ico -> image/x-icon
  - .pdf -> application/pdf
  - .zip -> application/zip
  - etc.
- Envoyer avec `Content-Length` et `Content-Type` appropries.

---

## Etape 12 : Gestion des timeouts

- Chaque client a un `last_activity` (timestamp).
- Dans la boucle principale, apres chaque poll(), verifier :
  ```cpp
  if (now - client.last_activity > TIMEOUT_SECONDS) {
      close(client.fd);
      clients.erase(client.fd);
  }
  ```
- Sujet : "A request to your server should never hang indefinitely."
- Valeur recommandee : 30-60 secondes.

---

## Etape 13 : README.md

Fichier obligatoire a la racine du repo. Sujet chapitre V.

Contenu minimum :

```markdown
*This project has been created as part of the 42 curriculum by <login1>[, <login2>[...]].*

## Description
[Description du projet, son but, apercu]

## Instructions
[Compilation, installation, execution]

## Resources
[Documentation, articles, tutoriels]
[Description de comment l'IA a ete utilisee]
```

- Doit etre en anglais.
- Premiere ligne en italique avec les logins.
- Sections "Description", "Instructions", "Resources".

---

## Etape 14 : Tests et validation

### 14.1 Tests avec curl (correction)

```bash
# Test GET
curl -v http://localhost:8080/

# Test POST
curl -X POST -H "Content-Type: plain/text" --data "BODY IS HERE" http://localhost:8080/

# Test DELETE
curl -X DELETE http://localhost:8080/file.txt

# Test avec hostname different
curl --resolve example.com:80:127.0.0.1 http://example.com/

# Test client body limit
curl -X POST -H "Content-Type: plain/text" --data "BODY IS HERE write something shorter or longer than body limit" http://localhost:8080/

# Test upload
curl -X POST -F "file=@test.txt" http://localhost:8080/upload/

# Test unknown method
curl -X OPTIONS http://localhost:8080/

# Test wrong URL
curl -v http://localhost:8080/nonexistent

# Test redirection
curl -v http://localhost:8080/redirect
```

### 14.2 Tests avec telnet

```bash
telnet localhost 8080
GET / HTTP/1.1
Host: localhost

```

### 14.3 Tests avec le navigateur

- Ouvrir Firefox/Chrome sur http://localhost:8080
- Verifier l'inspecteur reseau (onglet Network) pour voir headers requete et reponse
- Tester une URL inexistante
- Tester le directory listing si active
- Tester une URL redirigee

### 14.4 Tests avec siege (stress test)

```bash
# Installation
brew install siege   # sur Mac

# Test de stress
siege -b http://localhost:8080/

# Disponibilite doit etre > 99.5%
# Pas de fuite memoire (surveiller avec top/htop)
# Pas de connexions bloquantes
```

### 14.5 Tests CGI

```bash
# Test GET CGI
curl -v http://localhost:8080/cgi-bin/test.py?name=World

# Test POST CGI
curl -X POST -d "data=hello" http://localhost:8080/cgi-bin/test.py

# Test avec script qui boucle a l'infini
# -> le serveur ne doit pas crasher
```

### 14.6 Tests multi-ports

```bash
# Lancer avec config qui a plusieurs ports
./webserv conf/multi_port.conf

# Tester chaque port
curl -v http://localhost:8080/
curl -v http://localhost:8081/
```

### 14.7 Tests memoire

```bash
# Surveiller la memoire pendant les tests
while true; do
    ps -p $(pgrep webserv) -o %mem,rss
    sleep 1
done
# La memoire ne doit pas augmenter indefiniment
```

---

## Etape 15 : Verification avant soumission

Checklist finale :

- [ ] `make` -> compilation sans erreur
- [ ] `make re` -> clean rebuild
- [ ] Pas de relink (make 2 fois de suite, la 2e ne fait rien)
- [ ] `./webserv` sans argument -> utilise une config par defaut OU affiche une erreur
- [ ] `./webserv conf/test.conf` -> demarre correctement
- [ ] Test GET -> 200 OK
- [ ] Test POST -> 201 Created / 200 OK
- [ ] Test DELETE -> 204 No Content
- [ ] Test unknown method -> 501
- [ ] Test 404 -> bonne page
- [ ] Test 413 si body trop gros
- [ ] Test redirection -> 301/302 avec Location header
- [ ] Test directory listing -> HTML avec la liste
- [ ] Test upload -> fichier cree sur le disque
- [ ] Test CGI (GET) -> reponse correcte
- [ ] Test CGI (POST) -> reponse correcte
- [ ] Test CGI avec erreur -> pas de crash, message d'erreur
- [ ] Test navigateur -> site statique complet
- [ ] Test siege -b -> disponibilite > 99.5%
- [ ] Pas de fuite memoire
- [ ] Pas de connexions bloquantes
- [ ] Plusieurs ports fonctionnent
- [ ] Meme port avec plusieurs server_name fonctionne (Host header)
- [ ] Double declaration du meme port -> erreur au demarrage
- [ ] README.md present et complet
- [ ] Les fichiers de config et de test sont fournis
- [ ] Tout est dans le repo Git

---

## Rappel des pieges qui donnent 0 (correction)

1. **Pas de select()/poll() qui surveille read ET write en meme temps** -> 0
2. **errno check apres read/recv/write/send** -> 0 (et fin de l'eval)
3. **read/write sur socket sans passer par select()/poll()** -> 0
4. **fork() ailleurs que pour CGI** -> 0
5. **Segfault ou crash** -> 0
6. **Fuites memoire** -> flagged
7. **Recompilation (relink)** -> flagged "Invalid compilation"

---

## Ordre de developpement recommande

1. Makefile + structure de projet + classe de base
2. Parseur de config (lecture du fichier, stockage en memoire)
3. Sockets + boucle principale poll() (juste accept et read)
4. Parseur de requete HTTP (GET d'abord)
5. Serveur de fichiers statiques (GET)
6. Gestion des erreurs et codes HTTP
7. Routeur + matching location
8. POST + upload
9. DELETE
10. Directory listing (autoindex)
11. Configuration avancee (error_page, redirect, client max body)
12. CGI (fork/execve, pipes, env)
13. Gestion des timeouts
14. Tests complets (curl, telnet, navigateur, siege)
15. README.md
16. Bonus : cookies/session, multiples CGI
