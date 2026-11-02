/******************************************************************************
    MK VISION Studio - built-in MK panels (docks) hosted through obs-browser
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

#include "OBSBasic.hpp"

#ifdef BROWSER_AVAILABLE
#include <docks/BrowserDock.hpp>
#include <utility/MKWebServer.hpp>
#include <utility/platform.hpp>

#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QAction>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QNetworkInterface>
#include <QTimer>

#include <memory>

struct QCef;
extern QCef *cef;

namespace {

/* Fixed port => fixed origin => localStorage / sign-in persist between runs. */
constexpr quint16 MK_STUDIO_PORT = 17870;

struct MKTool {
	const char *id;
	const char *title;
	const char *route;
	int width;
	int height;
};

const MKTool MK_TOOLS[] = {
	{"switcher", "MK Switcher", "/", 1280, 760},
	{"studio", "MK Scenes & Sources", "/studio", 1100, 700},
	{"stream", "MK Stream & Record", "/streaming", 1100, 700},
	{"cam", "MK Cameras", "/cam", 1100, 700},
	{"graphics", "MK Graphics", "/graphics", 1280, 760},
	{"playout", "MK Playout", "/playout", 1100, 700},
	{"prompter", "MK Prompter", "/prompter", 900, 700},
	{"rundown", "MK Rundown", "/rundown", 1100, 700},
	{"sounds", "MK Sounds", "/sounds", 900, 600},
	{"audio", "MK VOXONE Audio", "/audio", 1280, 760},
	{"settings", "MK Settings", "/settings", 900, 700},
};

} // namespace

namespace {

/* Devices OBS can capture for this source kind (same list OBS Settings > Audio shows). */
QJsonArray ListAudioDevices(const char *sourceId)
{
	QJsonArray arr;
	obs_properties_t *props = obs_get_source_properties(sourceId);
	if (!props)
		return arr;
	obs_property_t *prop = obs_properties_get(props, "device_id");
	if (prop) {
		const size_t count = obs_property_list_item_count(prop);
		for (size_t i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;
			const char *name = obs_property_list_item_name(prop, i);
			const char *value = obs_property_list_item_string(prop, i);
			if (!name || !value)
				continue;
			QJsonObject d;
			d["name"] = QString::fromUtf8(name);
			d["id"] = QString::fromUtf8(value);
			arr.append(d);
		}
	}
	obs_properties_destroy(props);
	return arr;
}

bool CollectMonitorDevice(void *data, const char *name, const char *id)
{
	QJsonObject d;
	d["name"] = QString::fromUtf8(name ? name : "");
	d["id"] = QString::fromUtf8(id ? id : "");
	static_cast<QJsonArray *>(data)->append(d);
	return true;
}

QJsonArray ListMonitorDevices()
{
	QJsonArray arr;
	obs_enum_audio_monitoring_devices(CollectMonitorDevice, &arr);
	return arr;
}

bool HasDevice(const QJsonArray &list, const QString &id)
{
	for (const QJsonValue &v : list)
		if (v.toObject()["id"].toString() == id)
			return true;
	return false;
}

} // namespace

/* Native bridge for the MK pages. Audio first: OBS "global" audio channels 1-2 (desktop) and 3-6 (mic/aux).
 * These are always live, unlike inputs inside a scene, and are what OBS Settings > Audio edits. */
