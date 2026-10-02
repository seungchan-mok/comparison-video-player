#pragma once

#include "ui/Settings.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTableWidget;

namespace scvp::ui {

// Edits AppSettings: memory budget (with what it means for the open videos), arrow-key jump
// defaults and key bindings.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // `frameBytes` returns the size of one decoded frame of the open video(s) (0 if none) and the
    // typical GOP length, for the "frames per video" readout.
    SettingsDialog(const AppSettings& current, std::function<std::pair<size_t, int>()> frameInfo, QWidget* parent = nullptr);

    AppSettings result() const;

private:
    void updateBudgetText();

    AppSettings m_settings;
    std::function<std::pair<size_t, int>()> m_frameInfo;
    QComboBox* m_hwdec;
    QCheckBox* m_autoCache;
    QSpinBox* m_cacheMb;
    QCheckBox* m_autoVram;
    QSpinBox* m_vramMb;
    QLabel* m_cacheInfo;
    QDoubleSpinBox* m_jumpSmall;
    QDoubleSpinBox* m_jumpLarge;
    QComboBox* m_jumpUnit;
    QTableWidget* m_keys;
};

} // namespace scvp::ui
