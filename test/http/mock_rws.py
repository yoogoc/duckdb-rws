#!/usr/bin/env python3
"""A tiny stand-in for Rave Web Services.

It serves just enough of the protocol to exercise the extension's parsing and
error handling without credentials or a network: authentication, ODM documents
with unusual-but-legal XML, RWS business errors returned under HTTP 200, and
transient failures that must be retried.
"""

import sys
import threading
from base64 import b64encode
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

USER, PASSWORD = "tester", "s3cret"
EXPECTED_AUTH = "Basic " + b64encode(f"{USER}:{PASSWORD}".encode()).decode()

ODM_OPEN = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<ODM FileType="Snapshot" FileOID="mock" CreationDateTime="2026-01-01T00:00:00" '
    'ODMVersion="1.3" xmlns:mdsol="http://www.mdsol.com/ns/odm/metadata" '
    'xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.cdisc.org/ns/odm/v1.3">'
)

# Deliberately awkward: a UTF-8 BOM, comments, a CDATA section, entity
# references and a non-default namespace prefix for the ODM namespace itself.
STUDIES = (
    "﻿"
    + ODM_OPEN
    + "<!-- a comment -->"
    + '<Study OID="ACME &amp; Co(Prod)">'
    + "<GlobalVariables>"
    + "<StudyName><![CDATA[ACME & Co (Prod)]]></StudyName>"
    + "<StudyDescription>caf&#233; study</StudyDescription>"
    + "<ProtocolName>ACME &amp; Co</ProtocolName>"
    + "</GlobalVariables></Study>"
    + "</ODM>"
)

SUBJECTS = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<odm:ODM xmlns:odm="http://www.cdisc.org/ns/odm/v1.3" '
    'xmlns:m="http://www.mdsol.com/ns/odm/metadata" '
    'xmlns:xlink="http://www.w3.org/1999/xlink" ODMVersion="1.3" FileType="Snapshot">'
    '<odm:ClinicalData StudyOID="ACME &amp; Co(Prod)" MetaDataVersionOID="7">'
    '<odm:SubjectData SubjectKey="S-1" m:SubjectKeyType="SubjectName" m:SubjectActive="Yes" m:Deleted="No">'
    '<odm:SiteRef LocationOID="SITE-1" m:StudyEnvSiteNumber="001"/>'
    '<m:Link xlink:type="simple" xlink:href="https://example.invalid/s/1"/>'
    "</odm:SubjectData>"
    '<odm:SubjectData SubjectKey="S-2" m:SubjectKeyType="SubjectName">'
    '<odm:SiteRef LocationOID="SITE-2"/>'
    "</odm:SubjectData>"
    "</odm:ClinicalData></odm:ODM>"
)

DATASET = (
    ODM_OPEN
    + '<ClinicalData StudyOID="ACME &amp; Co(Prod)" MetaDataVersionOID="7">'
    + '<SubjectData SubjectKey="S-1"><SiteRef LocationOID="SITE-1"/>'
    + '<StudyEventData StudyEventOID="VISIT1" StudyEventRepeatKey="ALL[1]/V[1]">'
    + '<FormData FormOID="VITAL" FormRepeatKey="1">'
    + '<ItemGroupData ItemGroupOID="VITAL_LOG" ItemGroupRepeatKey="1">'
    + '<ItemData ItemOID="VITAL.WEIGHT" Value="80.5"/>'
    + '<ItemData ItemOID="VITAL.NOTE" Value="a &lt;b&gt; &amp; c"/>'
    + '<ItemData ItemOID="VITAL.EMPTY" Value=""/>'
    + '<ItemData ItemOID="VITAL.MISSING" IsNull="Yes"/>'
    + "</ItemGroupData></FormData></StudyEventData></SubjectData></ClinicalData>"
    + '<ClinicalData StudyOID="ACME &amp; Co(Prod)" MetaDataVersionOID="7">'
    + '<SubjectData SubjectKey="S-2"><SiteRef LocationOID="SITE-2"/>'
    + '<StudyEventData StudyEventOID="VISIT1">'
    + '<FormData FormOID="VITAL">'
    + '<ItemGroupData ItemGroupOID="VITAL_LOG">'
    + '<ItemData ItemOID="VITAL.WEIGHT" Value="72"/>'
    + '<ItemData ItemOID="VITAL.EXTRA" Value="only on S-2"/>'
    + "</ItemGroupData></FormData></StudyEventData></SubjectData></ClinicalData>"
    + "</ODM>"
)

