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

#include "MKMultistream.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/base.h>

#include <QJsonDocument>
#include <QUuid>

#include <algorithm>

MKMultistream::MKMultistream()
{
	clock.start();
}

MKMultistream::~MKMultistream()
{
	detach();
	shutdown();
}

bool MKMultistream::ValidServer(const QString &server)
{
	const QString s = server.trimmed().toLower();
	return (s.startsWith("rtmp://") || s.startsWith("rtmps://")) && s.size() > 8 && s.size() < 600;
}

void MKMultistream::load(const QString &stored)
{
	targets.clear();
	if (stored.isEmpty())
		return;
	const QJsonArray arr = QJsonDocument::fromJson(QByteArray::fromBase64(stored.toUtf8())).array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		Target t;
		t.id = o["id"].toString();
		t.name = o["name"].toString();
		t.server = o["server"].toString();
		t.key = o["key"].toString();
		t.enabled = o["enabled"].toBool(true);
		if (!t.id.isEmpty() && ValidServer(t.server) && targets.size() < MaxTargets)
			targets.push_back(t);
	}
}

QString MKMultistream::serialize() const
{
	QJsonArray arr;
	for (const Target &t : targets) {
		QJsonObject o;
		o["id"] = t.id;
		o["name"] = t.name;
		o["server"] = t.server;
		o["key"] = t.key;
		o["enabled"] = t.enabled;
		arr.append(o);
	}
	return QString::fromLatin1(QJsonDocument(arr).toJson(QJsonDocument::Compact).toBase64());
}

void MKMultistream::attach()
{
	if (attached)
		return;
	obs_frontend_add_event_callback(&MKMultistream::OnFrontendEvent, this);
	attached = true;
}

void MKMultistream::detach()
{
	if (!attached)
		return;
	obs_frontend_remove_event_callback(&MKMultistream::OnFrontendEvent, this);
	attached = false;
}

void MKMultistream::OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<MKMultistream *>(data);
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		QString error;
		self->start(QString(), error);
		if (!error.isEmpty())
			blog(LOG_WARNING, "[MK VISION Studio] Multistream: %s", error.toUtf8().constData());
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_EXIT) {
		self->stop(QString());
	}
}

void MKMultistream::release(Run &run)
{
	if (run.output) {
		if (obs_output_active(run.output))
			obs_output_force_stop(run.output);
		obs_output_release(run.output);
		run.output = nullptr;
	}
	if (run.service) {
		obs_service_release(run.service);
		run.service = nullptr;
	}
	run.wanted = false;
	run.kbps = 0.0;
	run.lastBytes = 0;
}

/* Drop outputs that were asked to stop and have finished stopping. */
void MKMultistream::reap()
{
	for (auto it = runs.begin(); it != runs.end(); ++it) {
		Run &r = it.value();
		if (r.output && !r.wanted && !obs_output_active(r.output) && !obs_output_reconnecting(r.output))
			release(r);
	}
}

bool MKMultistream::startOne(const Target &t, QString &error)
{
	OBSOutputAutoRelease main = obs_frontend_get_streaming_output();
	if (!main || !obs_output_active(main)) {
		error = "Start the main stream first. Extra destinations share its video and audio.";
		return false;
	}
	/* Borrowed pointers: the main stream output owns these encoders. */
	obs_encoder_t *video = obs_output_get_video_encoder(main);
	obs_encoder_t *audio = obs_output_get_audio_encoder(main, 0);
	if (!video || !audio) {
		error = "The main stream has no encoder yet.";
		return false;
	}

	Run &run = runs[t.id];
	if (run.output && obs_output_active(run.output)) {
		run.wanted = true;
		return true;
	}
	release(run);

	const QByteArray name = ("MK extra " + t.name).toUtf8();
	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", t.server.toUtf8().constData());
	obs_data_set_string(serviceSettings, "key", t.key.toUtf8().constData());
	obs_data_set_bool(serviceSettings, "use_auth", false);

	run.service = obs_service_create("rtmp_custom", name.constData(), serviceSettings, nullptr);
	run.output = obs_output_create("rtmp_output", name.constData(), nullptr, nullptr);
	if (!run.service || !run.output) {
		release(run);
		error = "OBS could not create the output for " + t.name + ".";
		return false;
	}

	obs_output_set_service(run.output, run.service);
	obs_output_set_video_encoder(run.output, video);
	obs_output_set_audio_encoder(run.output, audio, 0);
	obs_output_set_reconnect_settings(run.output, 20, 2);

	if (!obs_output_start(run.output)) {
		const char *why = obs_output_get_last_error(run.output);
		error = QString("%1: %2").arg(t.name, why && *why ? QString::fromUtf8(why) : QString("could not start"));
		run.error = error;
		release(run);
		return false;
	}

	run.wanted = true;
	run.error.clear();
	run.startedMs = clock.elapsed();
	run.lastMs = clock.elapsed();
	run.lastBytes = 0;
	blog(LOG_INFO, "[MK VISION Studio] Multistream: started '%s'", t.name.toUtf8().constData());
	return true;
}

