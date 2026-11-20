// SPDX-License-Identifier: MIT
#include <KPluginFactory>
#include <KTextEditor/Plugin>
#include <KTextEditor/Command>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>
#include <QAction>
#include <QApplication>
#include <QPointer>
#include <QTimer>
#include <QTreeView>
#include <QLineEdit>
#include <QKeyEvent>
#include <QFileInfo>
#include <QUrl>
#include <QStyledItemDelegate>
#include <QSet>
#include <KTextEditor/Editor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QPalette>
#include <kde_terminal_interface.h>
#include <QStandardPaths>

static void recordDiagnostic(const QString &event, KTextEditor::MainWindow *main = nullptr) {
    const QString path = qEnvironmentVariable("KATE_TERMINAL_DIAGNOSTICS");
    if (path.isEmpty()) return;
    QJsonObject data{{"build", "kate-terminal-theme-1.0"}, {"event", event}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"pid", QCoreApplication::applicationPid()}, {"qt", qVersion()},
        {"Ex_registered", KTextEditor::Editor::instance()->queryCommand("Ex") != nullptr},
        {"ex_registered", KTextEditor::Editor::instance()->queryCommand("ex") != nullptr}};
    if (auto *focus = QApplication::focusWidget()) data["focus"] = focus->metaObject()->className();
    if (main && main->window()) {
        if (auto *view = main->activeView()) data["input_mode"] = int(view->viewInputMode());
        if (auto *action = main->window()->findChild<QAction *>("kate_mdi_toolview_kateproject")) data["projects_visible"] = action->isChecked();
        data["focus_projects_available"] = main->window()->findChild<QAction *>("kate_mdi_focus_toolview_kateproject") != nullptr;
    }
    QFile output(path);
    if (output.open(QIODevice::WriteOnly | QIODevice::Append)) {
        output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        output.write(QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n');
    }
}
// Keep native colored file icons; folders are drawn once in the branch column.
class TerminalTreeDelegate : public QStyledItemDelegate {
public:
    explicit TerminalTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QString path = index.data(Qt::UserRole).toString();
        if (!path.isEmpty() && QFileInfo(path).isDir() && index.model()->hasChildren(index)) {
            option->icon = QIcon();
            option->features &= ~QStyleOptionViewItem::HasDecoration;
        }
    }
};
class ExplorerView : public QObject {
public:
    explicit ExplorerView(KTextEditor::MainWindow *window) : QObject(window), main(window) {
        qApp->installEventFilter(this);
        recordDiagnostic("plugin-view-created", main);
        const QString icon = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("kate-terminal-theme/icons/kate-terminal-black.svg"));
        if (QFile::exists(icon)) {
            const QIcon appIcon(icon);
            QApplication::setWindowIcon(appIcon);
            if (main->window()) main->window()->setWindowIcon(appIcon);
        }
        QTimer::singleShot(0, this, [this] { applyAppearance(); styleTrees(); });
        connect(main, &KTextEditor::MainWindow::pluginViewCreated, this, [this] {
            QTimer::singleShot(0, this, [this] { styleTrees(); });
        });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (opening || !now) return;
            for (QWidget *w = now; w; w = w->parentWidget()) {
                if (auto *view = qobject_cast<KTextEditor::View *>(w)) { if (view->mainWindow() == main) closeTree(); break; }
            }
        });
        connect(main, &KTextEditor::MainWindow::viewCreated, this, [this](KTextEditor::View *) { queueRefresh(); });
        connect(main, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
            if (!opening) closeTree();
            queueRefresh();
        });
    }
    ~ExplorerView() override {
        for (const auto &entry : styledTrees) {
            if (entry.tree && entry.original) entry.tree->setItemDelegate(entry.original);
        }
    }
    bool open(QString &message) {
        recordDiagnostic("Ex-open-request", main);
        styleTrees();
        if (!main || !main->window()) return false;
        auto *focus = main->window()->findChild<QAction *>(QStringLiteral("kate_mdi_focus_toolview_kateproject"));
        if (!focus) {
            message = QStringLiteral("Enable Kate's Project Plugin first.");
            return false;
        }
        opening = true;
        // The native Focus Projects action reveals the panel and focuses the
        // correct project widget, including when its sidebar button is hidden.
        // Run after the Vim command bar has finished restoring editor focus.
        QPointer<QAction> focusAction(focus);
        QTimer::singleShot(0, this, [this, focusAction] {
            if (main && main->window() && focusAction) {
                main->window()->activateWindow();
                focusAction->trigger();
                recordDiagnostic("focus-projects-triggered", main);
            }
            // Keep auto-close suppressed through the native focus transition.
            QTimer::singleShot(0, this, [this] { opening = false; });
        });
        return true;
    }