QByteArray OBSBasic::HandleMKApi(const QString &route, const QByteArray &body, int &status)
{
	status = 200;
	QJsonObject out;

	if (route == "audio-devices") {
		out["inputs"] = ListAudioDevices(App()->InputAudioSource());
		out["outputs"] = ListAudioDevices(App()->OutputAudioSource());
		QJsonArray channels;
		for (int ch = 1; ch <= 6; ch++) {
			QJsonObject c;
			c["channel"] = ch;
			c["kind"] = ch <= 2 ? "output" : "input";
			OBSSourceAutoRelease src = obs_get_output_source(ch);
			if (src) {
				OBSDataAutoRelease settings = obs_source_get_settings(src);
				c["name"] = QString::fromUtf8(obs_source_get_name(src));
				c["deviceId"] = QString::fromUtf8(obs_data_get_string(settings, "device_id"));
			}
			channels.append(c);
		}
		out["channels"] = channels;

	} else if (route == "monitor-devices") {
		/* Where Main / Solo pre-listen is heard (OBS Settings > Audio > Monitoring Device). */
		out["devices"] = ListMonitorDevices();
		const char *curName = nullptr;
		const char *curId = nullptr;
		obs_get_audio_monitoring_device(&curName, &curId);
		out["currentId"] = QString::fromUtf8(curId ? curId : "default");
		out["currentName"] = QString::fromUtf8(curName ? curName : "");

	} else if (route == "monitor-device") {
		const QJsonObject in = QJsonDocument::fromJson(body).object();
		const QString id = in["deviceId"].toString();
		const QJsonArray known = ListMonitorDevices();
		QString name;
		for (const QJsonValue &v : known)
			if (v.toObject()["id"].toString() == id)
				name = v.toObject()["name"].toString();

		if (id.isEmpty() || name.isEmpty()) {
			status = 400;
			out["error"] = "unknown device";
		} else {
			const QByteArray idUtf8 = id.toUtf8();
			const QByteArray nameUtf8 = name.toUtf8();
			if (obs_set_audio_monitoring_device(nameUtf8.constData(), idUtf8.constData())) {
				config_set_string(activeConfiguration, "Audio", "MonitoringDeviceName", nameUtf8.constData());
				config_set_string(activeConfiguration, "Audio", "MonitoringDeviceId", idUtf8.constData());
				config_save_safe(activeConfiguration, "tmp", nullptr);
				blog(LOG_INFO, "[MK VISION Studio] Monitoring device set to '%s'", nameUtf8.constData());
				out["ok"] = true;
			} else {
				status = 400;
				out["error"] = "the engine could not use that device";
			}
		}

	} else if (route == "audio-device") {
		const QJsonObject in = QJsonDocument::fromJson(body).object();
		const int ch = in["channel"].toInt();
		const QString id = in["deviceId"].toString();
		const QString label = in["label"].toString();
		const bool isOutput = ch <= 2;

		const QJsonArray known = ListAudioDevices(isOutput ? App()->OutputAudioSource() : App()->InputAudioSource());
		if (ch < 1 || ch > 6 || id.isEmpty() || (id != "disabled" && !HasDevice(known, id))) {
			status = 400;
			out["error"] = "unknown channel or device";
		} else {
			const QByteArray idUtf8 = id.toUtf8();
			const QByteArray labelUtf8 = (label.isEmpty() ? QString("USB audio") : label).toUtf8();
			ResetAudioDevice(isOutput ? App()->OutputAudioSource() : App()->InputAudioSource(), idUtf8.constData(),
					 labelUtf8.constData(), ch);
			SaveProject();
			blog(LOG_INFO, "[MK VISION Studio] Audio channel %d set to device '%s'", ch, idUtf8.constData());
			out["ok"] = true;
		}

	} else {
		status = 404;
		out["error"] = "unknown route";
	}

	return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

void OBSBasic::InitMKStudio()
{
	/* Not an OBS shell: no links to obsproject.com, Discord, updater or release notes. */
	for (QAction *a : {ui->actionHelpPortal, ui->actionWebsite, ui->actionDiscord, ui->actionCheckForUpdates,
			   ui->actionRepair, ui->actionShowWhatsNew, ui->actionReleaseNotes}) {
		if (a)
			a->setVisible(false);
	}

	if (!cef)
		return;

	/* obs_find_data_file() only searches data/libobs; our bundle lives in data/obs-studio. */
	std::string shellPath;
	if (!GetDataFilePath("mk-studio/web/_shell.html", shellPath)) {
		blog(LOG_WARNING, "[MK VISION Studio] Bundled web build not found (data/obs-studio/mk-studio/web); "
				  "MK docks disabled");
		return;
	}

	const QString root = QFileInfo(QString::fromUtf8(shellPath.c_str())).absolutePath();
	mkWebServer = new MKWebServer(root, this);
	const bool allowCrew = config_get_bool(App()->GetUserConfig(), "MKStudio", "AllowCrew");
	mkWebServer->setAllowLan(allowCrew);
	if (!mkWebServer->start(MK_STUDIO_PORT)) {
		blog(LOG_ERROR, "[MK VISION Studio] Could not listen on 127.0.0.1:%d (port in use?); MK docks disabled",
		     (int)MK_STUDIO_PORT);
		delete mkWebServer;
		mkWebServer = nullptr;
		return;
	}
	mkWebServer->setApiHandler(
		[this](const QString &route, const QByteArray &body, int &status) { return HandleMKApi(route, body, status); });
	blog(LOG_INFO, "[MK VISION Studio] Serving %s on http://127.0.0.1:%d", QT_TO_UTF8(root), (int)MK_STUDIO_PORT);

	static int panel_version = -1;
	if (panel_version == -1)
		panel_version = obs_browser_qcef_version();

	ui->viewMenu->addSeparator();
	QMenu *menu = ui->viewMenu->addMenu(QString::fromUtf8("MK VISION"));

	QAction *deskAction = menu->addAction(QString::fromUtf8("MK VISION Desk (full layout)"));
	connect(deskAction, &QAction::triggered, this, [this]() { ApplyMKLayout(true); });
	QAction *classicAction = menu->addAction(QString::fromUtf8("Classic OBS Layout"));
	connect(classicAction, &QAction::triggered, this, [this]() { ApplyMKLayout(false); });
	QAction *obsSettings = menu->addAction(QString::fromUtf8("OBS Settings (advanced)..."));
	connect(obsSettings, &QAction::triggered, this, [this]() { on_action_Settings_triggered(); });
	QAction *crewAction = menu->addAction(QString::fromUtf8("Allow crew on this network (restart to apply)"));
	crewAction->setCheckable(true);
	crewAction->setChecked(allowCrew);
	connect(crewAction, &QAction::toggled, this, [](bool on) {
		config_set_bool(App()->GetUserConfig(), "MKStudio", "AllowCrew", on);
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	});
	menu->addSeparator();

	setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);

	/* Zero setup: tell the MK pages how to reach the built-in engine (obs-websocket on this PC). */
	{
		config_t *user = App()->GetUserConfig();
		config_t *app = App()->GetAppConfig();
		const char *sec = "OBSWebSocket";
		config_t *cfg = config_has_user_value(user, sec, "ServerPort") ? user : app;

		const bool enabled = config_get_bool(cfg, sec, "ServerEnabled");
		int wsPort = (int)config_get_uint(cfg, sec, "ServerPort");
		if (wsPort <= 0 || wsPort > 65535)
			wsPort = 4455;
		const char *pw = config_get_string(cfg, sec, "ServerPassword");
		const bool auth = config_get_bool(cfg, sec, "AuthRequired");

		QJsonObject o;
		o["mk"] = 1;
		o["host"] = "127.0.0.1";
		o["port"] = wsPort;
		o["password"] = (auth && pw) ? QString::fromUtf8(pw) : QString();
		o["engineEnabled"] = enabled;
		o["token"] = QString::fromUtf8(mkWebServer->token());
		o["crew"] = allowCrew;
		o["crewPort"] = (int)mkWebServer->port();
		QJsonArray lan;
		if (allowCrew) {
			for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
				const QString ip = a.toString();
				if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !ip.startsWith("169.254."))
					lan.append(ip);
			}
		}
		o["lan"] = lan;
		mkWebServer->setConfigJson(QJsonDocument(o).toJson(QJsonDocument::Compact));

		if (!enabled) {
			/* obs-websocket starts before this runs, so it comes on at the next launch. */
			config_set_bool(cfg, sec, "ServerEnabled", true);
			config_save_safe(cfg, "tmp", nullptr);
			blog(LOG_WARNING, "[MK VISION Studio] Built-in engine connection was off; enabled for next start");
		}
	}

	const QString base = QString("http://127.0.0.1:%1").arg(mkWebServer->port());

	for (const MKTool &tool : MK_TOOLS) {
		const QString title = QString::fromUtf8(tool.title);
		const QString url = base + QString::fromUtf8(tool.route);

		BrowserDock *dock = new BrowserDock(title);
		dock->setObjectName(QString("mkstudio_%1_dock").arg(QString::fromUtf8(tool.id)));
		dock->resize(tool.width, tool.height);
		dock->setMinimumSize(160, 120);
		dock->setWindowTitle(title);
		dock->setAllowedAreas(Qt::AllDockWidgetAreas);

		/* Created blank; the real page loads the first time the dock is shown. */
		QCefWidget *browser = cef->create_widget(dock, "about:blank", nullptr);
		if (browser && panel_version >= 1)
			browser->allowAllPopups(true);
		dock->SetWidget(browser);

		AddMKDockWidget(dock, Qt::RightDockWidgetArea);
		/* All MK pages share one tab group; the first one (Switcher) is the anchor. */
		if (!mkStudioDocks.isEmpty())
			tabifyDockWidget(mkStudioDocks.first().get(), dock);
		dock->hide();

		/* Menu entry = "go to this page": show it and bring its tab to the front. */
		QAction *open = menu->addAction(QString(title).replace("&", "&&"));
		connect(open, &QAction::triggered, dock, [this, dock]() {
			if (!property("mkMode").toBool())
				dock->setFloating(true);
			dock->show();
			dock->raise();
		});

		auto loaded = std::make_shared<bool>(false);
		connect(dock, &QDockWidget::visibilityChanged, dock, [browser, url, loaded](bool visible) {
			if (!visible || !browser || *loaded)
				return;
			*loaded = true;
			browser->setURL(QT_TO_UTF8(url));
		});

		mkStudioDocks.push_back(std::shared_ptr<QDockWidget>(dock));
	}

	/* Default: MK VISION is the UI. Runs after OBS restores its saved dock state. */
	const bool classic = config_get_bool(App()->GetUserConfig(), "MKStudio", "ClassicLayout");
	if (!classic)
		QTimer::singleShot(400, this, [this]() { ApplyMKLayout(true); });
}

