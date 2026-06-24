#!/usr/bin/env python3
import os
import sys

content_length = int(os.environ.get('CONTENT_LENGTH', 0))
body = sys.stdin.read(content_length) if content_length > 0 else ""

print("Content-Type: text/html")
print()

print("""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>CGI Form Result - webserv</title>
    <link rel="stylesheet" href="/style.css">
    <style>
        .result { background: #eef; padding: 20px; border-radius: 8px; text-align: left; margin: 20px 0; }
        .result code { white-space: pre-wrap; word-break: break-word; }
    </style>
</head>
<body>
    <div class="container">
        <h1>CGI Form Result</h1>
        <p class="subtitle">Data received via POST method</p>
        <div class="result">
            <p><strong>Request Method:</strong> {}</p>
            <p><strong>Content Length:</strong> {}</p>
            <p><strong>Body:</strong></p>
            <code>{}</code>
        </div>
        <p><a href="/">&larr; Back to home</a></p>
    </div>
</body>
</html>""".format(os.environ.get('REQUEST_METHOD', 'UNKNOWN'), content_length, body))
