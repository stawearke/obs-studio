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
#include <QMenu>

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
	{"switcher", "MK Switcher", "/", 1280, 760},   {"cam", "MK Cameras", "/cam", 1100, 700},
	{"graphics", "MK Graphics", "/graphics", 1280, 760}, {"playout", "MK Playout", "/playout", 1100, 700},
	{"prompter", "MK Prompter", "/prompter", 900, 700}, {"rundown", "MK Rundown", "/rundown", 1100, 700},
	{"sounds", "MK Sounds", "/sounds", 900, 600},  {"audio", "MK VOXONE Audio", "/audio", 1280, 760},
	{"onair", "MK OnAir Studio", "/onair", 1280, 760}, {"settings", "MK Settings", "/settings", 900, 700},
};

} // namespace

void OBSBasic::InitMKStudio()
{
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
		dock->setFloating(true);
		dock->hide();
		menu->addAction(dock->toggleViewAction());

		auto loaded = std::make_shared<bool>(false);
		connect(dock, &QDockWidget::visibilityChanged, dock, [browser, url, loaded](bool visible) {
			if (!visible || !browser || *loaded)
				return;
			*loaded = true;
			browser->setURL(QT_TO_UTF8(url));
		});

		mkStudioDocks.push_back(std::shared_ptr<QDockWidget>(dock));
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
