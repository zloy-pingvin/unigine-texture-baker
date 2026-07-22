#include "BakerWindow.h"
#include "BakeCore.h"
#include "BakeGpu.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineCollection.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineShortcutManager.h>
#include <editor/UnigineWindowManager.h>

#include <UnigineEngine.h>
#include <UnigineLog.h>
#include <UnigineObjects.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace Unigine;

// The editor has no API to query its UI language: read its own config
// (%LOCALAPPDATA%/unigine/Editor/editor1.1.cfg, JSON): "editor/language" is
// written when the language differs from the default English;
// "editor/keep_original_ui" = "1" keeps labels English and translates only
// tooltips. Fallback: detect Cyrillic in the main menu "File" action.
static void detectEditorLanguage(bool &ruLabels, bool &ruTooltips)
{
	ruLabels = false;
	ruTooltips = false;

	const QString cfgPath = qEnvironmentVariable("LOCALAPPDATA") + "/unigine/Editor/editor1.1.cfg";
	QFile file(cfgPath);
	if (file.open(QIODevice::ReadOnly))
	{
		const QJsonObject editor = QJsonDocument::fromJson(file.readAll()).object()
			.value("editor").toObject();
		const bool ru = editor.value("language").toString().startsWith("ru", Qt::CaseInsensitive);
		const bool tooltipsOnly = editor.value("keep_original_ui").toString() == QLatin1String("1");
		ruTooltips = ru;
		ruLabels = ru && !tooltipsOnly;
		return;
	}

	for (QWidget *top : QApplication::topLevelWidgets())
	{
		for (QAction *act : top->findChildren<QAction *>())
		{
			if (act->objectName() != QLatin1String("menuFile"))
				continue;
			for (QChar c : act->text())
				if (c.unicode() >= 0x0400 && c.unicode() <= 0x04FF)
				{
					ruLabels = true;
					ruTooltips = true;
					return;
				}
			return;
		}
	}
}

QString BakerWindow::tr2(const char *ru, const char *en) const
{
	return QString::fromUtf8(ruUi_ ? ru : en);
}

QString BakerWindow::tip2(const char *ru, const char *en) const
{
	return QString::fromUtf8(ruTips_ ? ru : en);
}