protected:
    bool eventFilter(QObject *target, QEvent *event) override {
        // Kate's emulated Vim bar has its own command dispatch. Handle Ex there
        // as well as registering it with the ordinary KTextEditor command bar.
        if (!main || !main->window()) return false;
        if (event->type() == QEvent::ApplicationPaletteChange && !applyingAppearance) queueRefresh();
        if (event->type() == QEvent::ChildAdded || event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(target);
            if (widget && main->window()->isAncestorOf(widget)) queueRefresh();
        }
        if (!opening && event->type() == QEvent::MouseButtonPress) {
            for (QWidget *w = qobject_cast<QWidget *>(target); w; w = w->parentWidget()) {
                if (auto *view = qobject_cast<KTextEditor::View *>(w)) {
                    if (view->mainWindow() == main) closeTree();
                    break;
                }
            }
        }
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) return false;
        auto *key = static_cast<QKeyEvent *>(event);
        // Vim navigation belongs to the project tree, never to a search field
        // or the terminal display.
        auto *tree = qobject_cast<QTreeView *>(target);
        if (!tree) {
            if (auto *receiver = qobject_cast<QWidget *>(target)) {
                tree = qobject_cast<QTreeView *>(receiver->parentWidget());
                if (tree && receiver != tree->viewport()) tree = nullptr;
            }
        }
        auto *projects = projectsAction();
        if (tree && projects && projects->isChecked() && main->window()->isAncestorOf(tree) && key->modifiers() == Qt::NoModifier) {
            QModelIndex current = tree->currentIndex();
            const QModelIndex candidate = current.isValid() ? current : tree->model()->index(0, 0);
            const QFileInfo path(candidate.data(Qt::UserRole).toString());
            const bool navigation = key->key() == Qt::Key_J || key->key() == Qt::Key_K;
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            if (path.exists() && (navigation || enter)) {
                if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
                if (navigation) {
                    QKeyEvent move(QEvent::KeyPress, key->key() == Qt::Key_J ? Qt::Key_Down : Qt::Key_Up, Qt::NoModifier);
                    QApplication::sendEvent(tree, &move);
                } else if (path.isDir()) {
                    tree->setExpanded(candidate, !tree->isExpanded(candidate));
                } else if (path.isFile()) {
                    if (auto *view = main->openUrl(QUrl::fromLocalFile(path.absoluteFilePath()))) { closeTree(); view->setFocus(); }
                }
                return true;
            }
        }
        if (key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) return false;
        recordDiagnostic(event->type() == QEvent::ShortcutOverride ? "enter-shortcut-override" : "enter-key-received", main);
        auto *widget = qobject_cast<QWidget *>(target);
        auto *view = main->activeView();
        if (!widget || !main->window()->isAncestorOf(widget) || !view ||
            view->viewInputMode() != KTextEditor::View::ViInputMode) return false;
        for (auto *line : main->window()->findChildren<QLineEdit *>()) {
            // With Vi Input Mode Steal Keys enabled, Kate hides the command
            // bar before forwarding Return to its line edit. The event's
            // actual receiver still identifies that command unambiguously.
            const QString command = line->text().trimmed();
            const bool explorerCommand = command.compare(QLatin1String("Ex"), Qt::CaseInsensitive) == 0 || command.compare(QLatin1String("Explore"), Qt::CaseInsensitive) == 0;
            if (pendingCommand != line && ((line != widget && !line->isVisibleTo(main->window())) || !explorerCommand)) continue;
            bool commandBar = false;
            for (QWidget *p = line->parentWidget(); p; p = p->parentWidget()) {
                if (QByteArray(p->metaObject()->className()) == "KateViewBarWidget") { commandBar = true; break; }
            }
            if (!commandBar) continue;
            if (event->type() == QEvent::ShortcutOverride) {
                pendingCommand = line;
                key->accept();
                recordDiagnostic("Ex-captured-before-dispatch", main);
                return true;
            }
            pendingCommand.clear();
            recordDiagnostic("Ex-intercepted", main);
            if (auto *popup = QApplication::activePopupWidget()) popup->close();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, QString(QChar(0x1b)));
            QApplication::sendEvent(line, &escape);
            for (QWidget *p = line->parentWidget(); p; p = p->parentWidget()) {
                if (QByteArray(p->metaObject()->className()) == "KateViewBar") {
                    QMetaObject::invokeMethod(p, "hideCurrentBarWidget"); break;
                }
            }
            QTimer::singleShot(0, this, [this] { QString message; open(message); });
            return true;
        }
        return false;
    }
