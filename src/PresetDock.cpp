// SPDX-License-Identifier: GPL-2.0-or-later

#include "PresetDock.hpp"
#include "ScenePreset.hpp"
#include "ApplyPreset.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cstring>
#include <initializer_list>

namespace {

// (RecFormat2 identifier, human label). Identifiers must match what OBS writes
// for the recording container; labels are what the user sees.
struct FormatEntry {
	const char *id;
	const char *label;
};
const FormatEntry kFormats[] = {
	{"mkv", "Matroska (.mkv)"},
	{"hybrid_mp4", "Hybrid MP4 (.mp4)"},
	{"mp4", "MP4 (.mp4)"},
	{"mov", "QuickTime (.mov)"},
	{"fragmented_mp4", "Fragmented MP4 (.mp4)"},
	{"fragmented_mov", "Fragmented MOV (.mov)"},
	{"mpegts", "MPEG-TS (.ts)"},
	{"flv", "FLV (.flv)"},
};

QSpinBox *makeResSpin(int defaultValue)
{
	auto *s = new QSpinBox();
	s->setRange(1, 16384);
	s->setSingleStep(2);
	s->setValue(defaultValue);
	return s;
}

// obs_enum_audio_monitoring_devices callback: append each (name, id) to the
// combo as (text=name, data=id). Returning true keeps the enumeration going.
bool addMonitorDevice(void *data, const char *name, const char *id)
{
	auto *combo = static_cast<QComboBox *>(data);
	combo->addItem(QString::fromUtf8(name ? name : ""), QString::fromUtf8(id ? id : ""));
	return true;
}

} // namespace

PresetDock::PresetDock(QWidget *parent) : QWidget(parent)
{
	buildUi();
	refreshSceneList();
	loadFromScene();
}

PresetDock::~PresetDock() = default;