BakerWindow::BakerWindow(QWidget *parent)
	: QWidget(parent)
{
	setWindowTitle("Texture Baker");
	setObjectName("TextureBakerWindow");

	detectEditorLanguage(ruUi_, ruTips_);

	auto mainLayout = new QVBoxLayout(this);

	auto tabs = new QTabWidget();
	mainLayout->addWidget(tabs, 1);

	// --- Models tab ---
	auto modelsTab = new QWidget();
	modelsLayout_ = new QFormLayout(modelsTab);

	const QString notSet = tr2("<не выбрано>", "<not set>");
	const QString pickText = tr2("Выбрать", "Select");
	const QString pickTip = tip2(
		"Взять модель из текущего выделения в сцене.\n"
		"Вложенные меши выбранной ноды считаются её частями.",
		"Take the model from the current scene selection.\n"
		"Nested meshes of the selected node count as its parts.");

	const QString clearTip = tip2("Очистить слот.", "Clear the slot.");
	auto makeClearButton = [&clearTip]() {
		auto btn = new QPushButton("✕");
		btn->setFixedWidth(24);
		btn->setToolTip(clearTip);
		return btn;
	};
	const QString hideText = tr2("Скрыть", "Hide");
	const QString hideTip = tip2(
		"Скрыть/показать меши слота во вьюпорте (удобно при рисовании маски).\n"
		"На запекание не влияет: скрытые модели запекаются как обычно.",
		"Hide/show the slot meshes in the viewport (handy while painting the mask).\n"
		"Does not affect baking: hidden models bake as usual.");
	auto makeHideButton = [&hideText, &hideTip]() {
		auto btn = new QPushButton(hideText);
		btn->setCheckable(true);
		btn->setToolTip(hideTip);
		return btn;
	};

	// row 0: single-mode high-poly (the Select button ADDS the selection)
	highLabel_ = new QLabel(notSet);
	highPickButton_ = new QPushButton(pickText);
	highPickButton_->setToolTip(pickTip);
	connect(highPickButton_, &QPushButton::clicked, this, &BakerWindow::pickHighFromSelection);
	highHideButton_ = makeHideButton();
	connect(highHideButton_, &QPushButton::toggled, this,
		[this](bool on) { setSlotHidden(true, on); });
	auto highClear = makeClearButton();
	connect(highClear, &QPushButton::clicked, this, [this]() { clearSlot(true); });
	auto highRow = new QHBoxLayout();
	highRow->addWidget(highLabel_, 1);
	highRow->addWidget(highPickButton_);
	highRow->addWidget(highHideButton_);
	highRow->addWidget(highClear);
	modelsLayout_->addRow(tr2("High-poly (источник):", "High-poly (source):"), highRow);

	// row 1: single-mode low-poly
	lowLabel_ = new QLabel(notSet);
	lowPickButton_ = new QPushButton(pickText);
	lowPickButton_->setToolTip(pickTip);
	connect(lowPickButton_, &QPushButton::clicked, this, &BakerWindow::pickLowFromSelection);
	lowHideButton_ = makeHideButton();
	connect(lowHideButton_, &QPushButton::toggled, this,
		[this](bool on) { setSlotHidden(false, on); });
	auto lowClear = makeClearButton();
	connect(lowClear, &QPushButton::clicked, this, [this]() { clearSlot(false); });
	auto lowRow = new QHBoxLayout();
	lowRow->addWidget(lowLabel_, 1);
	lowRow->addWidget(lowPickButton_);
	lowRow->addWidget(lowHideButton_);
	lowRow->addWidget(lowClear);
	modelsLayout_->addRow(tr2("Low-poly (цель):", "Low-poly (target):"), lowRow);

	// row 2: bake groups mode
	groupsModeCombo_ = new QComboBox();
	groupsModeCombo_->addItem(tr2("выкл (одна пара)", "off (single pair)"), 0);
	groupsModeCombo_->addItem(tr2("вручную", "manual"), 1);
	groupsModeCombo_->addItem(tr2("по именам (авто)", "by name (auto)"), 2);
	groupsModeCombo_->setToolTip(tip2(
		"Бейк-группы: лучи каждой low-части видят только свою high-часть,\n"
		"соседние детали не отпечатываются друг на друге. Все группы\n"
		"запекаются в один общий набор текстур.\n"
		"Вручную — пары задаются списком групп.\n"
		"По именам — вложенные меши выбранных нод сопоставляются по имени\n"
		"без суффиксов _low/_high/_lp/_hp/_lod* (пары пишутся в консоль).",
		"Bake groups: rays of each low part see only its own high part, so\n"
		"neighboring pieces do not imprint on each other. All groups bake\n"
		"into one shared texture set.\n"
		"Manual — pairs are set in the group list.\n"
		"By name — nested meshes of the selected nodes are matched by name\n"
		"ignoring the _low/_high/_lp/_hp/_lod* suffixes (pairs are logged)."));
	connect(groupsModeCombo_, &QComboBox::currentIndexChanged, this, &BakerWindow::updateModelsMode);
	modelsLayout_->addRow(tr2("Группы:", "Groups:"), groupsModeCombo_);

	// row 3: the groups panel (visible only in group mode)
	groupsPanel_ = new QWidget();
	groupsLayout_ = new QVBoxLayout(groupsPanel_);
	groupsLayout_->setContentsMargins(0, 0, 0, 0);
	addGroupButton_ = new QPushButton(tr2("+ Добавить группу", "+ Add group"));
	connect(addGroupButton_, &QPushButton::clicked, this, &BakerWindow::addGroupRow);
	groupsLayout_->addWidget(addGroupButton_);
	modelsLayout_->addRow(groupsPanel_);
	modelsLayout_->setRowVisible(3, false);

	// row 4: striped participants list (dark rows = high-poly, gray = low-poly),
	// also a drop target for nodes dragged from the World Nodes hierarchy
	meshTree_ = new QTreeWidget();
	meshTree_->setColumnCount(2);
	meshTree_->setHeaderHidden(true);
	meshTree_->header()->setStretchLastSection(false);
	meshTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	meshTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
	meshTree_->setColumnWidth(1, 24);
	meshTree_->setRootIsDecorated(false);
	meshTree_->setSelectionMode(QAbstractItemView::NoSelection);
	meshTree_->setFocusPolicy(Qt::NoFocus);
	meshTree_->setMinimumHeight(120);
	meshTree_->setAcceptDrops(true);
	meshTree_->viewport()->setAcceptDrops(true);
	meshTree_->viewport()->installEventFilter(this);
	meshTree_->setToolTip(tip2(
		"Участники запекания. Тёмные строки — high-poly, светлые — low-poly.\n"
		"Перетащите ноды из окна World Nodes на секцию High-poly или Low-poly —\n"
		"они добавятся к списку. ✕ убирает меш из списка.",
		"Bake participants. Dark rows — high-poly, light rows — low-poly.\n"
		"Drag nodes from the World Nodes window onto the High-poly or Low-poly\n"
		"section to add them. ✕ removes a mesh from the list."));
	// clicking the ✕ column removes the mesh from its slot
	connect(meshTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
		if (column != 1 || !item->parent())
			return;
		const int id = item->data(0, Qt::UserRole).toInt();
		if (!id)
			return;
		const bool high = meshTree_->indexOfTopLevelItem(item->parent()) == 0;
		std::vector<int> &ids = high ? highIds_ : lowIds_;
		// a mesh leaving a hidden slot must become visible again
		if ((high ? highHideButton_ : lowHideButton_)->isChecked())
			if (NodePtr node = World::getNodeByID(id))
				node->setEnabled(true);
		ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
		if (high)
			updateSuggestedName();
		updateSlotLabels();
		refreshMeshTree();
		setUiLocked(baking_);
	});
	modelsLayout_->addRow(meshTree_);

	// row 5: output name (auto-suggested, user-editable)
	nameEdit_ = new QLineEdit();
	nameEdit_->setToolTip(tip2(
		"Имя запекаемых текстур и материала (<имя>_alb/_sh/_n и <имя>_baked.mat).\n"
		"Заполняется по high-poly модели, можно изменить перед запеканием.",
		"Name of the baked textures and material (<name>_alb/_sh/_n and <name>_baked.mat).\n"
		"Auto-filled from the high-poly model, editable before baking."));
	modelsLayout_->addRow(tr2("Имя набора:", "Output name:"), nameEdit_);

	tabs->addTab(modelsTab, tr2("Модели", "Models"));

	// --- Settings: a collapsible rollout above the Bake button (Parameters-window style) ---
	auto settingsPanel = new QWidget();
	auto settingsLayout = new QFormLayout(settingsPanel);

	resolutionCombo_ = new QComboBox();
	resolutionCombo_->addItem("512", 512);
	resolutionCombo_->addItem("1024", 1024);
	resolutionCombo_->addItem("2048", 2048);
	resolutionCombo_->addItem("4096", 4096);
	resolutionCombo_->setCurrentIndex(2);
	resolutionCombo_->setToolTip(tip2("Размер запекаемых текстур.", "Size of the baked textures."));
	settingsLayout->addRow(tr2("Разрешение:", "Resolution:"), resolutionCombo_);

	frontalSpin_ = new QDoubleSpinBox();
	frontalSpin_->setDecimals(4);
	frontalSpin_->setRange(0.0001, 100.0);
	frontalSpin_->setSingleStep(0.01);
	frontalSpin_->setValue(0.05);
	frontalSpin_->setSuffix(tr2(" м", " m"));
	frontalSpin_->setToolTip(tip2(
		"Насколько выше поверхности low-poly искать детали high-poly.\n"
		"Увеличивайте, чтобы захватить выступающий рельеф.",
		"How far above the low-poly surface to search for high-poly detail.\n"
		"Increase to capture protruding relief."));
	settingsLayout->addRow(tr2("Дистанция вперёд:", "Frontal distance:"), frontalSpin_);

	rearSpin_ = new QDoubleSpinBox();
	rearSpin_->setDecimals(4);
	rearSpin_->setRange(0.0, 100.0);
	rearSpin_->setSingleStep(0.01);
	rearSpin_->setValue(0.05);
	rearSpin_->setSuffix(tr2(" м", " m"));
	rearSpin_->setToolTip(tip2(
		"Насколько ниже поверхности low-poly искать.\n"
		"Увеличивайте, если low-poly местами выступает над high-poly.",
		"How far below the low-poly surface to search.\n"
		"Increase if the low-poly sticks out above the high-poly in places."));
	settingsLayout->addRow(tr2("Дистанция назад:", "Rear distance:"), rearSpin_);

	samplesCombo_ = new QComboBox();
	samplesCombo_->addItem(tr2("1 (быстро)", "1 (fast)"), 1);
	samplesCombo_->addItem(tr2("4 (качество)", "4 (quality)"), 4);
	samplesCombo_->addItem(tr2("16 (высокое)", "16 (high)"), 16);
	samplesCombo_->addItem(tr2("64 (максимум, медленно)", "64 (maximum, slow)"), 64);
	samplesCombo_->setCurrentIndex(1);
	samplesCombo_->setToolTip(tip2("Число лучей на тексель (сглаживание краёв).",
		"Rays per texel (edge anti-aliasing)."));
	settingsLayout->addRow(tr2("Сэмплов на тексель:", "Samples per texel:"), samplesCombo_);

	skewMaskCheck_ = new QCheckBox(tr2("учитывать skew-маску", "use skew mask"));
	skewMaskCheck_->setChecked(true);
	skewMaskCheck_->setToolTip(tip2(
		"Если у low-poly назначена Surface Custom Texture, она задаёт направление лучей:\n"
		"белое — по геометрической нормали треугольника (без скоса проекции),\n"
		"чёрное — по сглаженной нормали, серое — плавный переход.",
		"If the low-poly has a Surface Custom Texture assigned, it drives the ray direction:\n"
		"white — along the triangle's geometric normal (no projection skew),\n"
		"black — along the smoothed normal, gray — a smooth blend."));
	settingsLayout->addRow(tr2("Skew-маска:", "Skew mask:"), skewMaskCheck_);

	skewMaskButton_ = new QPushButton(tr2("🖌 Нарисовать skew-маску", "🖌 Paint skew mask"));
	skewMaskButton_->setToolTip(tip2(
		"Создаёт чёрную текстуру-маску рядом с low-poly мешем и назначает её\n"
		"в слот Surface Custom Texture.",
		"Creates a black mask texture next to the low-poly mesh and assigns it\n"
		"to the Surface Custom Texture slot."));
	settingsLayout->addRow(QString(), skewMaskButton_);
	connect(skewMaskButton_, &QPushButton::clicked, this, &BakerWindow::onCreateSkewMask);

	skewMaskHelp_ = new QLabel(tr2(
		"1. Нажмите кнопку — маска создастся, назначится на low-poly,\n"
		"    включится режим рисования (Texture Paint Mode).\n"
		"2. Выберите Custom Surface Texture в параметрах кисти.\n"
		"3. Закрасьте белым места, где детали запекаются со смещением, и сохраните.\n"
		"4. Запеките — белые зоны идут по нормали полигона, чёрные по сглаженной.",
		"1. Press the button — the mask is created, assigned to the low-poly\n"
		"    and the Texture Paint Mode activates.\n"
		"2. Pick Custom Surface Texture in the brush parameters.\n"
		"3. Paint the areas where details bake skewed white, then save.\n"
		"4. Bake — white areas use the face normal, black the smoothed one."));
	skewMaskHelp_->setWordWrap(true);
	skewMaskHelp_->setEnabled(false); // dimmed hint text
	settingsLayout->addRow(QString(), skewMaskHelp_);
	skewMaskHelp_->setVisible(skewMaskCheck_->isChecked());
	connect(skewMaskCheck_, &QCheckBox::toggled, skewMaskHelp_, &QWidget::setVisible);

	emissionCheck_ = new QCheckBox(tr2("запекать emissive (_e)", "bake emissive (_e)"));
	emissionCheck_->setChecked(false);
	emissionCheck_->setToolTip(tip2(
		"Дополнительно запекает текстуру свечения (_e) и включает\n"
		"стейт Emission у материала low-poly.",
		"Additionally bakes the emission texture (_e) and enables\n"
		"the Emission state on the low-poly material."));
	settingsLayout->addRow(tr2("Emissive:", "Emissive:"), emissionCheck_);

	// --- Debug tab ---
	auto debugTab = new QWidget();
	auto debugLayout = new QFormLayout(debugTab);

	gpuCheck_ = new QCheckBox(tr2("как в рендере (GPU)", "as rendered (GPU)"));
	gpuCheck_->setChecked(true);
	gpuCheck_->setToolTip(tip2(
		"Материалы high-poly рендерятся движком в развёртку (слои, маски,\n"
		"тайлинг, цвета) и переносятся на low-poly.\n"
		"Выключите для чтения только базовых текстур материала.",
		"The engine renders the high-poly materials into the unwrap (layers,\n"
		"masks, tiling, colors) and transfers the result onto the low-poly.\n"
		"Disable to read only the material's base textures."));
	debugLayout->addRow(tr2("Режим:", "Mode:"), gpuCheck_);

	captureUVCombo_ = new QComboBox();
	captureUVCombo_->addItem(tr2("авто", "auto"), -1);
	captureUVCombo_->addItem("UV0", 0);
	captureUVCombo_->addItem("UV1", 1);
	captureUVCombo_->setToolTip(tip2(
		"UV-канал развёртки для GPU-захвата.\n"
		"Авто: выбирается канал с меньшим перекрытием чартов (выбор пишется в консоль).",
		"UV channel to unwrap into for the GPU capture.\n"
		"Auto: the channel with less chart overlap is chosen (logged to the console)."));
	debugLayout->addRow(tr2("Развёртка захвата:", "Capture unwrap:"), captureUVCombo_);

	flipYCheck_ = new QCheckBox(tr2("инвертировать G (Y)", "invert G (Y)"));
	flipYCheck_->setChecked(false);
	flipYCheck_->setToolTip(tip2(
		"Дополнительная инверсия зелёного канала нормали.\n"
		"Обычно не нужна. Включайте, только если рельеф выглядит вывернутым.",
		"Extra inversion of the normal map green channel.\n"
		"Normally not needed. Enable only if the relief looks inverted."));
	debugLayout->addRow(tr2("Нормали:", "Normals:"), flipYCheck_);

	debugZonesCheck_ = new QCheckBox(tr2("раскрасить зоны попаданий", "colorize hit zones"));
	debugZonesCheck_->setChecked(false);
	debugZonesCheck_->setToolTip(tip2(
		"Вместо albedo запекает диагностическую раскраску:\n"
		"зелёный — поверхность выше low-poly, синий — ниже,\n"
		"красный — луч попал в изнанку, жёлтый — зона skew-маски,\n"
		"чёрный/растянутый — промах.",
		"Bakes a diagnostic colorization instead of albedo:\n"
		"green — surface above the low-poly, blue — below,\n"
		"red — the ray hit a back face, yellow — the skew mask area,\n"
		"black/stretched — a miss."));
	debugLayout->addRow(QString(), debugZonesCheck_);

	shadingRaysCheck_ = new QCheckBox(tr2("лучи по нормалям шейдинга", "rays along shading normals"));
	shadingRaysCheck_->setChecked(false);
	shadingRaysCheck_->setToolTip(tip2(
		"Пускать лучи по нормалям шейдинга вместо сглаженных.\n"
		"Может убрать скос проекции на фасетках, но даёт щели на жёстких рёбрах.",
		"Cast rays along the shading normals instead of the smoothed ones.\n"
		"May remove projection skew on facets but produces gaps at hard edges."));
	debugLayout->addRow(QString(), shadingRaysCheck_);

	tabs->addTab(debugTab, tr2("Отладка", "Debug"));

	// the Settings rollout sits between the tabs and the Bake button
	settingsRollout_ = new QToolButton();
	settingsRollout_->setText(tr2("Параметры", "Settings"));
	settingsRollout_->setCheckable(true);
	settingsRollout_->setChecked(false);
	settingsRollout_->setArrowType(Qt::RightArrow);
	settingsRollout_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	settingsRollout_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	// permanently highlighted (the theme's hover shade), so it never reads as disabled
	settingsRollout_->setStyleSheet(
		"QToolButton { background: #4a4a4a; padding: 4px; font-weight: bold; border: 1px solid #5a5a5a; border-radius: 2px; }"
		"QToolButton:hover { background: #565656; }"
		"QToolButton:pressed { background: #3e3e3e; }");
	settingsPanel->setVisible(false);
	connect(settingsRollout_, &QToolButton::toggled, settingsPanel,
		[this, settingsPanel](bool on) {
			settingsPanel->setVisible(on);
			settingsRollout_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
		});
	mainLayout->addWidget(settingsRollout_);
	mainLayout->addWidget(settingsPanel);

	// --- actions ---
	auto buttonsRow = new QHBoxLayout();
	bakeButton_ = new QPushButton(tr2("Запечь", "Bake"));
	// accent color (editor-blue) so the main action stands out
	bakeButton_->setStyleSheet(
		"QPushButton { background: #2f6fa8; color: #ffffff; font-weight: bold; padding: 5px; border: 1px solid #245680; border-radius: 2px; }"
		"QPushButton:hover { background: #3c82c0; }"
		"QPushButton:pressed { background: #245680; }"
		"QPushButton:disabled { background: #3a3a3a; color: #808080; border-color: #333333; }");
	connect(bakeButton_, &QPushButton::clicked, this, &BakerWindow::startBake);
	cancelButton_ = new QPushButton(tr2("Отмена", "Cancel"));
	cancelButton_->setEnabled(false);
	connect(cancelButton_, &QPushButton::clicked, this, &BakerWindow::cancelBake);
	buttonsRow->addWidget(bakeButton_, 1);
	buttonsRow->addWidget(cancelButton_);
	mainLayout->addLayout(buttonsRow);

	progressBar_ = new QProgressBar();
	progressBar_->setRange(0, 100);
	progressBar_->setValue(0);
	mainLayout->addWidget(progressBar_);

	statusLabel_ = new QLabel(tr2("Выберите модели и нажмите «Запечь».",
		"Select the models and press Bake."));
	statusLabel_->setWordWrap(true);
	mainLayout->addWidget(statusLabel_);

	mainLayout->addStretch(1);

	// author credit
	auto aboutLabel = new QLabel(
		"<span style=\"color:#9a9a9a;\">Texture Baker by zloy_pingvin</span>&nbsp;&nbsp;"
		"<a href=\"https://github.com/zloy-pingvin/unigine-texture-baker\" "
		"style=\"color:#7aa7cc; text-decoration:none;\">GitHub</a>&nbsp;&middot;&nbsp;"
		"<a href=\"https://t.me/zloytux\" "
		"style=\"color:#7aa7cc; text-decoration:none;\">Telegram</a>");
	aboutLabel->setOpenExternalLinks(true);
	aboutLabel->setAlignment(Qt::AlignRight);
	mainLayout->addWidget(aboutLabel);

	captureTimer_ = new QTimer(this);
	connect(captureTimer_, &QTimer::timeout, this, &BakerWindow::onCaptureTick);

	// remember the editor's "Active Tool" window when it first appears — it can
	// then be reopened programmatically after assigning the skew mask
	connect(UnigineEditor::WindowManager::instance(), &UnigineEditor::WindowManager::windowShown,
		this, [this](QWidget *widget) {
			if (widget && widget->objectName() == QLatin1String("ActiveToolWidget"))
				activeToolWindow_ = widget;
		});

	restoreSettings();
	refreshMeshTree();
	setUiLocked(false);
}

