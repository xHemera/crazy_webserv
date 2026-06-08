Étape 0 — Structure du projet

- Makefile avec -Wall -Wextra -Werror -std=c++98, règles all/clean/fclean/re, pas de relink, dépendances -MMD pour les headers
- conf/default.conf — config par défaut avec serveur sur 127.0.0.1:8080 + 3 locations (/, /upload, /cgi-bin)
- www/index.html, www/error/{404,500}.html, www/uploads/
- cgi-bin/test.py + test.php
- README.md obligatoire (première ligne en italique, sections Description/Instructions/Resources)

Étape 1 — Parseur de fichier de configuration

- ServerConfig.hpp + ServerConfig.cpp — structures ServerConfig et LocationConfig avec valeurs par défaut conformes au sujet
- ConfigParser.hpp + ConfigParser.cpp — tokenizer + parseur NGINX-like (blocs server { … }, location / { … }, directives avec ;, commentaires #)
- main.cpp — point d'entrée, parse et affiche la config (ou exit avec erreur)
  Vérifications passées :
- make → compilation sans warning
- make (2e fois) → rien à faire (no relink)
- make re → rebuild complet + exécution correcte
- Fichier config inexistant → Error: Cannot open config file
- Port dupliqué → Error: Duplicate port 8080