void PresetDock::buildUi()
{
	setObjectName("autoResizeOutputDock");
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	// Apply one rich-text tooltip to several widgets (Qt auto-wraps rich text).
	auto setTip = [](const QString &t, std::initializer_list<QWidget *> ws) {
		for (QWidget *w : ws)
			w->setToolTip(t);
	};

	// --- Global "mute to me" toggle ---------------------------------------
	// Lives above the per-scene editor because it is a global, instant control.
	m_muteToMe = new QPushButton();
	m_muteToMe->setCheckable(true);
	m_muteToMe->setToolTip(tr("<b>Mute to me</b><br>One click stops <i>you</i> from hearing monitored "
				  "audio — the recording keeps capturing everything at full volume. Works "
				  "instantly, even while recording, and applies to all scenes.<br><br>It "
				  "routes OBS's monitoring to a silent device; your recording's audio path "
				  "is never touched.<br><br>Tip: only affects audio you hear <i>through OBS "
				  "monitoring</i> (sources set to \"Monitor and Output\"). Audio your system "
				  "plays directly can't be muted this way without also muting the capture."));
	m_muteToMe->setEnabled(obs_audio_monitoring_available());
	m_muteToMe->setChecked(aro_mute_to_me_active());
	m_muteToMe->setText(m_muteToMe->isChecked() ? tr("Muted to you — click to hear again")
						    : tr("Mute to me (stop hearing; keep recording)"));
	root->addWidget(m_muteToMe);

	{
		auto *line = new QFrame();
		line->setFrameShape(QFrame::HLine);
		line->setFrameShadow(QFrame::Sunken);
		root->addWidget(line);
	}

	// --- Scene selector ---------------------------------------------------
	{
		auto *row = new QHBoxLayout();
		row->addWidget(new QLabel(tr("Editing scene:")));
		m_sceneCombo = new QComboBox();
		m_sceneCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		m_sceneCombo->setToolTip(tr("Choose which scene's settings to edit "
					    "(defaults to the current program scene)."));
		row->addWidget(m_sceneCombo, 1);
		root->addLayout(row);
	}

	m_enabled = new QCheckBox(tr("Enable per-scene overrides for this scene"));
	m_enabled->setToolTip(tr("<b>Per-scene overrides</b><br>When on, switching to this scene "
				 "automatically applies the checked items below. Unchecked items "
				 "keep their current value."));
	root->addWidget(m_enabled);

	m_modeLabel = new QLabel();
	m_modeLabel->setStyleSheet("color: gray;");
	root->addWidget(m_modeLabel);

	// --- Video group ------------------------------------------------------
	m_videoGroup = new QGroupBox(tr("Video"));
	{
		auto *g = new QGridLayout(m_videoGroup);
		int r = 0;

		m_useBaseRes = new QCheckBox(tr("Base (canvas) resolution"));
		m_baseCx = makeResSpin(1920);
		m_baseCy = makeResSpin(1080);
		setTip(tr("<b>Base (canvas) resolution</b><br>The overall size of OBS's "
			  "composited canvas — the area shown in the preview. Usually set "
			  "to match your monitor or capture source."),
		       {m_useBaseRes, m_baseCx, m_baseCy});
		g->addWidget(m_useBaseRes, r, 0);
		g->addWidget(m_baseCx, r, 1);
		g->addWidget(new QLabel("x"), r, 2);
		g->addWidget(m_baseCy, r, 3);
		++r;

		m_useOutputRes = new QCheckBox(tr("Output (scaled) resolution"));
		m_outputCx = makeResSpin(1920);
		m_outputCy = makeResSpin(1080);
		setTip(tr("<b>Output (scaled) resolution = clarity / frame size</b><br>The "
			  "pixel size actually recorded; the canvas is scaled to this. E.g. "
			  "a 2560×1440 canvas with 1920×1080 output is downscaled before "
			  "saving. Larger = sharper, but heavier and bigger files."),
		       {m_useOutputRes, m_outputCx, m_outputCy});
		g->addWidget(m_useOutputRes, r, 0);
		g->addWidget(m_outputCx, r, 1);
		g->addWidget(new QLabel("x"), r, 2);
		g->addWidget(m_outputCy, r, 3);
		++r;

		m_useFps = new QCheckBox(tr("FPS (integer)"));
		m_fps = new QSpinBox();
		m_fps->setRange(1, 1000);
		m_fps->setValue(60);
		setTip(tr("<b>FPS (frames per second) = smoothness</b><br>How many frames "
			  "are recorded each second. 30 = normal, 60 = smooth (good for "
			  "games / motion). Higher is smoother but uses more CPU/GPU and "
			  "makes bigger files.<br><br><b>Difference vs bitrate:</b> FPS "
			  "controls how many frames per second (smoothness); bitrate "
			  "controls how much data is spent compressing those frames "
			  "(quality). They are independent."),
		       {m_useFps, m_fps});
		g->addWidget(m_useFps, r, 0);
		g->addWidget(m_fps, r, 1);
		++r;

		auto *note = new QLabel(tr("Resolution / FPS change only while idle. While "
					   "recording or streaming, OBS blocks the change."));
		note->setWordWrap(true);
		note->setStyleSheet("color: gray; font-size: 11px;");
		g->addWidget(note, r, 0, 1, 4);
	}
	root->addWidget(m_videoGroup);

	// --- Recording group --------------------------------------------------
	m_recGroup = new QGroupBox(tr("Recording (applies to the next recording)"));
	{
		auto *g = new QGridLayout(m_recGroup);
		int r = 0;

		m_useRecPath = new QCheckBox(tr("Recording folder"));
		m_recPath = new QLineEdit();
		m_browse = new QPushButton(tr("Browse..."));
		setTip(tr("<b>Recording folder</b><br>Where this scene's recordings are "
			  "saved. Lets you route different scenes to different folders "
			  "(e.g. games vs tutorials)."),
		       {m_useRecPath, m_recPath});
		g->addWidget(m_useRecPath, r, 0);
		g->addWidget(m_recPath, r, 1, 1, 2);
		g->addWidget(m_browse, r, 3);
		++r;

		m_useRecFormat = new QCheckBox(tr("Format"));
		m_recFormat = new QComboBox();
		for (const auto &f : kFormats)
			m_recFormat->addItem(QString::fromUtf8(f.label), QString::fromUtf8(f.id));
		setTip(tr("<b>Recording file format (container)</b><br>mkv is the most "
			  "crash-resistant (a crash rarely corrupts the file); mp4 / "
			  "hybrid_mp4 are widely compatible and editable directly. The "
			  "container itself does not affect quality."),
		       {m_useRecFormat, m_recFormat});
		g->addWidget(m_useRecFormat, r, 0);
		g->addWidget(m_recFormat, r, 1, 1, 3);
		++r;

		m_useAudioTracks = new QCheckBox(tr("Audio tracks"));
		m_useAudioTracks->setToolTip(tr("<b>Audio tracks to record</b><br>Multi-select. E.g. track 1 = "
						"full mix, track 2 = microphone, track 3 = game audio — separate "
						"tracks make later editing easier."));
		g->addWidget(m_useAudioTracks, r, 0);
		auto *tracksRow = new QHBoxLayout();
		for (int i = 0; i < 6; ++i) {
			m_track[i] = new QCheckBox(QString::number(i + 1));
			m_track[i]->setToolTip(tr("Record audio track %1").arg(i + 1));
			tracksRow->addWidget(m_track[i]);
		}
		tracksRow->addStretch(1);
		auto *tracksWrap = new QWidget();
		tracksWrap->setLayout(tracksRow);
		g->addWidget(tracksWrap, r, 1, 1, 3);
		++r;

		m_useRecBitrate = new QCheckBox(tr("Video bitrate (kbps)"));
		m_recBitrate = new QSpinBox();
		m_recBitrate->setRange(100, 300000);
		m_recBitrate->setSingleStep(500);
		m_recBitrate->setValue(6000);
		setTip(tr("<b>Bitrate (kbps) = quality / file size</b><br>How much data per "
			  "second is used to encode the video. Higher = better quality and "
			  "bigger files.<br><br><b>Difference vs resolution / FPS:</b> "
			  "resolution = frame size (clarity), FPS = smoothness, bitrate = "
			  "the per-second data budget (compressed quality). All three are "
			  "independent — e.g. 1080p60 with too low a bitrate looks blurry / "
			  "blocky.<br><br>Reference: 1080p30 ≈ 8000, 1080p60 ≈ 12000, "
			  "1440p60 ≈ 20000 kbps. Advanced output mode only."),
		       {m_useRecBitrate, m_recBitrate});
		g->addWidget(m_useRecBitrate, r, 0);
		g->addWidget(m_recBitrate, r, 1, 1, 3);
		++r;

		auto *brNote = new QLabel(tr("Bitrate override requires Advanced output mode "
					     "(sets the recording encoder to CBR)."));
		brNote->setWordWrap(true);
		brNote->setStyleSheet("color: gray; font-size: 11px;");
		g->addWidget(brNote, r, 0, 1, 4);
	}
	root->addWidget(m_recGroup);

	// --- Audio monitoring -------------------------------------------------
	m_audioGroup = new QGroupBox(tr("Audio monitoring (the device you hear)"));
	{
		auto *g = new QGridLayout(m_audioGroup);
		int r = 0;

		m_useMonitorDevice = new QCheckBox(tr("Monitoring device"));
		m_monitorDevice = new QComboBox();
		m_monitorDevice->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		if (obs_audio_monitoring_available()) {
			obs_enum_audio_monitoring_devices(addMonitorDevice, m_monitorDevice);
		}
		if (m_monitorDevice->count() == 0) {
			// No devices (monitoring unavailable): keep the row usable but inert.
			m_monitorDevice->addItem(tr("(audio monitoring unavailable)"), QString());
			m_useMonitorDevice->setEnabled(false);
			m_monitorDevice->setEnabled(false);
		}
		setTip(tr("<b>Audio monitoring device = what YOU hear</b><br>Chooses which "
			  "playback device OBS sends <i>monitored</i> audio to. This is the "
			  "listening path only — it is completely separate from recording, so "
			  "it changes <b>instantly even while recording</b> and never alters "
			  "what is recorded or how loud it is.<br><br>Use it to stop hearing a "
			  "scene's audio (point monitoring at a device you are not listening "
			  "to) while the recording keeps capturing it at full volume.<br><br>"
			  "Note: only affects sources whose Audio Monitoring is set to "
			  "\"Monitor and Output\" (or \"Monitor Only\") in OBS's audio mixer."),
		       {m_useMonitorDevice, m_monitorDevice});
		g->addWidget(m_useMonitorDevice, r, 0);
		g->addWidget(m_monitorDevice, r, 1, 1, 3);
		++r;

		auto *amNote = new QLabel(tr("Applies immediately, even while recording. Does not "
					     "change the recording itself (content or volume)."));
		amNote->setWordWrap(true);
		amNote->setStyleSheet("color: gray; font-size: 11px;");
		g->addWidget(amNote, r, 0, 1, 4);
	}
	root->addWidget(m_audioGroup);

	// --- Behaviour --------------------------------------------------------
	m_restartRecording = new QCheckBox(tr("If recording when switching to this scene, restart "
					      "recording to apply video changes (creates a new file)"));
	m_restartRecording->setToolTip(tr("OBS cannot change resolution mid-recording. Enabling this "
					  "stops the current recording, applies the new resolution, "
					  "and starts a new recording file."));
	root->addWidget(m_restartRecording);

	// --- Actions ----------------------------------------------------------
	{
		auto *row = new QHBoxLayout();
		m_copyFromCurrent = new QPushButton(tr("Copy from current OBS settings"));
		m_copyFromCurrent->setToolTip(tr("Fill the fields above with OBS's current values as a "
						 "starting point (does not check any boxes)."));
		m_applyNow = new QPushButton(tr("Apply now"));
		m_applyNow->setToolTip(tr("Save and apply now. If the scene being edited is the "
					  "current program scene, it takes effect immediately."));
		row->addWidget(m_copyFromCurrent);
		row->addStretch(1);
		row->addWidget(m_applyNow);
		root->addLayout(row);
	}

	auto *line = new QFrame();
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	root->addWidget(line);

	m_statusLabel = new QLabel();
	m_statusLabel->setWordWrap(true);
	m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
	root->addWidget(m_statusLabel);

	root->addStretch(1);

	// --- Wiring -----------------------------------------------------------
	connect(m_sceneCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&PresetDock::onEditSceneChanged);

	const QList<QCheckBox *> checks = {m_enabled,          m_useBaseRes,       m_useOutputRes,   m_useFps,
					   m_useRecPath,       m_useRecFormat,     m_useAudioTracks, m_useRecBitrate,
					   m_useMonitorDevice, m_restartRecording, m_track[0],       m_track[1],
					   m_track[2],         m_track[3],         m_track[4],       m_track[5]};
	for (QCheckBox *c : checks)
		connect(c, &QCheckBox::toggled, this, &PresetDock::onFieldChanged);

	const QList<QSpinBox *> spins = {m_baseCx, m_baseCy, m_outputCx, m_outputCy, m_fps, m_recBitrate};
	for (QSpinBox *s : spins)
		connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, &PresetDock::onFieldChanged);

	connect(m_recPath, &QLineEdit::textEdited, this, &PresetDock::onFieldChanged);
	connect(m_recFormat, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PresetDock::onFieldChanged);
	connect(m_monitorDevice, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&PresetDock::onFieldChanged);

	connect(m_browse, &QPushButton::clicked, this, &PresetDock::onBrowsePath);
	connect(m_copyFromCurrent, &QPushButton::clicked, this, &PresetDock::onCopyFromCurrent);
	connect(m_applyNow, &QPushButton::clicked, this, &PresetDock::onApplyNow);
	connect(m_muteToMe, &QPushButton::toggled, this, &PresetDock::onMuteToMeToggled);
}