BakerWindow::~BakerWindow()
{
	saveSettings();
}

void BakerWindow::saveSettings() const
{
	QSettings s("zloy_pingvin", "TextureBaker");
	s.setValue("resolution", resolutionCombo_->currentData().toInt());
	s.setValue("frontal", frontalSpin_->value());
	s.setValue("rear", rearSpin_->value());
	s.setValue("samples", samplesCombo_->currentData().toInt());
	s.setValue("use_skew_mask", skewMaskCheck_->isChecked());
	s.setValue("bake_emission", emissionCheck_->isChecked());
	s.setValue("groups_mode", groupsModeCombo_->currentData().toInt());
	s.setValue("settings_expanded", settingsRollout_->isChecked());
	s.setValue("gpu_mode", gpuCheck_->isChecked());
	s.setValue("capture_uv", captureUVCombo_->currentData().toInt());
	s.setValue("flip_y", flipYCheck_->isChecked());
	s.setValue("shading_rays", shadingRaysCheck_->isChecked());
}

void BakerWindow::restoreSettings()
{
	QSettings s("zloy_pingvin", "TextureBaker");
	int idx = resolutionCombo_->findData(s.value("resolution", 2048).toInt());
	if (idx >= 0)
		resolutionCombo_->setCurrentIndex(idx);
	frontalSpin_->setValue(s.value("frontal", 0.05).toDouble());
	rearSpin_->setValue(s.value("rear", 0.05).toDouble());
	idx = samplesCombo_->findData(s.value("samples", 4).toInt());
	if (idx >= 0)
		samplesCombo_->setCurrentIndex(idx);
	skewMaskCheck_->setChecked(s.value("use_skew_mask", true).toBool());
	emissionCheck_->setChecked(s.value("bake_emission", false).toBool());
	// manual groups are not persisted (node IDs are not stable across sessions),
	// so restore the manual mode as "off"
	int gm = s.value("groups_mode", 0).toInt();
	if (gm == 1)
		gm = 0;
	idx = groupsModeCombo_->findData(gm);
	if (idx >= 0)
		groupsModeCombo_->setCurrentIndex(idx);
	settingsRollout_->setChecked(s.value("settings_expanded", false).toBool());
	gpuCheck_->setChecked(s.value("gpu_mode", true).toBool());
	idx = captureUVCombo_->findData(s.value("capture_uv", -1).toInt());
	if (idx >= 0)
		captureUVCombo_->setCurrentIndex(idx);
	flipYCheck_->setChecked(s.value("flip_y", false).toBool());
	shadingRaysCheck_->setChecked(s.value("shading_rays", false).toBool());
}

