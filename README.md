libwsclient
===========

WebSocket client library for C

## IMPORTANT NOTE

Although this library supports secure websockets, for simplicity the code that verifies the server's response to **Sec-WebSocket-Key** was removed. The library works exactly the same but it is **not recommended to send or receive sensitive information**.

This library abstracts away WebSocket protocol framing for
client connections.  It aims to provide a *somewhat* similar
API to the implementation in your browser.  You create a new
client context and create callbacks to be triggered when
certain events occur (onopen, onmessage, onclose, onerror).

Your best bet for getting started is to look at test.c which shows
how to connect to an echo server using libwsclient calls.

Also, to build:
gcc wsclient.c base64.c sha1.c test.c -lssl -lcrypto -lpthread -o test