// ---------------------------------------------------------------------------
// Scene list / selection
// ---------------------------------------------------------------------------

void PresetDock::refreshSceneList()
{
	const bool prevLoading = m_loading;
	m_loading = true;

	QString previous = m_sceneCombo->currentText();

	m_sceneCombo->clear();
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *s = scenes.sources.array[i];
		const char *name = obs_source_get_name(s);
		if (name)
			m_sceneCombo->addItem(QString::fromUtf8(name));
	}
	obs_frontend_source_list_free(&scenes);

	// Prefer the previously edited scene; fall back to program scene.
	int idx = previous.isEmpty() ? -1 : m_sceneCombo->findText(previous);
	if (idx < 0) {
		obs_source_t *cur = obs_frontend_get_current_scene();
		if (cur) {
			idx = m_sceneCombo->findText(QString::fromUtf8(obs_source_get_name(cur)));
			obs_source_release(cur);
		}
	}
	if (idx >= 0)
		m_sceneCombo->setCurrentIndex(idx);

	m_loading = prevLoading;
	updateModeLabel();
	if (!m_loading)
		loadFromScene();
}

void PresetDock::onProgramSceneChanged()
{
	updateModeLabel();

	// Only follow the live scene while the user hasn't pinned a different scene
	// to edit (selecting the program scene again re-arms following).
	if (!m_followProgram)
		return;

	obs_source_t *cur = obs_frontend_get_current_scene();
	if (!cur)
		return;
	const int idx = m_sceneCombo->findText(QString::fromUtf8(obs_source_get_name(cur)));
	obs_source_release(cur);
	if (idx >= 0 && idx != m_sceneCombo->currentIndex()) {
		m_programmaticSceneChange = true;
		m_sceneCombo->setCurrentIndex(idx); // triggers loadFromScene
		m_programmaticSceneChange = false;
	}
}

