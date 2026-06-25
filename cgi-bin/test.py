#!/usr/bin/env python3
import os
import sys
import html

method = os.environ.get('REQUEST_METHOD', 'UNKNOWN')
query = os.environ.get('QUERY_STRING', '')
content_length = int(os.environ.get('CONTENT_LENGTH', 0))
body = sys.stdin.read(content_length) if content_length > 0 else ""

print("Content-Type: text/html")
print()

print("""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>CGI Environment - webserv</title>
    <link rel="stylesheet" href="/style.css">
    <link rel="icon" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==">
    <style>
        .section {{ background: white; border-radius: 12px; padding: 25px; margin: 25px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.05); text-align: left; }}
        .section h2 {{ margin-top: 0; color: #3498db; font-size: 1.4em; }}
        table {{ width: 100%; border-collapse: collapse; margin-top: 15px; text-align: left; }}
        th, td {{ padding: 12px; border-bottom: 1px solid #eee; }}
        th {{ background: #3498db; color: white; }}
        tr:nth-child(even) {{ background: #f9f9f9; }}
        code {{ background: #eef; padding: 2px 6px; border-radius: 4px; word-break: break-word; }}
        .badge {{ display: inline-block; background: #2ecc71; color: white; padding: 4px 12px; border-radius: 20px; font-weight: bold; font-size: 0.9em; }}
        .info-row {{ display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #eee; }}
        .info-row:last-child {{ border-bottom: none; }}
        .info-label {{ font-weight: 600; color: #555; }}
    </style>
</head>
<body>
    <div class="container">
        <h1>CGI Environment</h1>
        <p class="subtitle">Visual overview of the CGI request and server variables</p>

        <div class="section">
            <h2>Request Overview</h2>
            <div class="info-row">
                <span class="info-label">Request Method</span>
                <span class="badge">{}</span>
            </div>
            <div class="info-row">
                <span class="info-label">Query String</span>
                <code>{}</code>
            </div>
            <div class="info-row">
                <span class="info-label">Content Length</span>
                <code>{}</code>
            </div>
            <div class="info-row">
                <span class="info-label">Script Name</span>
                <code>{}</code>
            </div>
        </div>
""".format(method, html.escape(query) if query else "(none)", content_length, os.environ.get('SCRIPT_NAME', '-')))

if body:
    print("""        <div class="section">
            <h2>Request Body</h2>
            <code>{}</code>
        </div>
""".format(html.escape(body)))

if method == "GET":
    print("""        <div class="section">
            <h2>Test POST request</h2>
            <form method="POST" action="/cgi-bin/test.py">
                <label for="msg">Message:</label>
                <input type="text" id="msg" name="msg" placeholder="Type something...">
                <button type="submit">Send POST</button>
            </form>
        </div>
""")

print("""        <div class="section">
            <h2>Environment Variables</h2>
            <table>
                <tr><th>Variable</th><th>Value</th></tr>""")

for k, v in sorted(os.environ.items()):
    print("                <tr><td><code>{0}</code></td><td>{1}</td></tr>".format(k, html.escape(v)))

print("""            </table>
        </div>
        <p><a href="/">&larr; Back to home</a></p>
    </div>
</body>
</html>""")
