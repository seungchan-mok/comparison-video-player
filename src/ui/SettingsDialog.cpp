#include "ui/SettingsDialog.h"

#include "engine/Engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace scvp::ui {

SettingsDialog::SettingsDialog(const AppSettings& current, std::function<std::pair<size_t, int>()> frameInfo, QWidget* parent)
    : QDialog(parent)
    , m_settings(current)
    , m_frameInfo(std::move(frameInfo))
{
    setWindowTitle(tr("Settings"));
    auto* tabs = new QTabWidget(this);

    // --- Memory ---------------------------------------------------------------------------
    auto* mem = new QWidget;
    auto* memForm = new QFormLayout(mem);
    m_autoCache = new QCheckBox(tr("Automatic (25 % of physical memory: %1 MB)").arg(Engine::defaultMemoryBudget() >> 20));
    m_cacheMb = new QSpinBox;
    m_cacheMb->setRange(256, 1 << 22);
    m_cacheMb->setSingleStep(256);
    m_cacheMb->setSuffix(QStringLiteral(" MB"));
    m_cacheMb->setValue(int(current.cacheBytes() >> 20));
    m_autoCache->setChecked(current.cacheMb == 0);
    m_cacheMb->setEnabled(current.cacheMb != 0);
    m_cacheInfo = new QLabel;
    m_cacheInfo->setWordWrap(true);
    m_hwdec = new QComboBox;
    m_hwdec->addItem(tr("GPU first (H.264, HEVC, VP8/9, AV1; bit-exact)"), QStringLiteral("gpu"));
    m_hwdec->addItem(tr("CPU only"), QStringLiteral("cpu"));
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
         t = av_hwdevice_iterate_types(t))
        m_hwdec->addItem(tr("Always %1 (any codec it supports)").arg(QString::fromLatin1(HwDevices::name(t))),
                         QString::fromLatin1(HwDevices::name(t)));
    const int hwIndex = m_hwdec->findData(current.hwdec);
    m_hwdec->setCurrentIndex(hwIndex >= 0 ? hwIndex : 0);
    memForm->addRow(tr("Decoding"), m_hwdec);
    memForm->addRow(tr("Decoded frame cache"), m_autoCache);
    memForm->addRow(QString(), m_cacheMb);
    memForm->addRow(QString(), m_cacheInfo);
    connect(m_autoCache, &QCheckBox::toggled, this, [this](bool on) {
        m_cacheMb->setEnabled(!on);
        if (on)
            m_cacheMb->setValue(int(Engine::defaultMemoryBudget() >> 20));
        updateBudgetText();
    });
    connect(m_cacheMb, &QSpinBox::valueChanged, this, &SettingsDialog::updateBudgetText);
    tabs->addTab(mem, tr("Performance"));

    // --- Navigation ------------------------------------------------------------------------
    auto* nav = new QWidget;
    auto* navForm = new QFormLayout(nav);
    m_jumpSmall = new QDoubleSpinBox;
    m_jumpLarge = new QDoubleSpinBox;
    for (QDoubleSpinBox* b : {m_jumpSmall, m_jumpLarge}) {
        b->setRange(0.001, 100000);
        b->setDecimals(3);
    }
    m_jumpSmall->setValue(current.jumps.small);
    m_jumpLarge->setValue(current.jumps.large);
    m_jumpUnit = new QComboBox;
    m_jumpUnit->addItems({tr("seconds"), tr("frames")});
    m_jumpUnit->setCurrentIndex(current.jumps.inFrames ? 1 : 0);
    navForm->addRow(tr("Left / Right jump"), m_jumpSmall);
    navForm->addRow(tr("Down / Up jump"), m_jumpLarge);
    navForm->addRow(tr("Unit"), m_jumpUnit);
    tabs->addTab(nav, tr("Navigation"));

    // --- Keys ------------------------------------------------------------------------------
    auto* keysPage = new QWidget;
    auto* keysLayout = new QVBoxLayout(keysPage);
    m_keys = new QTableWidget(int(Action::Count), 2);
    m_keys->setHorizontalHeaderLabels({tr("Action"), tr("Key")});
    m_keys->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keys->verticalHeader()->hide();
    for (int a = 0; a < int(Action::Count); ++a) {
        auto* name = new QTableWidgetItem(tr(KeyMap::info(Action(a)).label));
        name->setFlags(Qt::ItemIsEnabled);
        m_keys->setItem(a, 0, name);
        auto* edit = new QKeySequenceEdit;
        const auto keys = current.keys.keys(Action(a));
        if (!keys.isEmpty())
            edit->setKeySequence(QKeySequence(keys.first()));
        m_keys->setCellWidget(a, 1, edit);
    }
    auto* reset = new QPushButton(tr("Restore defaults"));
    connect(reset, &QPushButton::clicked, this, [this] {
        const KeyMap d = KeyMap::defaults();
        for (int a = 0; a < int(Action::Count); ++a) {
            auto* edit = static_cast<QKeySequenceEdit*>(m_keys->cellWidget(a, 1));
            const auto keys = d.keys(Action(a));
            edit->setKeySequence(keys.isEmpty() ? QKeySequence() : QKeySequence(keys.first()));
        }
        m_settings.keys = d;
    });
    keysLayout->addWidget(m_keys);
    keysLayout->addWidget(reset, 0, Qt::AlignRight);
    tabs->addTab(keysPage, tr("Keys"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
    resize(560, 520);
    updateBudgetText();
}

void SettingsDialog::updateBudgetText()
{
    const auto [frameBytes, gop] = m_frameInfo ? m_frameInfo() : std::pair<size_t, int>{0, 0};
    if (!frameBytes) {
        m_cacheInfo->setText(tr("Open a video to see how many frames fit."));
        return;
    }
    const size_t budget = size_t(m_cacheMb->value()) << 20;
    const size_t frames = budget / frameBytes;
    QString text = tr("About %1 decoded frames (%2 MB each) at the current resolution.").arg(frames).arg(double(frameBytes) / (1 << 20), 0, 'f', 1);
    if (gop > 0) {
        const double gops = double(frames) / gop;
        text += QLatin1Char(' ') + tr("That is %1 GOPs of %2 frames.").arg(gops, 0, 'f', 1).arg(gop);
        if (gops < 2.0)
            text += QLatin1Char(' ') + tr("Below 2 GOPs, stepping backwards across a GOP boundary has to wait for decoding.");
    }
    m_cacheInfo->setText(text);
}

AppSettings SettingsDialog::result() const
{
    AppSettings s = m_settings;
    s.cacheMb = m_autoCache->isChecked() ? 0 : size_t(m_cacheMb->value());
    s.hwdec = m_hwdec->currentData().toString();
    s.jumps.small = m_jumpSmall->value();
    s.jumps.large = m_jumpLarge->value();
    s.jumps.inFrames = m_jumpUnit->currentIndex() == 1;
    for (int a = 0; a < int(Action::Count); ++a) {
        auto* edit = static_cast<QKeySequenceEdit*>(m_keys->cellWidget(a, 1));
        const QKeySequence seq = edit->keySequence();
        QList<QKeyCombination> keys = s.keys.keys(Action(a));
        if (seq.isEmpty())
            keys.clear();
        else if (keys.isEmpty())
            keys = {seq[0]};
        else
            keys[0] = seq[0];  // edit the primary binding, keep alternates
        s.keys.setKeys(Action(a), keys);
    }
    return s;
}

} // namespace scvp::ui
