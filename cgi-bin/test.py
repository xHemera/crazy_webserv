#!/usr/bin/env python3
import os
import sys

print("Content-Type: text/html")
print()

print("""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>CGI Environment - webserv</title>
    <link rel="stylesheet" href="/style.css">
    <style>
        table { width: 100%%; border-collapse: collapse; margin-top: 20px; text-align: left; }
        th, td { padding: 10px; border-bottom: 1px solid #ddd; }
        th { background: #3498db; color: white; }
        tr:nth-child(even) { background: #f9f9f9; }
        code { background: #eef; padding: 2px 6px; border-radius: 4px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>CGI Environment Variables</h1>
        <p class="subtitle">All variables passed by the server to the CGI process</p>
        <table>
            <tr><th>Variable</th><th>Value</th></tr>""")

for k, v in sorted(os.environ.items()):
    print("<tr><td><code>{0}</code></td><td>{1}</td></tr>".format(k, v))

print("""        </table>
        <p><a href="/">&larr; Back to home</a></p>
    </div>
</body>
</html>""")
