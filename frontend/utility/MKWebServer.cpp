/******************************************************************************
    MK VISION Studio - local web server for the bundled MK web build
    Copyright (C) 2026 MK VISION

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
******************************************************************************/

#include "MKWebServer.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QUrl>

#include <memory>

#include "moc_MKWebServer.cpp"

namespace {

struct ConnState {
	QByteArray data;
	bool done = false;
};

const char *MimeFor(const QString &suffix)
{
	const QString s = suffix.toLower();
	if (s == "html" || s == "htm")
		return "text/html; charset=utf-8";
	if (s == "js" || s == "mjs")
		return "text/javascript; charset=utf-8";
	if (s == "css")
		return "text/css; charset=utf-8";
	if (s == "json" || s == "map")
		return "application/json; charset=utf-8";
	if (s == "svg")
		return "image/svg+xml";
	if (s == "png")
		return "image/png";
	if (s == "jpg" || s == "jpeg")
		return "image/jpeg";
	if (s == "gif")
		return "image/gif";
	if (s == "webp")
		return "image/webp";
	if (s == "ico")
		return "image/x-icon";
	if (s == "woff2")
		return "font/woff2";
	if (s == "woff")
		return "font/woff";
	if (s == "ttf")
		return "font/ttf";
	if (s == "wasm")
		return "application/wasm";
	if (s == "mp3")
		return "audio/mpeg";
	if (s == "wav")
		return "audio/wav";
	if (s == "mp4")
		return "video/mp4";
	if (s == "webm")
		return "video/webm";
	if (s == "txt" || s == "md" || s == "yml" || s == "yaml")
		return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

} // namespace

MKWebServer::MKWebServer(const QString &rootDir, QObject *parent)
	: QObject(parent),
	  server(this),
	  root(rootDir)
{
	rootCanonical = QFileInfo(root).canonicalFilePath();
	connect(&server, &QTcpServer::newConnection, this, &MKWebServer::onNewConnection);
}

bool MKWebServer::start(quint16 port)
{
	if (rootCanonical.isEmpty())
		return false;
	if (!server.listen(allowLan ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(QHostAddress::LocalHost), port))
		return false;
	listenPort = server.serverPort();
	return true;
}

void MKWebServer::onNewConnection()
{
	while (QTcpSocket *sock = server.nextPendingConnection()) {
		auto state = std::make_shared<ConnState>();
		connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
		connect(sock, &QTcpSocket::readyRead, this, [this, sock, state]() {
			if (state->done)
				return;
			state->data.append(sock->readAll());
			if (state->data.size() > 16384) {
				state->done = true;
				sock->abort();
				return;
			}
			if (!state->data.contains("\r\n\r\n"))
				return;
			state->done = true;
			respond(sock, state->data);
		});
	}
}

void MKWebServer::sendSimple(QTcpSocket *sock, int code, const char *text)
{
	QByteArray body = QByteArray(text) + "\n";
	QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + text + "\r\n";
	head += "Content-Type: text/plain; charset=utf-8\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += "Connection: close\r\n\r\n";
	sock->write(head + body);
	sock->disconnectFromHost();
}

void MKWebServer::respond(QTcpSocket *sock, const QByteArray &request)
{
	const int lineEnd = request.indexOf("\r\n");
	const QList<QByteArray> parts = request.left(lineEnd).split(' ');
	if (parts.size() < 2) {
		sendSimple(sock, 400, "Bad Request");
		return;
	}

	const QByteArray method = parts[0];
	const bool isHead = (method == "HEAD");
	if (method != "GET" && !isHead) {
		sendSimple(sock, 405, "Method Not Allowed");
		return;
	}

	/* Host check (DNS-rebinding protection): only our own origin */
	QByteArray host;
	for (const QByteArray &line : request.left(request.indexOf("\r\n\r\n")).split('\n')) {
		if (line.toLower().startsWith("host:")) {
			host = line.mid(5).trimmed().toLower();
			break;
		}
	}
	const QByteArray p = QByteArray::number(listenPort);
	bool hostOk = (host == "127.0.0.1:" + p || host == "localhost:" + p);
	if (!hostOk && allowLan) {
		/* Crew on this network arrive by IP address. Names are refused (DNS rebinding). */
		const int colon = host.lastIndexOf(':');
		if (colon > 0 && host.mid(colon + 1) == p) {
			QHostAddress addr;
			hostOk = addr.setAddress(QString::fromLatin1(host.left(colon))) &&
				 addr.protocol() == QAbstractSocket::IPv4Protocol;
		}
	}
	if (!hostOk) {
		sendSimple(sock, 403, "Forbidden");
		return;
	}

	/* Decode + sanitize path */
	QByteArray rawPath = parts[1];
	const int q = rawPath.indexOf('?');
	if (q >= 0)
		rawPath.truncate(q);
	const int h = rawPath.indexOf('#');
	if (h >= 0)
		rawPath.truncate(h);
	const QString path = QUrl::fromPercentEncoding(rawPath);
	if (path.contains("..") || path.contains('\\') || path.contains(QChar(0)) || !path.startsWith('/')) {
		sendSimple(sock, 400, "Bad Request");
		return;
	}

	if (path == "/mk-config.json") {
		/* The engine password is only ever given to this PC, never to crew on the network. */
		if (!sock->peerAddress().isLoopback()) {
			sendSimple(sock, 404, "Not Found");
			return;
		}
		if (configJson.isEmpty()) {
			sendSimple(sock, 404, "Not Found");
			return;
		}
		/* Same-origin only: a page from another site sends Origin / Sec-Fetch-Site headers. */
		QByteArray origin, fetchSite;
		for (const QByteArray &line : request.left(request.indexOf("\r\n\r\n")).split('\n')) {
			const QByteArray l = line.trimmed().toLower();
			if (l.startsWith("origin:"))
				origin = l.mid(7).trimmed();
			else if (l.startsWith("sec-fetch-site:"))
				fetchSite = l.mid(15).trimmed();
		}
		const bool originOk = origin.isEmpty() || origin == "http://127.0.0.1:" + p ||
				      origin == "http://localhost:" + p;
		const bool siteOk = fetchSite.isEmpty() || fetchSite == "same-origin" || fetchSite == "none";
		if (!originOk || !siteOk) {
			sendSimple(sock, 403, "Forbidden");
			return;
		}
		QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n";
		head += "Content-Length: " + QByteArray::number(configJson.size()) + "\r\n";
		head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
		sock->write(head);
		if (!isHead)
			sock->write(configJson);
		sock->disconnectFromHost();
		return;
	}

	const QString rel = path.mid(1);
	QString filePath = rootCanonical + "/" + rel;
	QFileInfo info(filePath);
	bool fallback = false;

	if (rel.isEmpty() || !info.isFile()) {
		const QString last = rel.section('/', -1);
		if (last.contains('.')) {
			sendSimple(sock, 404, "Not Found");
			return;
		}
		filePath = rootCanonical + "/_shell.html";
		info.setFile(filePath);
		fallback = true;
	}

	const QString canon = info.canonicalFilePath();
	if (canon.isEmpty() || !canon.startsWith(rootCanonical + "/")) {
		sendSimple(sock, 404, "Not Found");
		return;
	}

	QFile file(canon);
	if (!file.open(QIODevice::ReadOnly)) {
		sendSimple(sock, 404, "Not Found");
		return;
	}
	const QByteArray body = file.readAll();

	const bool isHashedAsset = rel.startsWith("assets/");
	QByteArray head = "HTTP/1.1 200 OK\r\n";
	head += "Content-Type: " + QByteArray(MimeFor(fallback ? QString("html") : info.suffix())) + "\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += isHashedAsset ? "Cache-Control: public, max-age=31536000, immutable\r\n" : "Cache-Control: no-cache\r\n";
	head += "X-Content-Type-Options: nosniff\r\n";
	head += "Connection: close\r\n\r\n";

	sock->write(head);
	if (!isHead)
		sock->write(body);
	sock->disconnectFromHost();
}
