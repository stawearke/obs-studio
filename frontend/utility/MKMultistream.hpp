/******************************************************************************
    MK VISION Studio - extra stream destinations (multistream)
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

#include <obs-frontend-api.h>
#include <obs.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

/* Sends the main stream to more sites. Every extra destination is its own RTMP(S) output that SHARES the
 * main stream's encoders: no extra CPU, only extra upload bandwidth. Destinations follow the main stream
 * (they start when it starts and stop when it stops) and can also be started / stopped one by one while
 * the main stream is live. */
class MKMultistream {
public:
	struct Target {
		QString id;
		QString name;
		QString server;
		QString key;
		bool enabled = true;
	};

	MKMultistream();
	~MKMultistream();

	/* Saved form: base64 of a JSON array (kept in the user config). */
	void load(const QString &stored);
	QString serialize() const;

	/* Listen to the main stream starting / stopping. */
	void attach();
	void detach();

	/* Targets + live state for the MK page. Keys are never sent back, only whether one is set. */
	QJsonObject list();
	/* Replaces the target list. An empty key keeps the key already stored for that id. */
	bool save(const QJsonArray &incoming, QString &error);
	/* id empty = every enabled destination. */
	bool start(const QString &id, QString &error);
	/* id empty = all. */
	void stop(const QString &id);
	/* Stop everything now (shutdown). */
	void shutdown();

	static constexpr int MaxTargets = 8;

private:
	struct Run {
		obs_output_t *output = nullptr;
		obs_service_t *service = nullptr;
		bool wanted = false;
		quint64 lastBytes = 0;
		qint64 lastMs = 0;
		double kbps = 0.0;
		qint64 startedMs = 0;
		QString error;
	};

	static void OnFrontendEvent(enum obs_frontend_event event, void *data);
	static bool ValidServer(const QString &server);

	bool startOne(const Target &t, QString &error);
	void release(Run &run);
	void reap();

	QVector<Target> targets;
	QMap<QString, Run> runs;
	QElapsedTimer clock;
	bool attached = false;
};