void PresetDock::onEditSceneChanged(int)
{
	// A user-initiated selection decides whether we keep following the program
	// scene: following resumes only when they pick the current program scene.
	if (!m_programmaticSceneChange) {
		obs_source_t *cur = obs_frontend_get_current_scene();
		if (cur) {
			const char *name = obs_source_get_name(cur);
			m_followProgram = name && m_sceneCombo->currentText() == QString::fromUtf8(name);
			obs_source_release(cur);
		}
	}

	if (!m_loading)
		loadFromScene();
}

obs_source_t *PresetDock::editedScene() const
{
	const QString name = m_sceneCombo->currentText();
	if (name.isEmpty())
		return nullptr;
	return obs_get_source_by_name(name.toUtf8().constData());
}

// ---------------------------------------------------------------------------
// Load / save between widgets and the scene preset
// ---------------------------------------------------------------------------

void PresetDock::loadFromScene()
{
	obs_source_t *scene = editedScene();
	const ScenePreset p = preset_load(scene);
	if (scene)
		obs_source_release(scene);

	m_loading = true;

	m_enabled->setChecked(p.enabled);

	m_useBaseRes->setChecked(p.use_base_res);
	m_baseCx->setValue((int)p.base_cx);
	m_baseCy->setValue((int)p.base_cy);

	m_useOutputRes->setChecked(p.use_output_res);
	m_outputCx->setValue((int)p.output_cx);
	m_outputCy->setValue((int)p.output_cy);

	m_useFps->setChecked(p.use_fps);
	m_fps->setValue((int)p.fps);

	m_useRecPath->setChecked(p.use_rec_path);
	m_recPath->setText(QString::fromStdString(p.rec_path));

	m_useRecFormat->setChecked(p.use_rec_format);
	{
		int fi = m_recFormat->findData(QString::fromStdString(p.rec_format));
		m_recFormat->setCurrentIndex(fi >= 0 ? fi : 0);
	}

	m_useAudioTracks->setChecked(p.use_audio_tracks);
	for (int i = 0; i < 6; ++i)
		m_track[i]->setChecked((p.audio_tracks >> i) & 1u);

	m_useRecBitrate->setChecked(p.use_rec_bitrate);
	m_recBitrate->setValue((int)p.rec_bitrate);

	m_useMonitorDevice->setChecked(p.use_monitor_device);
	{
		int di = m_monitorDevice->findData(QString::fromStdString(p.monitor_device_id));
		if (di < 0 && !p.monitor_device_id.empty()) {
			// Saved device isn't currently present (e.g. unplugged). Keep it
			// in the list so the preset still round-trips instead of silently
			// switching to another device on the next edit.
			m_monitorDevice->addItem(QString::fromStdString(p.monitor_device_name.empty()
										? p.monitor_device_id
										: p.monitor_device_name),
						 QString::fromStdString(p.monitor_device_id));
			di = m_monitorDevice->count() - 1;
		}
		if (di >= 0)
			m_monitorDevice->setCurrentIndex(di);
	}

	m_restartRecording->setChecked(p.restart_recording);

	m_loading = false;
	updateEnabledState();
}

