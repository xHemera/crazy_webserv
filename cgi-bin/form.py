#!/usr/bin/env python3
import os
import sys
import html

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
    <link rel="icon" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==">
    <style>
        .result {{ background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); text-align: left; margin: 25px 0; }}
        .result h2 {{ margin-top: 0; color: #3498db; font-size: 1.4em; }}
        .info-row {{ display: flex; justify-content: space-between; padding: 12px 0; border-bottom: 1px solid #eee; }}
        .info-row:last-child {{ border-bottom: none; }}
        .info-label {{ font-weight: 600; color: #555; }}
        .badge {{ display: inline-block; background: #2ecc71; color: white; padding: 4px 12px; border-radius: 20px; font-weight: bold; font-size: 0.9em; }}
        code {{ background: #eef; padding: 2px 6px; border-radius: 4px; white-space: pre-wrap; word-break: break-word; display: block; margin-top: 10px; padding: 15px; }}
    </style>
</head>
<body>
    <div class="container">
        <h1>CGI Form Result</h1>
        <p class="subtitle">Data received via POST method</p>
        <div class="result">
            <h2>Request Details</h2>
            <div class="info-row">
                <span class="info-label">Request Method</span>
                <span class="badge">{}</span>
            </div>
            <div class="info-row">
                <span class="info-label">Content Length</span>
                <span>{} bytes</span>
            </div>
            <div class="info-row">
                <span class="info-label">Parsed Body</span>
            </div>
            <code>{}</code>
        </div>
        <p><a href="/">&larr; Back to home</a></p>
    </div>
</body>
</html>""".format(os.environ.get('REQUEST_METHOD', 'UNKNOWN'), content_length, html.escape(body) if body else "(empty)"))