// Picks a node containing Static Meshes (itself or nested children) from the
// scene selection; fills the label with the name (+ mesh count). Returns the
// node ID or 0.
int BakerWindow::grabSelectedMeshGroupId(QLabel *label)
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return 0;
	}
	for (const NodePtr &node : selector->getNodes())
	{
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(node);
		if (meshes.empty())
			continue;
		QString name = QString::fromUtf8(node->getName());
		if (meshes.size() > 1)
			name += tr2(" (%1 мешей)", " (%1 meshes)").arg(meshes.size());
		label->setText(name);
		statusLabel_->setText("");
		return node->getID();
	}
	statusLabel_->setText(tr2("В выделении нет ноды с мешами типа Static Mesh.",
		"The selection contains no node with Static Meshes."));
	return 0;
}

// Expands the given root nodes into their Static Meshes and APPENDS them to
// the high/low slot (duplicates skipped), then refreshes the dependent UI.
void BakerWindow::addSlotNodes(bool high, const std::vector<int> &rootIds)
{
	std::vector<int> &ids = high ? highIds_ : lowIds_;
	int added = 0;
	for (int rootId : rootIds)
		for (const Ptr<ObjectMeshStatic> &mesh : BakeCore::collectMeshes(World::getNodeByID(rootId)))
		{
			const int id = mesh->getID();
			if (std::find(ids.begin(), ids.end(), id) == ids.end())
			{
				ids.push_back(id);
				added++;
			}
		}
	if (!added)
	{
		statusLabel_->setText(tr2("Мешей типа Static Mesh не добавлено (нет в ноде или уже в списке).",
			"No Static Meshes added (none in the node or already listed)."));
		return;
	}
	// newly added meshes follow the slot's current visibility toggle
	if ((high ? highHideButton_ : lowHideButton_)->isChecked())
		setSlotHidden(high, true);
	statusLabel_->setText("");
	if (high)
		updateSuggestedName();
	updateSlotLabels();
	refreshMeshTree();
	setUiLocked(baking_);
}

// Viewport-only visibility of the slot meshes; baking ignores it
// (collectMeshes includes disabled nodes on purpose).
void BakerWindow::setSlotHidden(bool high, bool hidden)
{
	for (int id : (high ? highIds_ : lowIds_))
		if (NodePtr node = World::getNodeByID(id))
			node->setEnabled(!hidden);
	QPushButton *btn = high ? highHideButton_ : lowHideButton_;
	btn->setText(hidden ? tr2("Показать", "Show") : tr2("Скрыть", "Hide"));
}

void BakerWindow::clearSlot(bool high)
{
	// restore visibility before dropping the meshes from the list
	QPushButton *hideBtn = high ? highHideButton_ : lowHideButton_;
	if (hideBtn->isChecked())
		hideBtn->setChecked(false);
	(high ? highIds_ : lowIds_).clear();
	if (high)
		updateSuggestedName();
	updateSlotLabels();
	refreshMeshTree();
	setUiLocked(baking_);
}

void BakerWindow::updateSlotLabels()
{
	auto describe = [this](const std::vector<int> &ids) -> QString {
		if (ids.empty())
			return tr2("<не выбрано>", "<not set>");
		QString name;
		if (NodePtr node = World::getNodeByID(ids[0]))
			name = QString::fromUtf8(node->getName());
		if (ids.size() > 1)
			name += tr2(" (+ ещё %1)", " (+ %1 more)").arg(ids.size() - 1);
		return name;
	};
	highLabel_->setText(describe(highIds_));
	lowLabel_->setText(describe(lowIds_));
}

// Rebuilds the striped participants list: a High-poly section with dark rows
// and a Low-poly section with lighter rows (like the World Nodes hierarchy).
// Column 1 holds the per-mesh remove button (✕).
void BakerWindow::refreshMeshTree()
{
	if (!meshTree_)
		return;
	meshTree_->clear();

	const QColor headerBg(0x33, 0x33, 0x33);
	const QColor highBg(0x26, 0x26, 0x26);
	const QColor lowBg(0x42, 0x42, 0x42);

	auto addSection = [this, &headerBg](const QString &title, const std::vector<int> &ids, const QColor &rowBg) {
		auto top = new QTreeWidgetItem(meshTree_, {title});
		QFont f = top->font(0);
		f.setBold(true);
		top->setFont(0, f);
		top->setBackground(0, headerBg);
		top->setBackground(1, headerBg);
		top->setFlags(top->flags() & ~Qt::ItemIsSelectable);
		for (int id : ids)
		{
			NodePtr node = World::getNodeByID(id);
			if (!node)
				continue;
			auto item = new QTreeWidgetItem(top, {QString::fromUtf8(node->getName()), "✕"});
			item->setBackground(0, rowBg);
			item->setBackground(1, rowBg);
			item->setTextAlignment(1, Qt::AlignCenter);
			item->setForeground(1, QColor(0xa0, 0xa0, 0xa0));
			item->setData(0, Qt::UserRole, id);
			item->setToolTip(1, tip2("Убрать из списка.", "Remove from the list."));
		}
		if (top->childCount() == 0)
		{
			auto item = new QTreeWidgetItem(top, {tr2("<не выбрано>", "<not set>")});
			item->setBackground(0, rowBg);
			item->setBackground(1, rowBg);
			item->setForeground(0, QColor(0x80, 0x80, 0x80));
		}
	};
	addSection(tr2("High-poly (источник)", "High-poly (source)"), highIds_, highBg);
	addSection(tr2("Low-poly (цель)", "Low-poly (target)"), lowIds_, lowBg);
	meshTree_->expandAll();
}

void BakerWindow::updateSuggestedName()
{
	QString suggestion;
	if (manualGroups())
	{
		const int rootId = groupRows_.empty() ? 0 : groupRows_[0].highId;
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(World::getNodeByID(rootId));
		if (!meshes.empty())
			suggestion = QString::fromUtf8(BakeCore::suggestBaseName(meshes[0]).get());
	}
	else if (!highIds_.empty())
	{
		Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(highIds_[0]));
		if (mesh)
			suggestion = QString::fromUtf8(BakeCore::suggestBaseName(mesh).get());
	}
	// respect a user-edited name: overwrite only untouched/auto-filled text
	if (nameEdit_->text().isEmpty() || nameEdit_->text() == lastAutoName_)
		nameEdit_->setText(suggestion);
	lastAutoName_ = suggestion;
}