void PresetDock::saveToScene()
{
	if (m_loading)
		return;

	obs_source_t *scene = editedScene();
	if (!scene)
		return;

	ScenePreset p;
	p.enabled = m_enabled->isChecked();

	p.use_base_res = m_useBaseRes->isChecked();
	p.base_cx = (uint32_t)m_baseCx->value();
	p.base_cy = (uint32_t)m_baseCy->value();

	p.use_output_res = m_useOutputRes->isChecked();
	p.output_cx = (uint32_t)m_outputCx->value();
	p.output_cy = (uint32_t)m_outputCy->value();

	p.use_fps = m_useFps->isChecked();
	p.fps = (uint32_t)m_fps->value();

	p.use_rec_path = m_useRecPath->isChecked();
	p.rec_path = m_recPath->text().toStdString();

	p.use_rec_format = m_useRecFormat->isChecked();
	p.rec_format = m_recFormat->currentData().toString().toStdString();

	p.use_audio_tracks = m_useAudioTracks->isChecked();
	uint32_t mask = 0;
	for (int i = 0; i < 6; ++i)
		if (m_track[i]->isChecked())
			mask |= (1u << i);
	p.audio_tracks = mask;

	p.use_rec_bitrate = m_useRecBitrate->isChecked();
	p.rec_bitrate = (uint32_t)m_recBitrate->value();

	p.use_monitor_device = m_useMonitorDevice->isChecked();
	p.monitor_device_id = m_monitorDevice->currentData().toString().toStdString();
	p.monitor_device_name = m_monitorDevice->currentText().toStdString();

	p.restart_recording = m_restartRecording->isChecked();

	preset_save(scene, p);
	obs_source_release(scene);
}