void OBSBasic::ApplyMKLayout(bool mkMode)
{
	if (mkStudioDocks.isEmpty())
		return;

	setProperty("mkMode", mkMode);
	config_set_bool(App()->GetUserConfig(), "MKStudio", "ClassicLayout", !mkMode);
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);

	QList<QDockWidget *> natives{ui->scenesDock, ui->sourcesDock, ui->mixerDock, ui->transitionsDock};
	if (controlsDock)
		natives.push_back(controlsDock);

	if (mkMode) {
		for (QDockWidget *d : natives)
			d->hide();
		if (statsDock)
			statsDock->hide();
		centralWidget()->hide();

		/* Re-dock every MK page into one tab group on the right, Switcher in front. */
		QDockWidget *first = mkStudioDocks.first().get();
		for (auto &d : mkStudioDocks) {
			removeDockWidget(d.get());
			addDockWidget(Qt::RightDockWidgetArea, d.get());
			d->setFloating(false);
			if (d.get() != first)
				tabifyDockWidget(first, d.get());
			d->show();
		}
		first->raise();
	} else {
		for (auto &d : mkStudioDocks)
			d->hide();
		centralWidget()->show();
		for (QDockWidget *d : natives)
			d->show();
	}
}

void OBSBasic::ClearMKStudio()
{
	mkStudioDocks.clear();
	if (mkWebServer) {
		delete mkWebServer;
		mkWebServer = nullptr;
	}
}
#endif