bool MKMultistream::start(const QString &id, QString &error)
{
	bool any = false;
	for (const Target &t : targets) {
		if (!id.isEmpty() && t.id != id)
			continue;
		if (id.isEmpty() && !t.enabled)
			continue;
		any = true;
		QString one;
		if (!startOne(t, one) && error.isEmpty())
			error = one;
	}
	if (!any && error.isEmpty())
		error = id.isEmpty() ? "No destination is switched on." : "That destination does not exist.";
	return error.isEmpty();
}

void MKMultistream::stop(const QString &id)
{
	for (auto it = runs.begin(); it != runs.end(); ++it) {
		if (!id.isEmpty() && it.key() != id)
			continue;
		Run &r = it.value();
		r.wanted = false;
		if (r.output && obs_output_active(r.output))
			obs_output_stop(r.output);
	}
	reap();
}

void MKMultistream::shutdown()
{
	for (auto it = runs.begin(); it != runs.end(); ++it)
		release(it.value());
	runs.clear();
}

bool MKMultistream::save(const QJsonArray &incoming, QString &error)
{
	if (incoming.size() > MaxTargets) {
		error = QString("At most %1 destinations.").arg(MaxTargets);
		return false;
	}

	QVector<Target> next;
	for (const QJsonValue &v : incoming) {
		const QJsonObject o = v.toObject();
		Target t;
		t.id = o["id"].toString();
		t.name = o["name"].toString().trimmed().left(40);
		t.server = o["server"].toString().trimmed();
		t.key = o["key"].toString().trimmed();
		t.enabled = o["enabled"].toBool(true);

		if (t.name.isEmpty()) {
			error = "Every destination needs a name.";
			return false;
		}
		if (!ValidServer(t.server)) {
			error = QString("%1: the server must start with rtmp:// or rtmps://").arg(t.name);
			return false;
		}
		const Target *old = nullptr;
		for (const Target &x : targets)
			if (!t.id.isEmpty() && x.id == t.id)
				old = &x;
		if (t.key.isEmpty() && old)
			t.key = old->key;
		if (t.key.isEmpty()) {
			error = QString("%1: enter the stream key.").arg(t.name);
			return false;
		}
		if (t.id.isEmpty())
			t.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
		next.push_back(t);
	}

	/* A destination that was removed, or whose address changed, must not keep sending to the old one. */
	for (const Target &x : targets) {
		bool kept = false;
		for (const Target &n : next)
			if (n.id == x.id && n.server == x.server && n.key == x.key)
				kept = true;
		if (!kept)
			stop(x.id);
	}
	targets = next;
	return true;
}

QJsonObject MKMultistream::list()
{
	reap();
	OBSOutputAutoRelease main = obs_frontend_get_streaming_output();
	const bool mainLive = main && obs_output_active(main);

	const qint64 now = clock.elapsed();
	QJsonArray arr;
	for (const Target &t : targets) {
		QJsonObject o;
		o["id"] = t.id;
		o["name"] = t.name;
		o["server"] = t.server;
		o["hasKey"] = !t.key.isEmpty();
		o["keyHint"] = t.key.size() > 4 ? QString("…") + t.key.right(4) : QString();
		o["enabled"] = t.enabled;

		QString state = "idle";
		double dropped = 0.0, congestion = 0.0;
		qint64 seconds = 0;
		QString error;

		auto it = runs.find(t.id);
		if (it != runs.end() && it->output) {
			Run &r = it.value();
			const bool active = obs_output_active(r.output);
			if (obs_output_reconnecting(r.output)) {
				state = "reconnecting";
			} else if (active) {
				const quint64 bytes = obs_output_get_total_bytes(r.output);
				state = bytes == 0 ? "connecting" : "live";
				if (now - r.lastMs >= 500) {
					r.kbps = bytes >= r.lastBytes ? double(bytes - r.lastBytes) * 8.0 / double(now - r.lastMs) : 0.0;
					r.lastBytes = bytes;
					r.lastMs = now;
				}
				const int frames = obs_output_get_total_frames(r.output);
				dropped = frames > 0 ? 100.0 * obs_output_get_frames_dropped(r.output) / frames : 0.0;
				congestion = double(obs_output_get_congestion(r.output));
				seconds = (now - r.startedMs) / 1000;
			} else if (r.wanted) {
				const char *why = obs_output_get_last_error(r.output);
				state = "failed";
				r.error = why && *why ? QString::fromUtf8(why) : QString("The connection stopped.");
			}
			error = r.error;
			o["kbps"] = state == "live" ? r.kbps : 0.0;
		} else {
			o["kbps"] = 0.0;
			if (it != runs.end())
				error = it->error;
		}
		o["state"] = state;
		o["dropped"] = dropped;
		o["congestion"] = congestion;
		o["seconds"] = double(seconds);
		o["error"] = error;
		arr.append(o);
	}

	QJsonObject out;
	out["mainLive"] = mainLive;
	out["targets"] = arr;
	out["max"] = MaxTargets;
	return out;
}
