#!/usr/bin/env python3
import os
print("Content-Type: text/html")
print()
print("<html><body><h1>CGI Test</h1><pre>")
for k, v in sorted(os.environ.items()):
    print("{0} = {1}".format(k, v))
print("</pre></body></html>")
