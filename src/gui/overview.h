#pragma once

#include <QDate>
#include <QString>
#include <QWidget>

#include <map>

#include "vaultik/jobs/job.hpp"

class QLabel;
class QPushButton;

// What a job's start page shows; filled by the main window from the job's history, schedule and
// (once opened) its repository.
struct OverviewData {
    QString warning;  // something needs attention; empty when all is well

    bool has_backup = false;
    vaultik::jobs::RunStatus backup_status = vaultik::jobs::RunStatus::Success;
    QString backup_when;    // "today 02:00"
    QString backup_detail;  // "1,204 files, 3.2 MB new"

    QString next_run;     // "tomorrow 02:00"
    QString next_detail;  // "Daily at 02:00", or how to start a manual job

    QString versions;         // "23 versions"
    QString versions_detail;  // "from 3 Nov 2025 to today"
    QString size;             // "1.2 TB in 30,412 files" (the newest version)
    QString size_detail;      // "Stored: 640 GB, 1.9 TB free on F:"

    bool has_check = false;
    bool check_ok = true;
    QString check_when;
    QString check_detail;
    QString test_text;   // the last test restore, one line ("Test restore 3 days ago: ..."); empty: none
    bool test_ok = true;

    // The drives involved, as Windows reports their health.
    struct Drive {
        QString role;    // "Backup drive", "Second copy", "Backed up from"
        QString name;    // "WD My Passport 2627 (USB HDD, 4.55 TiB)"
        QString status;  // "healthy", or the problem
        bool problem = false;
        int health = -1;         // the health percentage, -1 when unknown
        QString health_unknown;  // why it is unknown (a tooltip)
    };
    std::vector<Drive> drives;

    // Backup results per day (the best one of the day).
    enum class Day { None, Saved, Partial, Failed };
    std::map<QDate, Day> days;
};

// A grid of the last weeks, one square per day, coloured by that day's backups.
class BackupCalendar : public QWidget {
    Q_OBJECT

public:
    explicit BackupCalendar(QWidget* parent = nullptr);
    void set_days(std::map<QDate, OverviewData::Day> days);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    QDate first_day() const;  // the Monday of the first column
    QDate day_at(const QPoint& pos) const;
    int weeks() const;

    std::map<QDate, OverviewData::Day> days_;
};

// A job's start page: what happened last, what happens next, how much is kept, whether the
// backup was checked, and a calendar of backup days.
class OverviewView : public QWidget {
    Q_OBJECT

public:
    explicit OverviewView(QWidget* parent = nullptr);
    void set_data(const OverviewData& data);

signals:
    void back_up_requested();
    void check_requested();
    void test_requested();
    void versions_requested();

private:
    struct Tile {
        QLabel* title = nullptr;
        QLabel* value = nullptr;
        QLabel* detail = nullptr;
    };
    static Tile make_tile(const QString& title, QWidget*& frame);

    QWidget* warning_ = nullptr;
    QLabel* warning_text_ = nullptr;
    Tile last_, next_, versions_, health_;
    BackupCalendar* calendar_ = nullptr;
    QWidget* drives_box_ = nullptr;
    QWidget* drives_list_ = nullptr;  // rebuilt on each set_data
    QPushButton* check_ = nullptr;
    QPushButton* test_ = nullptr;
};
