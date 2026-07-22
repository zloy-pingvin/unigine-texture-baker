#pragma once

#include <editor/UniginePlugin.h>

#include <QObject>

class BakerWindow;
class QAction;

class Baker : public QObject, public ::UnigineEditor::Plugin
{
	Q_OBJECT
	Q_DISABLE_COPY(Baker)
	Q_PLUGIN_METADATA(IID UNIGINE_EDITOR_PLUGIN_IID FILE "Baker.json")
	Q_INTERFACES(UnigineEditor::Plugin)
public:
	Baker() = default;
	~Baker() override;

	bool init() override;
	void shutdown() override;

private slots:
	void showWindow();

private:
	BakerWindow *window_ = nullptr;
	QAction *action_ = nullptr;
};
