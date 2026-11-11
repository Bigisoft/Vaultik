#include "apps_dialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "runner.h"
#include "theme.h"

namespace {

enum Column { NameColumn, VersionColumn, PublisherColumn, StatusColumn };
constexpr int IdRole = Qt::UserRole;

}  // namespace

AppsDialog::AppsDialog(QStringList repo_args, QString snapshot, QString password, vaultik::CloudCredentials cloud,
                       QWidget* parent)
    : QDialog(parent),
      repo_args_(std::move(repo_args)),
      snapshot_(std::move(snapshot)),
      password_(std::move(password)),
      cloud_(std::move(cloud)) {
    setWindowTitle(tr("Reinstall programs"));
    resize(900, 560);

    intro_ = new QLabel(tr("The programs this computer had when the version was made, and that are not installed "
                           "here. Tick the ones to install: winget installs them one after the other, and asks for "
                           "administrator approval when a program needs it."));
    intro_->setWordWrap(true);

    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("Program"), tr("Version"), tr("Publisher"), tr("Status")});
    list_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    list_->header()->setStretchLastSection(false);
    list_->setColumnWidth(VersionColumn, 120);
    list_->setColumnWidth(PublisherColumn, 200);
    list_->setColumnWidth(StatusColumn, 200);
    list_->setRootIsDecorated(true);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    connect(list_, &QTreeWidget::itemChanged, this, [this] { update_buttons(); });

    bar_ = new QProgressBar;
    bar_->setRange(0, 0);
    status_ = new QLabel(tr("Looking at the programs on this PC..."));
    status_->setTextFormat(Qt::PlainText);  // it shows program names
    status_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    all_ = buttons->addButton(tr("Tick all"), QDialogButtonBox::ActionRole);
    install_ = buttons->addButton(tr("Install"), QDialogButtonBox::AcceptRole);
    set_role(install_, "primary");
    connect(all_, &QPushButton::clicked, this, [this] {
        if (winget_group_ == nullptr) return;
        for (int i = 0; i < winget_group_->childCount(); ++i) {
            QTreeWidgetItem* item = winget_group_->child(i);
            if (item->flags() & Qt::ItemIsUserCheckable) item->setCheckState(NameColumn, Qt::Checked);
        }
    });
    connect(install_, &QPushButton::clicked, this, &AppsDialog::install);
    connect(buttons, &QDialogButtonBox::rejected, this, &AppsDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro_);
    layout->addWidget(list_, 1);
    layout->addWidget(bar_);
    layout->addWidget(status_);
    layout->addWidget(buttons);

    runner_ = new Runner(this);
    connect(runner_, &Runner::event, this, [this](const QJsonObject& ev) {
        const QString type = ev.value("event").toString();
        if (type == "app" && step_ == Step::Loading) {
            const bool installable = ev.value("installable").toBool();
            QTreeWidgetItem* group = installable ? winget_group_ : manual_group_;
            auto* item = new QTreeWidgetItem(group);
            item->setText(NameColumn, ev.value("name").toString());
            item->setText(VersionColumn, ev.value("version").toString());
            item->setText(PublisherColumn, ev.value("publisher").toString());
            if (installable) {
                const QString id = ev.value("id").toString();
                item->setData(NameColumn, IdRole, id);
                item->setToolTip(NameColumn, tr("winget package %1").arg(id));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(NameColumn, Qt::Unchecked);
                by_id_.insert(id, item);
            } else {
                item->setText(StatusColumn, tr("Install by hand"));
            }
        } else if (type == "app_install") {
            if (QTreeWidgetItem* item = by_id_.value(ev.value("id").toString())) {
                item->setText(StatusColumn, tr("Installing..."));
                list_->scrollToItem(item);
            }
            status_->setText(tr("Installing %1 (%2 of %3)...")
                                 .arg(ev.value("name").toString())
                                 .arg(ev.value("index").toInt() + 1)
                                 .arg(ev.value("count").toInt()));
        } else if (type == "app_installed") {
            const bool ok = ev.value("ok").toBool();
            ok ? ++installed_ : ++failed_;
            bar_->setValue(installed_ + failed_);
            if (QTreeWidgetItem* item = by_id_.value(ev.value("id").toString())) {
                const QString message = ev.value("message").toString();
                item->setText(StatusColumn, ok ? tr("Installed") : tr("Failed"));
                item->setToolTip(StatusColumn, message);
                if (ok) {
                    item->setCheckState(NameColumn, Qt::Unchecked);
                    item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
                }
            }
        }
    });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        bar_->hide();
        if (step_ == Step::Loading) {
            step_ = Step::Ready;
            if (result != "success") {
                status_->setText(tr("Cannot list the programs: %1").arg(ev.value("message").toString()));
                update_buttons();
                return;
            }
            const int can = winget_group_->childCount();
            const int by_hand = manual_group_->childCount();
            winget_group_->setText(NameColumn, tr("Can be installed with winget (%1)").arg(can));
            manual_group_->setText(NameColumn, tr("Install by hand (%1)").arg(by_hand));
            winget_group_->setHidden(can == 0);
            manual_group_->setHidden(by_hand == 0);
            QString text;
            if (can + by_hand == 0)
                text = tr("Every program of this version is installed on this PC.");
            else
                text = tr("%1 program(s) of this version are not on this PC; %2 can be installed from here.")
                           .arg(can + by_hand)
                           .arg(can);
            if (!ev.value("winget").toBool())
                text += " " + tr("winget was not found for your account, so programs were compared by name only and "
                                 "none can be installed from here. Install App Installer from the Microsoft Store.");
            status_->setText(text);
            if (!ev.value("winget").toBool()) {
                for (int i = 0; i < can; ++i) {
                    QTreeWidgetItem* item = winget_group_->child(i);
                    item->setData(NameColumn, Qt::CheckStateRole, QVariant());
                    item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
                }
            }
        } else {
            step_ = Step::Ready;
            if (result == "cancelled")
                status_->setText(tr("Stopped. %1 installed, %2 failed.").arg(installed_).arg(failed_));
            else if (result == "failed" && installed_ + failed_ == 0)
                status_->setText(tr("Cannot install: %1").arg(ev.value("message").toString()));
            else
                status_->setText(tr("%1 installed, %2 failed. Hold the pointer over a failed one to see why.")
                                     .arg(installed_)
                                     .arg(failed_));
        }
        update_buttons();
    });

    load();
}

