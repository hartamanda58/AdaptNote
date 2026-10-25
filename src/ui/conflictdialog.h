#pragma once

#include "core/model.h"
#include "sync/conflicts.h"

#include <QDialog>
#include <QHash>
#include <QVector>

class QLabel;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace an {

class Store;

// Side by side view of a sync conflict with three resolutions.
class ConflictDialog : public QDialog {
    Q_OBJECT
public:
    ConflictDialog(Store& store, QWidget* parent = nullptr);

    int conflictCount() const { return pairs_.size(); }
    void reload();
    bool selectPair(int index);
    // Applies the action for the currently selected pair (also used by tests).
    bool resolve(conflicts::Resolution action);

signals:
    void conflictsChanged();

private:
    void updateDetails();

    Store& store_;
    QVector<conflicts::Pair> pairs_;
    QHash<QString, QString> bookNames_;

    QListWidget* list_ = nullptr;
    QLabel* leftTitle_ = nullptr;
    QLabel* rightTitle_ = nullptr;
    QTextBrowser* leftBody_ = nullptr;
    QTextBrowser* rightBody_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* keepOriginal_ = nullptr;
    QPushButton* keepCopy_ = nullptr;
    QPushButton* keepBoth_ = nullptr;
};

}  // namespace an