void PresetDock::onFieldChanged()
{
	if (m_loading)
		return;
	saveToScene();
	updateEnabledState();
}

void PresetDock::updateEnabledState()
{
	const bool on = m_enabled->isChecked();
	m_videoGroup->setEnabled(on);
	m_recGroup->setEnabled(on);
	m_audioGroup->setEnabled(on);
	m_restartRecording->setEnabled(on);

	m_baseCx->setEnabled(m_useBaseRes->isChecked());
	m_baseCy->setEnabled(m_useBaseRes->isChecked());
	m_outputCx->setEnabled(m_useOutputRes->isChecked());
	m_outputCy->setEnabled(m_useOutputRes->isChecked());
	m_fps->setEnabled(m_useFps->isChecked());
	m_recPath->setEnabled(m_useRecPath->isChecked());
	m_browse->setEnabled(m_useRecPath->isChecked());
	m_recFormat->setEnabled(m_useRecFormat->isChecked());
	for (int i = 0; i < 6; ++i)
		m_track[i]->setEnabled(m_useAudioTracks->isChecked());
	m_recBitrate->setEnabled(m_useRecBitrate->isChecked());

	const bool monAvail = obs_audio_monitoring_available();
	m_useMonitorDevice->setEnabled(on && monAvail);
	m_monitorDevice->setEnabled(on && monAvail && m_useMonitorDevice->isChecked());
}

