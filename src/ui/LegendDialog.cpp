#include "ui/LegendDialog.h"

#include "ui/HudOverlay.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace scvp::ui {

LegendDialog::LegendDialog(const std::array<Entry, 2>& entries, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Legend text"));
    auto* form = new QFormLayout;
    QLineEdit* first = nullptr;
    for (int slot = 0; slot < 2; ++slot) {
        const Entry& e = entries[size_t(slot)];
        // The same coloured chip as in the legend, so it is clear which entry is which.
        auto* chip = new QLabel(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"), this);
        chip->setAlignment(Qt::AlignCenter);
        chip->setMinimumWidth(chip->fontMetrics().height() + 8);
        chip->setStyleSheet(QStringLiteral("QLabel { background: %1; color: #141418; font-weight: bold; border-radius: 4px; padding: 2px 6px; }")
                                .arg(HudOverlay::slotColor(slot).name()));
        auto* edit = new QLineEdit(e.open ? e.text : QString(), this);
        edit->setObjectName(slot == 0 ? QStringLiteral("legendA") : QStringLiteral("legendB"));
        edit->setPlaceholderText(e.open ? e.fileName : tr("(no video)"));
        edit->setClearButtonEnabled(true);
        edit->setMinimumWidth(320);
        edit->setEnabled(e.open);
        if (e.open && !first)
            first = edit;
        m_edits[size_t(slot)] = edit;
        form->addRow(chip, edit);
    }
    auto* hint = new QLabel(tr("Empty: the file name."), this);
    hint->setEnabled(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* col = new QVBoxLayout(this);
    col->addLayout(form);
    col->addWidget(hint);
    col->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (first) {
        first->setFocus();
        first->selectAll();  // typing replaces the old text
    }
}

LegendDialog::Texts LegendDialog::texts() const { return {m_edits[0]->text().trimmed(), m_edits[1]->text().trimmed()}; }

std::optional<LegendDialog::Texts> LegendDialog::ask(QWidget* parent, const std::array<Entry, 2>& entries)
{
    LegendDialog d(entries, parent);
    if (d.exec() != QDialog::Accepted)
        return std::nullopt;
    return d.texts();
}

} // namespace scvp::ui