private:
    struct StyledTree { QPointer<QTreeView> tree; QPointer<QAbstractItemDelegate> original; };
    QList<StyledTree> styledTrees;
    void styleTrees() {
        if (!main || !main->window()) return;
        applyAppearance();
        applyTerminalProfile();
        for (auto *tree : main->window()->findChildren<QTreeView *>()) {
            wireTree(tree);
            auto *original = tree->itemDelegate();
            if (!original || QByteArray(original->metaObject()->className()) != "QStyledItemDelegate" || original->objectName() == "terminalTreeDelegate") continue;
            bool toolView = false;
            for (QWidget *w = tree; w; w = w->parentWidget()) {
                if (QByteArray(w->metaObject()->className()) == "KateMDI::ToolView") { toolView = true; break; }
            }
            if (!toolView) continue;
            auto *delegate = new TerminalTreeDelegate(this);
            delegate->setObjectName("terminalTreeDelegate");
            styledTrees.append({tree, original});
            tree->setItemDelegate(delegate);
        }
    }
    void applyAppearance() {
        if (applyingAppearance || !main || !main->window()) return;
        applyingAppearance = true;
        // The app can also be started by session restore or a file association,
        // without the launcher's -stylesheet flag. Load the same installed QSS.
        const QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kate-terminal.qss"));
        QFile stylesheet(path);
        if (!path.isEmpty() && stylesheet.open(QIODevice::ReadOnly)) {
            const QString contents = QString::fromUtf8(stylesheet.readAll());
            if (qApp->styleSheet() != contents) qApp->setStyleSheet(contents);
        }
        // KDE can replace QApplication's palette after a desktop theme/focus
        // change. Keep every normal surface black in all activation states.
        QPalette black = qApp->palette();
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Button, QPalette::ToolTipBase})
                black.setColor(group, role, Qt::black);
            for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText})
                black.setColor(group, role, group == QPalette::Disabled ? QColor("#6a737d") : QColor(Qt::white));
        }
        if (qApp->palette() != black) qApp->setPalette(black);
        const QString theme = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("org.kde.syntax-highlighting/themes/GitHub Black.theme"));
        if (!theme.isEmpty()) {
            for (auto *view : main->views()) {
                if (view->configValue(QStringLiteral("theme")).toString() != QLatin1String("GitHub Black"))
                    view->setConfigValue(QStringLiteral("theme"), QStringLiteral("GitHub Black"));
            }
        }
        applyingAppearance = false;
    }
    void applyTerminalProfile() {
        const QList<QObject *> roots{main->window(), main->pluginView(QStringLiteral("katekonsoleplugin"))};
        for (auto *root : roots) {
            if (!root) continue;
            auto objects = root->findChildren<QObject *>();
            objects.prepend(root);
            for (auto *object : objects) {
                if (auto *terminal = qobject_cast<TerminalInterface *>(object)) {
                    if (terminal->currentProfileName() != QLatin1String("Kate GitHub Black") || terminal->profileProperty(QStringLiteral("ColorScheme")).toString() != QLatin1String("KateGitHubBlack"))
                        terminal->setCurrentProfile(QStringLiteral("Kate GitHub Black"));
                }
            }
        }
    }
    QAction *projectsAction() const {
        return main && main->window() ? main->window()->findChild<QAction *>(QStringLiteral("kate_mdi_toolview_kateproject")) : nullptr;
    }
    void closeTree() {
        if (auto *action = projectsAction(); action && action->isChecked()) action->trigger();
    }
    void queueRefresh() {
        if (refreshQueued) return;
        refreshQueued = true;
        QTimer::singleShot(0, this, [this] { refreshQueued = false; styleTrees(); });
    }
    void wireTree(QTreeView *tree) {
        if (wiredTrees.contains(tree)) return;
        wiredTrees.insert(tree);
        tree->setAnimated(false);
        connect(tree, &QObject::destroyed, this, [this, tree] { wiredTrees.remove(tree); });
        connect(tree, &QTreeView::clicked, this, [this](const QModelIndex &index) {
            auto *action = projectsAction();
            if (opening || !action || !action->isChecked() || QApplication::keyboardModifiers() != Qt::NoModifier) return;
            const QFileInfo file(index.data(Qt::UserRole).toString());
            if (!file.isFile()) return;
            const QUrl url = QUrl::fromLocalFile(file.absoluteFilePath());
            QTimer::singleShot(0, this, [this, url] {
                if (!main) return;
                if (auto *view = main->openUrl(url)) { closeTree(); view->setFocus(); }
            });
        });
    }
    QPointer<KTextEditor::MainWindow> main;
    QSet<QTreeView *> wiredTrees;
    QPointer<QLineEdit> pendingCommand;
    bool opening = false;
    bool refreshQueued = false;
    bool applyingAppearance = false;
};