void PresetDock::updateModeLabel()
{
	config_t *cfg = obs_frontend_get_profile_config();
	const char *mode = cfg ? config_get_string(cfg, "Output", "Mode") : "";
	const bool advanced = mode && std::strcmp(mode, "Advanced") == 0;
	m_modeLabel->setText(tr("Current output mode: %1").arg(advanced ? "Advanced" : "Simple"));
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

void PresetDock::onBrowsePath()
{
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Select recording folder"), m_recPath->text());
	if (!dir.isEmpty()) {
		m_recPath->setText(dir);
		onFieldChanged();
	}
}

void PresetDock::onCopyFromCurrent()
{
	// Fill widget values from OBS's live state (does not toggle the "use_*"
	// checkboxes -- it just gives the user a starting point).
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		m_baseCx->setValue((int)ovi.base_width);
		m_baseCy->setValue((int)ovi.base_height);
		m_outputCx->setValue((int)ovi.output_width);
		m_outputCy->setValue((int)ovi.output_height);
		if (ovi.fps_den)
			m_fps->setValue((int)(ovi.fps_num / ovi.fps_den));
	}

	config_t *cfg = obs_frontend_get_profile_config();
	if (cfg) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		const bool advanced = mode && std::strcmp(mode, "Advanced") == 0;
		const char *section = advanced ? "AdvOut" : "SimpleOutput";

		const char *path = config_get_string(cfg, section, advanced ? "RecFilePath" : "FilePath");
		if (path)
			m_recPath->setText(QString::fromUtf8(path));

		const char *fmt = config_get_string(cfg, section, "RecFormat2");
		if (fmt) {
			int fi = m_recFormat->findData(QString::fromUtf8(fmt));
			if (fi >= 0)
				m_recFormat->setCurrentIndex(fi);
		}

		const uint32_t tracks = (uint32_t)config_get_int(cfg, section, "RecTracks");
		for (int i = 0; i < 6; ++i)
			m_track[i]->setChecked((tracks >> i) & 1u);

		// Recording bitrate lives in recordEncoder.json (Advanced mode only).
		if (advanced) {
			char *profilePath = obs_frontend_get_current_profile_path();
			if (profilePath) {
				const std::string encPath = std::string(profilePath) + "/recordEncoder.json";
				bfree(profilePath);
				obs_data_t *enc = obs_data_create_from_json_file(encPath.c_str());
				if (enc) {
					const long long br = obs_data_get_int(enc, "bitrate");
					if (br > 0)
						m_recBitrate->setValue((int)br);
					obs_data_release(enc);
				}
			}
		}
	}

	// Current audio monitoring device. Skip while "mute to me" is active, since
	// the live device is then the silent sentinel rather than a real choice.
	if (obs_audio_monitoring_available() && !aro_mute_to_me_active()) {
		const char *mdName = nullptr;
		const char *mdId = nullptr;
		obs_get_audio_monitoring_device(&mdName, &mdId);
		if (mdId) {
			int di = m_monitorDevice->findData(QString::fromUtf8(mdId));
			if (di < 0) {
				m_monitorDevice->addItem(QString::fromUtf8(mdName ? mdName : mdId),
							 QString::fromUtf8(mdId));
				di = m_monitorDevice->count() - 1;
			}
			m_monitorDevice->setCurrentIndex(di);
		}
	}

	onFieldChanged();
	showStatus(tr("Copied current OBS settings into the form."));
}

void PresetDock::onApplyNow()
{
	saveToScene();

	// Only apply immediately if the edited scene is the live program scene.
	obs_source_t *edited = editedScene();
	obs_source_t *current = obs_frontend_get_current_scene();
	const bool isProgram = edited && current && edited == current;
	if (edited)
		obs_source_release(edited);

	if (isProgram) {
		aro_apply_preset_for_scene(current);
	} else {
		showStatus(tr("Saved. Settings will apply when you switch to "
			      "this scene."));
	}
	if (current)
		obs_source_release(current);
}

void PresetDock::onMuteToMeToggled(bool checked)
{
	aro_set_mute_to_me(checked);

	// Reflect the actual resulting state (set may be a no-op if monitoring is
	// unavailable) and relabel for the next action.
	const bool active = aro_mute_to_me_active();
	if (active != checked) {
		const QSignalBlocker block(m_muteToMe);
		m_muteToMe->setChecked(active);
	}
	m_muteToMe->setText(active ? tr("Muted to you — click to hear again")
				   : tr("Mute to me (stop hearing; keep recording)"));
}

void PresetDock::showStatus(const QString &message)
{
	m_statusLabel->setText(message);
}