BUSINESS_ERROR = (
    '<Response ReferenceNumber="x" InboundODMFileOID="N/A" IsTransactionSuccessful="0" '
    'ReasonCode="RWS00024" ErrorClientResponseMessage="You do not have access to this study."/>'
)

DOCTYPE_ATTACK = (
    '<?xml version="1.0"?>'
    '<!DOCTYPE ODM [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>'
    '<ODM xmlns="http://www.cdisc.org/ns/odm/v1.3"><Study OID="x"/></ODM>'
)

TRUNCATED = ODM_OPEN + '<ClinicalData StudyOID="x"><SubjectData SubjectKey="S-1">'

NOT_FOUND = BUSINESS_ERROR.replace("You do not have access to this study.", "RWS URL does not exist").replace(
    "RWS00024", "RWS00055"
)

KNOWN_STUDIES = {
    "ACME & Co(Prod)",
    "BUSERR(Prod)",
    "DOCTYPE(Prod)",
    "TRUNC(Prod)",
    "REDIR(Prod)",
    "FLAKY(Prod)",
}

flaky_hits = {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body, content_type="text/xml", headers=None):
        payload = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.headers.get("Authorization") != EXPECTED_AUTH:
            self.reply(
                401, BUSINESS_ERROR.replace("You do not have access to this study.", "Invalid user name or password.")
            )
            return

        base = "/RaveWebServices/"
        if not self.path.startswith(base):
            self.reply(404, "<html>not found</html>", "text/html")
            return
        route = self.path[len(base) :]

        if route == "version":
            self.reply(200, "1.16.0", "text/plain")
            return
        if route == "studies":
            self.reply(200, STUDIES)
            return
        if not route.startswith("studies/"):
            self.reply(404, NOT_FOUND)
            return

        # Everything below is scoped to a study. The study OID selects which
        # failure mode to exercise, so that each one is reached through the
        # ordinary code path rather than through a special parameter.
        rest = route[len("studies/") :]
        study, _, tail = rest.partition("/")
        study = unquote(study)
        tail = tail.split("?", 1)[0]

        if study not in KNOWN_STUDIES:
            self.reply(404, NOT_FOUND)
            return
        if tail == "subjects":
            self.reply(200, SUBJECTS)
            return
        if tail != "datasets/regular":
            self.reply(404, NOT_FOUND)
            return

        if study == "BUSERR(Prod)":
            # A business error delivered with a 200 status must still fail.
            self.reply(200, BUSINESS_ERROR)
        elif study == "DOCTYPE(Prod)":
            self.reply(200, DOCTYPE_ATTACK)
        elif study == "TRUNC(Prod)":
            self.reply(200, TRUNCATED)
        elif study == "REDIR(Prod)":
            self.send_response(302)
            self.send_header("Location", "https://evil.invalid/steal")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif study == "FLAKY(Prod)":
            hits = flaky_hits.get("n", 0) + 1
            flaky_hits["n"] = hits
            if hits < 3:
                self.reply(503, "<html>busy</html>", "text/html", {"Retry-After": "0"})
            else:
                self.reply(200, DATASET, headers={"X-MWS-CV-Last-Updated": "2026-01-01T00:00:00"})
        else:
            self.reply(200, DATASET, headers={"X-MWS-CV-Last-Updated": "2026-01-01T00:00:00"})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
