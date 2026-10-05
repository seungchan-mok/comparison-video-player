#pragma once

#include <QDialog>

#include <array>
#include <optional>

class QLineEdit;

namespace scvp::ui {

// Asks for the text each video's legend entry shows instead of its file name: the name of a
// method or a setting reads better in screenshots than out.mp4 twice. An empty field means the
// file name; videos that are not open cannot be named.
class LegendDialog : public QDialog {
    Q_OBJECT
public:
    struct Entry {
        QString text;      // the user's text so far ("" = the file name)
        QString fileName;  // shown as the placeholder
        bool open = false;
    };
    using Texts = std::array<QString, 2>;

    explicit LegendDialog(const std::array<Entry, 2>& entries, QWidget* parent = nullptr);

    Texts texts() const;  // trimmed
    static std::optional<Texts> ask(QWidget* parent, const std::array<Entry, 2>& entries);

private:
    std::array<QLineEdit*, 2> m_edits{};
};

} // namespace scvp::ui