// The World Nodes hierarchy drags "application/unigine.hierarchy.list". The
// payload layout is not documented: scan it for 32-bit values (both byte
// orders) that resolve to existing scene nodes.
std::vector<int> BakerWindow::decodeNodeDrop(const QMimeData *mime) const
{
	std::vector<int> ids;
	if (!mime)
		return ids;
	const QByteArray data = mime->data(QStringLiteral("application/unigine.hierarchy.list"));
	if (data.isEmpty())
		return ids;

	auto tryAdd = [&ids](int v) {
		if (v == 0 || !World::getNodeByID(v))
			return;
		if (std::find(ids.begin(), ids.end(), v) == ids.end())
			ids.push_back(v);
	};
	for (int off = 0; off + 4 <= data.size(); off++)
	{
		quint32 le = 0;
		memcpy(&le, data.constData() + off, 4);
		tryAdd(int(le));
		const quint32 be = ((le & 0xFF) << 24) | ((le & 0xFF00) << 8)
			| ((le >> 8) & 0xFF00) | (le >> 24);
		tryAdd(int(be));
	}

	if (ids.empty())
		Unigine::Log::warning("Baker: could not decode the dropped nodes (payload %d bytes: %s...)\n",
			int(data.size()), data.left(48).toHex(' ').constData());
	return ids;
}

bool BakerWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (meshTree_ && watched == meshTree_->viewport())
	{
		if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)
		{
			auto drag = static_cast<QDropEvent *>(event);
			if (drag->mimeData()->hasFormat(QStringLiteral("application/unigine.hierarchy.list")))
			{
				drag->acceptProposedAction();
				return true;
			}
		}
		else if (event->type() == QEvent::Drop)
		{
			auto drop = static_cast<QDropEvent *>(event);
			std::vector<int> ids = decodeNodeDrop(drop->mimeData());
			if (ids.empty())
			{
				statusLabel_->setText(tr2("Не удалось распознать перетащенную ноду.",
					"Could not recognize the dropped node."));
				return true;
			}
			// the section (top-level item) under the cursor decides the slot
			QTreeWidgetItem *item = meshTree_->itemAt(drop->position().toPoint());
			if (!item)
			{
				statusLabel_->setText(tr2("Бросьте ноду на секцию High-poly или Low-poly.",
					"Drop the node onto the High-poly or Low-poly section."));
				return true;
			}
			QTreeWidgetItem *top = item;
			while (top->parent())
				top = top->parent();
			addSlotNodes(meshTree_->indexOfTopLevelItem(top) == 0, ids);
			drop->acceptProposedAction();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void BakerWindow::pickHighFromSelection()
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return;
	}
	std::vector<int> roots;
	for (const NodePtr &node : selector->getNodes())
		roots.push_back(node->getID());
	addSlotNodes(true, roots);
}

void BakerWindow::addGroupRow()
{
	GroupRow gr;
	gr.box = new QGroupBox();
	auto form = new QFormLayout(gr.box);

	const QString notSet = tr2("<не выбрано>", "<not set>");
	const QString pickText = tr2("Выбрать", "Select");
	const QString clearTip = tip2("Очистить слот.", "Clear the slot.");
	auto makeClearButton = [&clearTip]() {
		auto btn = new QPushButton("✕");
		btn->setFixedWidth(24);
		btn->setToolTip(clearTip);
		return btn;
	};

	gr.highLabel = new QLabel(notSet);
	auto highBtn = new QPushButton(pickText);
	auto highClear = makeClearButton();
	auto highRow = new QHBoxLayout();
	highRow->addWidget(gr.highLabel, 1);
	highRow->addWidget(highBtn);
	highRow->addWidget(highClear);
	form->addRow("High:", highRow);

	gr.lowLabel = new QLabel(notSet);
	auto lowBtn = new QPushButton(pickText);
	auto lowClear = makeClearButton();
	auto lowRow = new QHBoxLayout();
	lowRow->addWidget(gr.lowLabel, 1);
	lowRow->addWidget(lowBtn);
	lowRow->addWidget(lowClear);
	form->addRow("Low:", lowRow);

	auto removeBtn = new QPushButton(tr2("Удалить группу", "Remove group"));
	form->addRow(QString(), removeBtn);

	QGroupBox *boxPtr = gr.box;
	connect(highBtn, &QPushButton::clicked, this, [this, boxPtr]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				int id = grabSelectedMeshGroupId(g.highLabel);
				if (id)
					g.highId = id;
				break;
			}
		updateSuggestedName();
		setUiLocked(baking_);
	});
	connect(lowBtn, &QPushButton::clicked, this, [this, boxPtr]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				int id = grabSelectedMeshGroupId(g.lowLabel);
				if (id)
					g.lowId = id;
				break;
			}
		setUiLocked(baking_);
	});
	connect(highClear, &QPushButton::clicked, this, [this, boxPtr, notSet]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				g.highId = 0;
				g.highLabel->setText(notSet);
				break;
			}
		updateSuggestedName();
		setUiLocked(baking_);
	});
	connect(lowClear, &QPushButton::clicked, this, [this, boxPtr, notSet]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				g.lowId = 0;
				g.lowLabel->setText(notSet);
				break;
			}
		setUiLocked(baking_);
	});
	connect(removeBtn, &QPushButton::clicked, this, [this, boxPtr]() { removeGroupRow(boxPtr); });

	// keep the add button at the bottom of the panel
	groupsLayout_->insertWidget(groupsLayout_->count() - 1, gr.box);
	groupRows_.push_back(gr);
	renumberGroups();
	setUiLocked(baking_);
}

void BakerWindow::removeGroupRow(QGroupBox *box)
{
	for (auto it = groupRows_.begin(); it != groupRows_.end(); ++it)
		if (it->box == box)
		{
			box->deleteLater();
			groupRows_.erase(it);
			break;
		}
	renumberGroups();
	setUiLocked(baking_);
}

void BakerWindow::renumberGroups()
{
	for (size_t i = 0; i < groupRows_.size(); i++)
		groupRows_[i].box->setTitle(tr2("Группа %1", "Group %1").arg(i + 1));
}

bool BakerWindow::manualGroups() const
{
	return groupsModeCombo_->currentData().toInt() == 1;
}

bool BakerWindow::nameGroups() const
{
	return groupsModeCombo_->currentData().toInt() == 2;
}

void BakerWindow::updateModelsMode()
{
	const bool manual = manualGroups();
	modelsLayout_->setRowVisible(0, !manual); // single-mode high row
	modelsLayout_->setRowVisible(1, !manual); // single-mode low row
	modelsLayout_->setRowVisible(3, manual);  // manual groups panel
	modelsLayout_->setRowVisible(4, !manual); // participants list
	if (manual && groupRows_.empty())
		addGroupRow();
	updateSuggestedName();
	setUiLocked(baking_);
}

int BakerWindow::firstLowId() const
{
	if (!manualGroups())
		return lowIds_.empty() ? 0 : lowIds_[0];
	for (const GroupRow &g : groupRows_)
		if (g.lowId)
			return g.lowId;
	return 0;
}

// Normalized name for by-name pairing: lowercased, the trailing
// _low/_high/_lp/_hp/_lowpoly/_highpoly/_lod<N> decorations stripped.
static QString bakeGroupKey(const char *name)
{
	QString n = QString::fromUtf8(name).toLower();
	static const QRegularExpression re(
		QStringLiteral("(_(low|high|lp|hp|lowpoly|highpoly)|_lod[_\\-]?\\d*)+$"));
	n.remove(re);
	return n;
}

void BakerWindow::pickLowFromSelection()
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return;
	}
	std::vector<int> roots;
	for (const NodePtr &node : selector->getNodes())
		roots.push_back(node->getID());
	addSlotNodes(false, roots);
}

