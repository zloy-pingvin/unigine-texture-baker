#pragma once

#include "BakeCore.h"

#include <UnigineEvent.h>

#include <QPointer>
#include <QWidget>

#include <chrono>
#include <vector>

class QTimer;
class QToolButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMimeData;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;
class QWidget;

class BakerWindow : public QWidget
{
	Q_OBJECT
	Q_DISABLE_COPY(BakerWindow)
public:
	explicit BakerWindow(QWidget *parent = nullptr);
	~BakerWindow() override;

protected:
	// drop target for nodes dragged from the World Nodes hierarchy
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void pickHighFromSelection();
	void pickLowFromSelection();
	void startBake();
	void cancelBake();
	void onCaptureTick();
	void onCreateSkewMask();

private:
	int grabSelectedMeshGroupId(QLabel *label);
	void addSlotNodes(bool high, const std::vector<int> &rootIds);
	void clearSlot(bool high);
	void updateSlotLabels();
	void setSlotHidden(bool high, bool hidden);
	std::vector<int> decodeNodeDrop(const QMimeData *mime) const;
	void refreshMeshTree();
	void updateSuggestedName();
	void addGroupRow();
	void removeGroupRow(QGroupBox *box);
	void renumberGroups();
	void updateModelsMode();
	int firstLowId() const;
	void setUiLocked(bool locked);
	// UI language follows the editor translation settings: tr2 = labels and
	// status texts, tip2 = tooltips (translated even in "tooltips only" mode)
	QString tr2(const char *ru, const char *en) const;
	QString tip2(const char *ru, const char *en) const;
	void saveSettings() const;
	void restoreSettings();
	void runBake();
	void abortCapture(const QString &reason);

	void performCaptures();

	// two-phase GPU bake: renders+readback requests run at the engine window's
	// end-of-render event (a safe point), then a timer polls until images arrive
	BakeCore::Settings settings_;
	// one entry per surface to capture; flatIndex matches BakeCore's
	// consecutive surface numbering over all high-poly objects
	struct CaptureItem
	{
		int objectId = 0;
		int surface = -1;
		int flatIndex = -1;
	};
	std::vector<CaptureItem> captureItems_;
	// resolved object IDs for the current bake, one entry per bake group
	// (single mode = one implicit group)
	struct IdGroup
	{
		std::vector<int> highIds;
		std::vector<int> lowIds;
	};
	std::vector<IdGroup> bakeGroupIds_;
	std::vector<BakeGpu::PendingCapturePtr> pending_;
	std::vector<BakeGpu::SurfaceCapture> captures_;
	bool captureStarted_ = false;
	bool captureKicked_ = false;
	// warm-up render pass before the real captures: triggers texture streaming
	// and shader compilation, then waits for them to settle
	bool warmupDone_ = false;
	std::chrono::steady_clock::time_point warmupTime_;
	int captureSize_ = 2048;
	int captureUVMode_ = -1; // -1 auto, 0/1 force UV channel
	Unigine::EventConnections engineConns_;
	QTimer *captureTimer_ = nullptr;
	std::chrono::steady_clock::time_point captureDeadline_;

	// individual Static Mesh node IDs of the two slots (picking/dropping a
	// parent node expands it into its meshes); resolved by ID at bake time
	std::vector<int> highIds_;
	std::vector<int> lowIds_;

	bool baking_ = false;
	bool cancelRequested_ = false;
	bool ruUi_ = false;   // labels/status in Russian
	bool ruTips_ = false; // tooltips in Russian

	QLabel *highLabel_ = nullptr;
	QLabel *lowLabel_ = nullptr;
	QPushButton *highPickButton_ = nullptr;
	QPushButton *lowPickButton_ = nullptr;
	// viewport-only visibility toggles (checkable); baking ignores hidden state
	QPushButton *highHideButton_ = nullptr;
	QPushButton *lowHideButton_ = nullptr;
	// bake groups (optional): each group's low parts trace only its high parts
	struct GroupRow
	{
		int highId = 0;
		int lowId = 0;
		QGroupBox *box = nullptr;
		QLabel *highLabel = nullptr;
		QLabel *lowLabel = nullptr;
	};
	std::vector<GroupRow> groupRows_;
	// group mode combo: 0 = off (single pair), 1 = manual groups, 2 = by name
	QComboBox *groupsModeCombo_ = nullptr;
	bool manualGroups() const;
	bool nameGroups() const;
	QWidget *groupsPanel_ = nullptr;
	QVBoxLayout *groupsLayout_ = nullptr;
	QPushButton *addGroupButton_ = nullptr;
	QFormLayout *modelsLayout_ = nullptr;
	QTreeWidget *meshTree_ = nullptr; // striped high/low participants list, drop target
	QLineEdit *nameEdit_ = nullptr;   // output name for textures/material
	QString lastAutoName_;            // last auto-suggested name (user edits win)
	QToolButton *settingsRollout_ = nullptr; // collapsible Settings header
	// the editor's "active tool" window (objectName "active_tool_window"),
	// caught via WindowManager::windowShown — used to open the paint tool
	QPointer<QWidget> activeToolWindow_;
	QComboBox *resolutionCombo_ = nullptr;
	QDoubleSpinBox *frontalSpin_ = nullptr;
	QDoubleSpinBox *rearSpin_ = nullptr;
	QComboBox *samplesCombo_ = nullptr;
	QCheckBox *gpuCheck_ = nullptr;
	QComboBox *captureUVCombo_ = nullptr;
	QCheckBox *flipYCheck_ = nullptr;
	QCheckBox *debugZonesCheck_ = nullptr;
	QCheckBox *shadingRaysCheck_ = nullptr;
	QCheckBox *skewMaskCheck_ = nullptr;
	QPushButton *skewMaskButton_ = nullptr;
	QLabel *skewMaskHelp_ = nullptr;
	QCheckBox *emissionCheck_ = nullptr;
	QPushButton *bakeButton_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
	QProgressBar *progressBar_ = nullptr;
	QLabel *statusLabel_ = nullptr;
};
