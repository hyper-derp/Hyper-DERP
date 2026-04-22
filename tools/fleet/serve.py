#!/usr/bin/env python3
"""WSGI app serving a fleet policy bundle.

Reads the bundle from disk on every request (or serves a
cached copy if mtime is unchanged). nginx sits in front
of this handling TLS termination + mTLS client-cert
verification + rate limiting + access logs. This script
does no auth of its own; its only job is to serve the
bundle to already-authenticated clients.

Endpoints:

  GET /api/v1/policy              200 bundle JSON
  GET /api/v1/policy?since=<v>    304 if client has the
                                  current version, else
                                  200 bundle JSON
  GET /api/v1/revocations         200 revocations subset

Run behind nginx/uwsgi:

  uwsgi --socket /run/fleet-serve.sock \\
        --wsgi-file serve.py \\
        --callable application
"""

import json
import os
import threading
import time

BUNDLE_PATH = os.environ.get(
    "HD_FLEET_BUNDLE_PATH",
    "/var/fleet/bundle.json")

_lock = threading.Lock()
_cache = {"mtime": 0.0, "body": b"", "version": 0,
          "revocations": {}}


def _load():
  try:
    st = os.stat(BUNDLE_PATH)
  except FileNotFoundError:
    return None
  with _lock:
    if st.st_mtime == _cache["mtime"]:
      return _cache
    with open(BUNDLE_PATH, "rb") as f:
      body = f.read()
    try:
      parsed = json.loads(body)
    except ValueError:
      return None
    _cache["mtime"] = st.st_mtime
    _cache["body"] = body
    _cache["version"] = int(parsed.get("version", 0))
    _cache["revocations"] = parsed.get(
        "revocations", {})
    return _cache


def _respond(start_response, status, body, ctype):
  start_response(status, [
      ("Content-Type", ctype),
      ("Content-Length", str(len(body))),
      ("Cache-Control", "no-store"),
  ])
  return [body]


def application(environ, start_response):
  path = environ.get("PATH_INFO", "")
  qs = environ.get("QUERY_STRING", "")
  cache = _load()
  if cache is None:
    return _respond(start_response, "503 Unavailable",
                    b"bundle missing\n",
                    "text/plain")

  if path == "/api/v1/policy":
    since = 0
    for pair in qs.split("&"):
      if pair.startswith("since="):
        try:
          since = int(pair.split("=", 1)[1])
        except ValueError:
          since = 0
    if since > 0 and since >= cache["version"]:
      start_response("304 Not Modified", [])
      return [b""]
    return _respond(start_response, "200 OK",
                    cache["body"], "application/json")
  if path == "/api/v1/revocations":
    payload = {
        "version": cache["version"],
        "revocations": cache["revocations"],
    }
    body = json.dumps(payload,
                      sort_keys=True).encode("utf-8")
    return _respond(start_response, "200 OK", body,
                    "application/json")
  return _respond(start_response, "404 Not Found",
                  b"not found\n", "text/plain")


if __name__ == "__main__":
  # Tiny dev server for local testing only; use uwsgi
  # behind nginx in production.
  from wsgiref.simple_server import make_server
  port = int(os.environ.get("PORT", 8080))
  with make_server("127.0.0.1", port, application) as s:
    print(f"dev fleet server on :{port} "
          f"bundle={BUNDLE_PATH}")
    s.serve_forever()