void BakerWindow::setUiLocked(bool locked)
{
	highPickButton_->setEnabled(!locked);
	lowPickButton_->setEnabled(!locked);
	groupsModeCombo_->setEnabled(!locked);
	addGroupButton_->setEnabled(!locked);
	for (const GroupRow &g : groupRows_)
		g.box->setEnabled(!locked);
	resolutionCombo_->setEnabled(!locked);
	frontalSpin_->setEnabled(!locked);
	rearSpin_->setEnabled(!locked);
	samplesCombo_->setEnabled(!locked);
	flipYCheck_->setEnabled(!locked);
	gpuCheck_->setEnabled(!locked);
	captureUVCombo_->setEnabled(!locked);
	debugZonesCheck_->setEnabled(!locked);
	shadingRaysCheck_->setEnabled(!locked);
	skewMaskCheck_->setEnabled(!locked);
	emissionCheck_->setEnabled(!locked);
	skewMaskButton_->setEnabled(!locked && firstLowId() != 0);
	bool modelsReady;
	if (manualGroups())
	{
		modelsReady = !groupRows_.empty();
		for (const GroupRow &g : groupRows_)
			if (!g.highId || !g.lowId)
				modelsReady = false;
	}
	else
		modelsReady = !highIds_.empty() && !lowIds_.empty();
	bakeButton_->setEnabled(!locked && modelsReady);
	cancelButton_->setEnabled(locked);
}

void BakerWindow::cancelBake()
{
	cancelRequested_ = true;
	statusLabel_->setText(tr2("Отмена...", "Cancelling..."));
}