class ExplorerCommand : public KTextEditor::Command {
public:
    explicit ExplorerCommand(QObject *parent)
        : KTextEditor::Command({QStringLiteral("Ex"), QStringLiteral("ex"), QStringLiteral("Explore"), QStringLiteral("explore")}, parent) {}
    QObject *createView(KTextEditor::MainWindow *window) {
        auto *v = new ExplorerView(window);
        views.insert(window, v);
        connect(window, &QObject::destroyed, this, [this, window] { views.remove(window); });
        return v;
    }
    bool exec(KTextEditor::View *view, const QString &cmd, QString &msg, const KTextEditor::Range &) override {
        if (cmd.trimmed().compare(QLatin1String("Ex"), Qt::CaseInsensitive) != 0 && cmd.trimmed().compare(QLatin1String("Explore"), Qt::CaseInsensitive) != 0) {
            msg = QStringLiteral("Usage: Ex"); return false;
        }
        if (!view || !views.contains(view->mainWindow())) return false;
        return views.value(view->mainWindow())->open(msg);
    }
    bool help(KTextEditor::View *, const QString &, QString &msg) override {
        msg = QStringLiteral("Ex: focus the project file tree; returning to a file closes it."); return true;
    }
private:
    QHash<KTextEditor::MainWindow *, ExplorerView *> views;
};
class TerminalExplorer : public KTextEditor::Plugin {
    Q_OBJECT
public:
    TerminalExplorer(QObject *parent, const QVariantList &) : KTextEditor::Plugin(parent), command(new ExplorerCommand(this)) {}
    QObject *createView(KTextEditor::MainWindow *window) override { return command->createView(window); }
private:
    ExplorerCommand *command;
};
K_PLUGIN_FACTORY_WITH_JSON(TerminalExplorerFactory, "plugin.json", registerPlugin<TerminalExplorer>();)
#include "plugin.moc"
