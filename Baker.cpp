#include "Baker.h"
#include "BakerWindow.h"

#include <editor/UnigineConstants.h>
#include <editor/UnigineWindowManager.h>

#include <UnigineLog.h>

#include <QMenu>

using ::UnigineEditor::WindowManager;

Baker::~Baker() = default;

bool Baker::init()
{
	Unigine::Log::message("Baker plugin: init\n");

	window_ = new BakerWindow();
	WindowManager::add(window_, WindowManager::ROOT_AREA_RIGHT);

	QMenu *menu = WindowManager::findMenu(::UnigineEditor::Constants::MM_WINDOWS);
	if (menu)
		action_ = menu->addAction("Texture Baker", this, &Baker::showWindow);

	return true;
}

void Baker::shutdown()
{
	Unigine::Log::message("Baker plugin: shutdown\n");

	if (action_)
	{
		QMenu *menu = WindowManager::findMenu(::UnigineEditor::Constants::MM_WINDOWS);
		if (menu)
			menu->removeAction(action_);
		action_ = nullptr;
	}

	if (window_)
	{
		WindowManager::remove(window_);
		delete window_;
		window_ = nullptr;
	}
}

void Baker::showWindow()
{
	if (window_)
		WindowManager::show(window_);
}