void BakerWindow::onCreateSkewMask()
{
	if (baking_)
		return;

	const int lowId = firstLowId();
	Ptr<ObjectMeshStatic> low;
	{
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(World::getNodeByID(lowId));
		if (!meshes.empty())
			low = meshes[0];
	}
	if (!low)
	{
		statusLabel_->setText(tr2("Low-poly модель не выбрана (или нода удалена).",
			"The low-poly model is not set (or the node was deleted)."));
		return;
	}

	String error;
	String virtualPath = BakeCore::createSkewMask(low, 1024, error);
	if (virtualPath.empty())
	{
		statusLabel_->setText(QString::fromUtf8(error.get()));
		return;
	}
	// import UNCOMPRESSED ("unchanged"): the default texture import compresses
	// to BC (ATI1), and the editor's paint tools cannot paint compressed textures.
	// An already-correct asset is NOT touched at all: reimport re-processes the
	// asset after this call returns and wipes the freshly assigned slot.
	const bool wasAsset = UnigineEditor::AssetManager::isAsset(virtualPath.get());
	bool needsImport = !wasAsset;
	bool needsReimport = false;
	if (wasAsset)
	{
		UnigineEditor::CollectionPtr current =
			UnigineEditor::AssetManager::getAssetImportParameters(virtualPath.get());
		needsReimport = !(current && current->getBool("unchanged", false));
	}
	if (needsImport || needsReimport)
	{
		UnigineEditor::CollectionPtr importParams = UnigineEditor::Collection::create();
		importParams->setBool("unchanged", true);
		importParams->setBool("srgb_correction", false);
		const bool imported = needsImport
			? UnigineEditor::AssetManager::importAssetSync(virtualPath.get(), importParams)
			: UnigineEditor::AssetManager::reimportAssetSync(virtualPath.get(), importParams);
		Unigine::Log::message("Baker: skew mask %s \"%s\": %s\n",
			needsImport ? "import" : "reimport (was compressed)",
			virtualPath.get(), imported ? "ok" : "FAILED");
		if (needsImport && !imported)
		{
			// without a first import the engine cannot resolve the path at all
			statusLabel_->setText(tr2("Не удалось импортировать маску: %1",
				"Failed to import the mask: %1")
				.arg(QString::fromUtf8(virtualPath.get())));
			return;
		}
	}
	else
	{
		Unigine::Log::message("Baker: skew mask \"%s\" already imported, reusing as is\n",
			virtualPath.get());
	}

	// one mask in the shared UV layout, assigned to every low-poly part
	if (manualGroups())
	{
		for (const GroupRow &g : groupRows_)
			for (const Ptr<ObjectMeshStatic> &part : BakeCore::collectMeshes(World::getNodeByID(g.lowId)))
				BakeCore::assignSkewMask(part, virtualPath.get());
	}
	else
		for (int id : lowIds_)
			if (Ptr<ObjectMeshStatic> part = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				BakeCore::assignSkewMask(part, virtualPath.get());

	// hide the high-poly so it does not block painting on the low-poly
	// (viewport-only: baking ignores the hidden state)
	if (manualGroups())
	{
		for (const GroupRow &g : groupRows_)
			for (const Ptr<ObjectMeshStatic> &part : BakeCore::collectMeshes(World::getNodeByID(g.highId)))
				part->setEnabled(false);
	}
	else if (!highIds_.empty() && !highHideButton_->isChecked())
		highHideButton_->setChecked(true); // triggers setSlotHidden(true, true)

	// select the low-poly so the Texture Editor picks it up; refresh so the
	// Parameters window shows the newly assigned custom texture
	auto selectLow = [lowId]() {
		Unigine::Vector<NodePtr> nodes;
		nodes.append(World::getNodeByID(lowId));
		if (UnigineEditor::SelectorNodes *sel = UnigineEditor::SelectorNodes::createObjectsSelector(nodes))
			UnigineEditor::SelectionAction::applySelection(sel);
		UnigineEditor::SelectionAction::refreshSelection();
	};
	selectLow();

	// open the editor's "Active Tool" window (tool hotkeys are unreachable —
	// engine level — but the tool WINDOW is managed by WindowManager)
	QWidget *toolWindow = activeToolWindow_;
	if (!toolWindow)
		for (QWidget *w : UnigineEditor::WindowManager::allWindows())
			if (w->objectName() == QLatin1String("ActiveToolWidget"))
			{
				toolWindow = w;
				break;
			}
	if (toolWindow)
	{
		UnigineEditor::WindowManager::show(toolWindow);
		UnigineEditor::WindowManager::activate(toolWindow);
	}

	// activate the Texture Paint mode by firing its editor shortcut handler
	// directly (the hotkey itself is handled at the engine level and cannot be
	// synthesized, but the ShortcutManager exposes the shortcut events)
	bool paintActivated = false;
	if (UnigineEditor::ShortcutContextPtr ctx = UnigineEditor::ShortcutManager::getContext("global"))
		if (UnigineEditor::ShortcutPtr sc = ctx->getShortcut("texture_paint_mode"))
		{
			static_cast<Unigine::EventInvoker<> &>(sc->getEventPressed()).run();
			paintActivated = true;
			Unigine::Log::message("Baker: Texture Paint Mode activated\n");
		}
	if (!paintActivated)
		Unigine::Log::warning("Baker: texture_paint_mode shortcut not found, activate the paint mode manually\n");

	// the paint mode hooks the mesh on a selection CHANGE event, but our
	// selection was applied before the mode became active — re-selecting the
	// same node does nothing (identical selection emits no "changed"). Deselect,
	// let the editor fully process the empty state (a synchronous clear+select
	// is coalesced and does not re-hook the tool), then re-select (this is the
	// manual "switch away and back" workaround, automated).
	if (paintActivated)
	{
		QTimer::singleShot(150, this, []() {
			UnigineEditor::Selection::clear();
			UnigineEditor::SelectionAction::refreshSelection();
		});
		QTimer::singleShot(450, this, [selectLow]() { selectLow(); });
	}

	statusLabel_->setText(
		(paintActivated
			? tr2("Маска назначена (%1), режим рисования включён.\n"
				  "Выберите Custom Surface Texture, закрасьте белым проблемные\n"
				  "места и сохраните текстуру.",
				  "Mask assigned (%1), the paint mode is on.\n"
				  "Pick Custom Surface Texture, paint the problem areas white\n"
				  "and save the texture.")
			: tr2("Маска назначена (%1). Нажмите Shift+2 (Texture Paint Mode), выберите\n"
				  "Custom Surface Texture, закрасьте белым проблемные места и сохраните.",
				  "Mask assigned (%1). Press Shift+2 (Texture Paint Mode), pick\n"
				  "Custom Surface Texture, paint the problem areas white and save."))
			.arg(QString::fromUtf8(virtualPath.get())));
}

void BakerWindow::startBake()
{
	if (baking_)
		return;

	// resolve the bake groups (single mode = one implicit group).
	// baking a mesh onto itself is allowed: it is a useful null-test
	// (the resulting normal map must be perfectly flat)
	std::vector<BakeCore::BakeGroup> bakeGroups;
	if (manualGroups())
	{
		for (size_t i = 0; i < groupRows_.size(); i++)
		{
			BakeCore::BakeGroup g;
			g.highs = BakeCore::collectMeshes(World::getNodeByID(groupRows_[i].highId));
			g.lows = BakeCore::collectMeshes(World::getNodeByID(groupRows_[i].lowId));
			if (g.highs.empty() || g.lows.empty())
			{
				statusLabel_->setText(tr2("В группе %1 не выбраны модели (или нода удалена).",
					"Group %1 has models missing (or a node was deleted).").arg(i + 1));
				return;
			}
			bakeGroups.push_back(std::move(g));
		}
		if (bakeGroups.empty())
		{
			statusLabel_->setText(tr2("Добавьте хотя бы одну группу.", "Add at least one group."));
			return;
		}
	}
	else
	{
		std::vector<Ptr<ObjectMeshStatic>> highs, lows;
		for (int id : highIds_)
			if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				highs.push_back(mesh);
		for (int id : lowIds_)
			if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				lows.push_back(mesh);
		if (highs.empty())
		{
			statusLabel_->setText(tr2("High-poly модель не выбрана (или нода удалена).",
				"The high-poly model is not set (or the node was deleted)."));
			return;
		}
		if (lows.empty())
		{
			statusLabel_->setText(tr2("Low-poly модель не выбрана (или нода удалена).",
				"The low-poly model is not set (or the node was deleted)."));
			return;
		}

		if (nameGroups())
		{
			// pair the parts by normalized names; group order follows the lows
			std::vector<QString> keys;
			auto findKey = [&keys](const QString &k) -> int {
				for (size_t i = 0; i < keys.size(); i++)
					if (keys[i] == k)
						return int(i);
				return -1;
			};
			for (const auto &low : lows)
			{
				const QString k = bakeGroupKey(low->getName());
				int i = findKey(k);
				if (i < 0)
				{
					keys.push_back(k);
					bakeGroups.push_back(BakeCore::BakeGroup());
					i = int(keys.size()) - 1;
				}
				bakeGroups[i].lows.push_back(low);
			}
			QStringList unmatchedHighs;
			for (const auto &high : highs)
			{
				const int i = findKey(bakeGroupKey(high->getName()));
				if (i >= 0)
					bakeGroups[i].highs.push_back(high);
				else
					unmatchedHighs << QString::fromUtf8(high->getName());
			}
			QStringList unmatchedLows;
			for (size_t i = 0; i < bakeGroups.size(); i++)
				if (bakeGroups[i].highs.empty())
					for (const auto &low : bakeGroups[i].lows)
						unmatchedLows << QString::fromUtf8(low->getName());
			if (!unmatchedLows.isEmpty())
			{
				statusLabel_->setText(tr2("Нет high-пары по имени для: %1",
					"No high match by name for: %1").arg(unmatchedLows.join(", ")));
				return;
			}
			if (!unmatchedHighs.isEmpty())
				Unigine::Log::warning("Baker: high parts with no low match by name (skipped): %s\n",
					unmatchedHighs.join(", ").toUtf8().constData());
			for (size_t i = 0; i < bakeGroups.size(); i++)
				Unigine::Log::message("Baker: name group \"%s\": %d high, %d low\n",
					keys[i].toUtf8().constData(),
					int(bakeGroups[i].highs.size()), int(bakeGroups[i].lows.size()));
		}
		else
		{
			BakeCore::BakeGroup g;
			g.highs = std::move(highs);
			g.lows = std::move(lows);
			bakeGroups.push_back(std::move(g));
		}
	}

	bakeGroupIds_.clear();
	for (const BakeCore::BakeGroup &g : bakeGroups)
	{
		IdGroup ids;
		for (const auto &obj : g.highs)
			ids.highIds.push_back(obj->getID());
		for (const auto &obj : g.lows)
			ids.lowIds.push_back(obj->getID());
		bakeGroupIds_.push_back(std::move(ids));
	}

	saveSettings();
	settings_ = BakeCore::Settings();
	settings_.resolution = resolutionCombo_->currentData().toInt();
	settings_.frontalDistance = float(frontalSpin_->value());
	settings_.rearDistance = float(rearSpin_->value());
	settings_.supersamples = samplesCombo_->currentData().toInt();
	settings_.flipNormalY = flipYCheck_->isChecked();
	settings_.gpuMode = gpuCheck_->isChecked();
	captureUVMode_ = captureUVCombo_->currentData().toInt();
	settings_.debugZones = debugZonesCheck_->isChecked();
	settings_.raysAlongShading = shadingRaysCheck_->isChecked();
	settings_.useSkewMask = skewMaskCheck_->isChecked();
	settings_.bakeEmission = emissionCheck_->isChecked();
	settings_.outputName = nameEdit_->text().trimmed().toUtf8().constData();

	baking_ = true;
	cancelRequested_ = false;
	setUiLocked(true);
	progressBar_->setValue(0);

	pending_.clear();
	captures_.clear();

	// GPU mode phase 1: renders and readback requests must run at a safe point
	// of the engine frame (a Qt slot can fire mid-render: "frame is already
	// rendering"). Subscribe to the main window's end-of-render event, do the
	// captures there, then poll for readback completion with a timer.
	if (settings_.gpuMode)
	{
		// flat surface numbering spans all groups and objects in order —
		// must match BakeCore's numbering of srcSurfaces
		captureItems_.clear();
		int flatBase = 0;
		for (const BakeCore::BakeGroup &g : bakeGroups)
		for (const auto &obj : g.highs)
		{
			Ptr<ConstMesh> highMesh = obj->getMeshForceRAM();
			if (!highMesh)
				continue;
			for (int s = 0; s < highMesh->getNumSurfaces(); s++)
			{
				if (!BakeCore::isSurfaceBakeable(obj, s))
					continue;
				if (highMesh->getNumTexCoords0(s) <= 0)
					continue;
				captureItems_.push_back({obj->getID(), s, flatBase + s});
			}
			flatBase += highMesh->getNumSurfaces();
		}
		if (!captureItems_.empty())
		{
			captureSize_ = settings_.resolution;
			captureSize_ = captureSize_ < 512 ? 512 : (captureSize_ > 2048 ? 2048 : captureSize_);
			captureStarted_ = false;
			captureKicked_ = false;
			warmupDone_ = false;
			engineConns_.disconnectAll();
			// end of the engine UPDATE phase: no frame is being rendered, the
			// classic point where samples do render-to-texture
			Engine::get()->getEventEndUpdate().connect(engineConns_, [this]() { performCaptures(); });
			statusLabel_->setText(tr2("Захват материалов (GPU)...", "Capturing materials (GPU)..."));
			captureDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
			captureTimer_->start(50);
			return; // continues in performCaptures() + onCaptureTick()
		}
	}

	runBake();
}

// runs at the end of the engine update phase — a safe moment
// for our own off-screen renders and readback requests
void BakerWindow::performCaptures()
{
	// the captures take seconds and Qt timers can fire re-entrantly meanwhile,
	// so guard the entry and signal completion (captureKicked_) only at the end
	if (captureStarted_)
		return;

	// cold start protection: on the first bake after editor launch the material
	// textures may not be streamed in yet and the shaders may still be compiling
	// asynchronously — a capture then grabs placeholder data (broken normals).
	// Pass 1 renders every surface once (discarding the results) to trigger the
	// loading, then the real captures run after the resources settle.
	if (!warmupDone_)
	{
		const bool prevForceStreaming = Render::isForceStreaming();
		const Render::SHADERS_COMPILE_MODE prevCompileMode = Render::getShadersCompileMode();
		Render::setForceStreaming(true);
		Render::setShadersCompileMode(Render::SHADERS_COMPILE_MODE_FORCE);
		for (const CaptureItem &item : captureItems_)
		{
			Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(item.objectId));
			Ptr<ConstMesh> mesh = obj ? obj->getMeshForceRAM() : Ptr<ConstMesh>();
			if (mesh)
				BakeGpu::requestSurfaceCapture(obj, mesh, item.surface, captureSize_, captureUVMode_);
		}
		Render::setShadersCompileMode(prevCompileMode);
		Render::setForceStreaming(prevForceStreaming);
		warmupDone_ = true;
		warmupTime_ = std::chrono::steady_clock::now();
		return; // stay subscribed; the real captures run on a later frame
	}
	// give texture streaming and shader compilation time to finish
	if (std::chrono::steady_clock::now() - warmupTime_ < std::chrono::milliseconds(1200))
		return;

	captureStarted_ = true;

	const bool prevForceStreaming = Render::isForceStreaming();
	const Render::SHADERS_COMPILE_MODE prevCompileMode = Render::getShadersCompileMode();
	Render::setForceStreaming(true);
	Render::setShadersCompileMode(Render::SHADERS_COMPILE_MODE_FORCE);

	for (const CaptureItem &item : captureItems_)
	{
		Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(item.objectId));
		Ptr<ConstMesh> mesh = obj ? obj->getMeshForceRAM() : Ptr<ConstMesh>();
		if (!mesh)
			continue;
		BakeGpu::PendingCapturePtr p = BakeGpu::requestSurfaceCapture(obj, mesh, item.surface,
			captureSize_, captureUVMode_, settings_.bakeEmission);
		if (p)
		{
			p->surface = item.flatIndex; // capture list index = BakeCore's flat surface index
			pending_.push_back(p);
		}
		else
			Unigine::Log::warning("Baker: GPU capture request failed for \"%s\" surface %d\n",
				obj->getName(), item.surface);
	}
	// each render's swap delivers the previous render's readbacks;
	// a final dummy render flushes the last one
	if (!captureItems_.empty())
		BakeGpu::flushTransfers();

	Render::setShadersCompileMode(prevCompileMode);
	Render::setForceStreaming(prevForceStreaming);

	captureKicked_ = true;
}

