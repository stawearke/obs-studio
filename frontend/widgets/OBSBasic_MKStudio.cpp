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
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
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
	{"onair", "MK OnAir Studio", "/onair", 1280, 760},
	{"settings", "MK Settings", "/settings", 900, 700},
};

} // namespace

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
	if (!mkWebServer->start(MK_STUDIO_PORT)) {
		blog(LOG_ERROR, "[MK VISION Studio] Could not listen on 127.0.0.1:%d (port in use?); MK docks disabled",
		     (int)MK_STUDIO_PORT);
		delete mkWebServer;
		mkWebServer = nullptr;
		return;
	}
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
