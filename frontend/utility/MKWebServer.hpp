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

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpServer>

class QTcpSocket;

/* Minimal static-file HTTP server bound to 127.0.0.1 only.
 * Serves the bundled MK web build from a FIXED origin (http://127.0.0.1:<port>)
 * so localStorage / sign-in persist between runs. Unknown extension-less paths
 * fall back to _shell.html (SPA routing). */
class MKWebServer : public QObject {
	Q_OBJECT

	QTcpServer server;
	QString root;
	QString rootCanonical;
	quint16 listenPort = 0;
	QByteArray configJson;

	void respond(QTcpSocket *sock, const QByteArray &request);
	void sendSimple(QTcpSocket *sock, int code, const char *text);

private slots:
	void onNewConnection();

public:
	explicit MKWebServer(const QString &rootDir, QObject *parent = nullptr);

	bool start(quint16 port);
	quint16 port() const { return listenPort; }

	/* Served at /mk-config.json to same-origin pages only: how the MK pages reach the built-in engine. */
	void setConfigJson(const QByteArray &json) { configJson = json; }
};