void BakerWindow::abortCapture(const QString &reason)
{
	engineConns_.disconnectAll();
	captureTimer_->stop();
	pending_.clear();
	baking_ = false;
	setUiLocked(false);
	progressBar_->setValue(0);
	statusLabel_->setText(reason);
}

void BakerWindow::onCaptureTick()
{
	if (cancelRequested_)
	{
		engineConns_.disconnectAll();
		abortCapture(tr2("Запекание отменено.", "Baking cancelled."));
		return;
	}

	const bool deadlinePassed = std::chrono::steady_clock::now() >= captureDeadline_;

	// still waiting for the end-of-render event to fire
	if (!captureKicked_)
	{
		if (deadlinePassed)
		{
			engineConns_.disconnectAll();
			captureTimer_->stop();
			Unigine::Log::warning("Baker: end-of-update event never fired, GPU captures skipped\n");
			runBake();
		}
		return;
	}
	engineConns_.disconnectAll(); // one-shot: captures already requested

	int readyCount = 0;
	for (const auto &p : pending_)
		if (p->ready())
			readyCount++;
	const bool allReady = readyCount == int(pending_.size());

	if (!allReady && !deadlinePassed)
		return;

	captureTimer_->stop();

	int maxSurface = -1;
	for (const auto &p : pending_)
		maxSurface = std::max(maxSurface, p->surface);
	captures_.assign(maxSurface + 1, BakeGpu::SurfaceCapture());

	for (const auto &p : pending_)
	{
		if (p->ready())
			captures_[p->surface] = BakeGpu::finishCapture(p);
		else
			Unigine::Log::warning("Baker: GPU readback timed out for surface %d\n", p->surface);
	}
	pending_.clear();

	// diagnostic dump of the capture atlases (shares the debug zones checkbox)
	if (debugZonesCheck_->isChecked())
	{
		const QString dir = QDir::tempPath() + "/baker_captures";
		QDir().mkpath(dir);
		for (int s = 0; s < int(captures_.size()); s++)
		{
			const BakeGpu::SurfaceCapture &c = captures_[s];
			if (!c.valid())
				continue;
			c.albedo->save(QString("%1/baker_gpu_s%2_alb.png").arg(dir).arg(s).toUtf8().constData());
			c.shading->save(QString("%1/baker_gpu_s%2_sh.png").arg(dir).arg(s).toUtf8().constData());
			c.normal->save(QString("%1/baker_gpu_s%2_n.png").arg(dir).arg(s).toUtf8().constData());
			if (c.emission)
				c.emission->save(QString("%1/baker_gpu_s%2_e.png").arg(dir).arg(s).toUtf8().constData());
		}
		Unigine::Log::message("Baker: capture atlases dumped to %s\n", dir.toUtf8().constData());
	}

	runBake();
}

void BakerWindow::runBake()
{
	// re-resolve everything by ID: nodes may have been deleted while the GPU
	// captures were in flight
	std::vector<BakeCore::BakeGroup> groups;
	for (const IdGroup &ids : bakeGroupIds_)
	{
		BakeCore::BakeGroup g;
		for (int id : ids.highIds)
			if (Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				g.highs.push_back(obj);
		for (int id : ids.lowIds)
			if (Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				g.lows.push_back(obj);
		if (g.highs.size() != ids.highIds.size() || g.lows.size() != ids.lowIds.size())
		{
			abortCapture(tr2("Нода удалена во время запекания.", "A node was deleted during baking."));
			return;
		}
		groups.push_back(std::move(g));
	}

	settings_.gpuCaptures = &captures_;

	auto progress = [this](int percent, const char *text) -> bool {
		progressBar_->setValue(percent);
		statusLabel_->setText(QString::fromUtf8(text));
		QCoreApplication::processEvents();
		return !cancelRequested_;
	};

	BakeCore::Result result = BakeCore::bake(groups, settings_, progress);
	settings_.gpuCaptures = nullptr;
	captures_.clear();

	baking_ = false;
	setUiLocked(false);

	if (result.cancelled)
	{
		progressBar_->setValue(0);
		statusLabel_->setText(tr2("Запекание отменено.", "Baking cancelled."));
		return;
	}
	if (!result.success)
	{
		progressBar_->setValue(0);
		statusLabel_->setText(tr2("Ошибка: %1", "Error: %1").arg(QString::fromUtf8(result.error.get())));
		QMessageBox::warning(this, "Texture Baker", statusLabel_->text());
		return;
	}

	// import the textures BEFORE assigning them to the material: a material
	// referencing a not-yet-imported file binds it raw, bypassing the import
	// pipeline (normal maps render broken until reimported). New files need
	// importAssetSync — reimport fails on unknown assets.
	statusLabel_->setText(tr2("Импорт текстур...", "Importing textures..."));
	QCoreApplication::processEvents();
	for (const Unigine::String &path : {result.albedoPath, result.shadingPath, result.normalPath, result.emissionPath})
	{
		if (path.empty())
			continue;
		const bool wasAsset = UnigineEditor::AssetManager::isAsset(path.get());
		const bool imported = wasAsset
			? UnigineEditor::AssetManager::reimportAssetSync(path.get())
			: UnigineEditor::AssetManager::importAssetSync(path.get());
		if (!imported)
			Unigine::Log::warning("Baker: %s failed for \"%s\"\n",
				wasAsset ? "reimport" : "import", path.get());
	}

	statusLabel_->setText(tr2("Назначение материала...", "Assigning the material..."));
	QCoreApplication::processEvents();
	BakeCore::assignMaterial(groups, result);

	QString report = tr2("Готово. Сохранено:", "Done. Saved:");
	if (!result.albedoPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.albedoPath.get()));
	if (!result.shadingPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.shadingPath.get()));
	if (!result.normalPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.normalPath.get()));
	if (!result.emissionPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.emissionPath.get()));
	if (!result.materialPath.empty())
		report += tr2("\nМатериал: %1", "\nMaterial: %1").arg(QString::fromUtf8(result.materialPath.get()));
	statusLabel_->setText(report);
}