AppsDialog::~AppsDialog() {
    password_.fill(QChar(0));
}

void AppsDialog::load() {
    list_->clear();
    by_id_.clear();
    winget_group_ = new QTreeWidgetItem(list_, {tr("Can be installed with winget")});
    manual_group_ = new QTreeWidgetItem(list_, {tr("Install by hand")});
    for (QTreeWidgetItem* g : {winget_group_, manual_group_}) {
        g->setFlags(Qt::ItemIsEnabled);
        g->setFirstColumnSpanned(true);
        g->setExpanded(true);
    }
    step_ = Step::Loading;
    update_buttons();
    runner_->start(QStringList{"apps", snapshot_, "--missing"} + repo_args_, password_, cloud_);
}

QList<QTreeWidgetItem*> AppsDialog::ticked() const {
    QList<QTreeWidgetItem*> out;
    if (winget_group_ == nullptr) return out;
    for (int i = 0; i < winget_group_->childCount(); ++i) {
        QTreeWidgetItem* item = winget_group_->child(i);
        if ((item->flags() & Qt::ItemIsUserCheckable) && item->checkState(NameColumn) == Qt::Checked) out << item;
    }
    return out;
}

void AppsDialog::update_buttons() {
    bool any_installable = false;
    if (winget_group_ != nullptr)
        for (int i = 0; i < winget_group_->childCount() && !any_installable; ++i)
            any_installable = (winget_group_->child(i)->flags() & Qt::ItemIsUserCheckable) != 0;
    all_->setEnabled(step_ == Step::Ready && any_installable);
    const int n = step_ == Step::Ready ? static_cast<int>(ticked().size()) : 0;
    install_->setEnabled(n > 0);
    install_->setText(n > 0 ? tr("Install %1").arg(n) : tr("Install"));
}

void AppsDialog::install() {
    if (runner_->running()) return;
    const QList<QTreeWidgetItem*> items = ticked();
    if (items.isEmpty()) return;
    QStringList args{"apps", "install"};
    for (QTreeWidgetItem* item : items) {
        args << item->data(NameColumn, IdRole).toString();
        item->setText(StatusColumn, tr("Waiting"));
    }
    args << "--snapshot" << snapshot_;
    args += repo_args_;
    installed_ = 0;
    failed_ = 0;
    step_ = Step::Installing;
    bar_->setRange(0, static_cast<int>(items.size()));
    bar_->setValue(0);
    bar_->show();
    status_->setText(tr("Starting..."));
    update_buttons();
    runner_->start(args, password_, cloud_);
}

void AppsDialog::reject() {
    if (runner_->running()) {
        if (step_ == Step::Installing) {
            const auto answer = QMessageBox::question(
                this, windowTitle(), tr("Programs are being installed. Stop after the one being installed now?"));
            if (answer != QMessageBox::Yes) return;
        }
        runner_->cancel();
    }
    QDialog::reject();
}
