// SPDX-License-Identifier: MIT
// Drives and captures the real Kate UI in a disposable demo installation.
#include <KPluginFactory>
#include <KTextEditor/Plugin>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>
#include <KTextEditor/Document>
#include <kde_terminal_interface.h>
#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <cstdio>

class Recording : public QObject {
public:
    Recording(KTextEditor::MainWindow *window) : QObject(window), main(window),
        project(qEnvironmentVariable("KATE_SHOWCASE_PROJECT")),
        output(qEnvironmentVariable("KATE_SHOWCASE_FRAMES")) {
        QTimer::singleShot(1600, this, [this] { start(); });
    }
private:
    template<typename F> void at(int milliseconds, F callback) {
        QTimer::singleShot(milliseconds, this, callback);
    }
    void fail(const char *message) {
        fprintf(stderr, "Showcase failed: %s\n", message);
        captureTimer.stop();
        qApp->exit(2);
    }
    QAction *action(const char *name) {
        return main->window()->findChild<QAction *>(QString::fromLatin1(name));
    }
    void label(const QString &description, const QString &keys = {}) {
        caption = description;
        badge = keys;
    }
    void capture() {
        if (!main || !main->window()) return;
        if (!paused || lastFrame.isNull()) {
            QImage frame(960, 678, QImage::Format_RGB32);
            frame.fill(Qt::black);
            QPainter painter(&frame);
            painter.drawPixmap(0, 0, main->window()->grab());
            painter.fillRect(0, 630, 960, 48, QColor("#000000"));
            painter.setPen(QColor("#21262d"));
            painter.drawLine(0, 630, 960, 630);
            QFont font("Noto Sans Mono");
            font.setPixelSize(14);
            painter.setFont(font);
            painter.setPen(QColor("#e1e4e8"));
            painter.drawText(QRect(20, 641, 650, 26), Qt::AlignLeft | Qt::AlignVCenter, caption);
            painter.setPen(QColor("#79b8ff"));
            painter.drawText(QRect(640, 641, 300, 26), Qt::AlignRight | Qt::AlignVCenter, badge);
            painter.end();
            lastFrame = frame;
        }
        const QString path = QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frameNumber++, 5, 10, QLatin1Char('0')));
        if (!lastFrame.save(path)) fail("could not save frame");
    }
    QTreeView *tree() {
        auto *selected = qobject_cast<QTreeView *>(QApplication::focusWidget());
        if (!selected) fail("Projects did not receive focus");
        return selected;
    }
    QModelIndex named(QTreeView *view, const QString &name) {
        const auto found = view->model()->match(view->model()->index(0, 0), Qt::DisplayRole, name, 1, Qt::MatchExactly | Qt::MatchRecursive);
        if (found.isEmpty()) { fail("sample item missing in Projects"); return {}; }
        return found.first();
    }
    void explore() {
        main->activeView()->setFocus();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTest::keyClicks(QApplication::focusWidget(), ":");
        QApplication::processEvents();
        QTest::qWait(100);
        QTest::keyClicks(QApplication::focusWidget(), "Ex", Qt::NoModifier, 100);
        QTest::qWait(180);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    }
    TerminalInterface *terminal() {
        const QList<QObject *> roots{main->window(), main->pluginView("katekonsoleplugin")};
        for (auto *root : roots) {
            if (!root) continue;
            auto objects = root->findChildren<QObject *>(); objects.prepend(root);
            for (auto *object : objects) if (auto *part = qobject_cast<TerminalInterface *>(object)) return part;
        }
        fail("terminal part not created");
        return nullptr;
    }
    void typeTerminal(const QString &text, int index = 0) {
        auto *part = terminal();
        if (!part) return;
        if (index >= text.size()) { part->sendInput("\n"); return; }
        part->sendInput(text.mid(index, 1));
        at(48, [this, text, index] { typeTerminal(text, index + 1); });
    }
    void clickTab(const QString &filename) {
        for (auto *bar : main->window()->findChildren<QTabBar *>()) {
            if (!bar->isVisibleTo(main->window())) continue;
            for (int i = 0; i < bar->count(); ++i) {
                if (bar->tabText(i).contains(filename)) {
                    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(i).center());
                    return;
                }
            }
        }
        fail("sample document tab not found");
    }
    void start() {
        if (project.isEmpty() || output.isEmpty()) { fail("missing recorder environment"); return; }
        main->window()->setWindowState(Qt::WindowNoState);
        main->window()->resize(960, 630);
        main->window()->activateWindow();
        main->openUrl(QUrl::fromLocalFile(project + "/src/palette.py"));
        main->openUrl(QUrl::fromLocalFile(project + "/README.md"));
        main->openUrl(QUrl::fromLocalFile(project + "/main.py"));
        if (auto *projects = action("kate_mdi_toolview_kateproject"); projects && projects->isChecked()) projects->trigger();
        if (auto *console = action("kate_mdi_toolview_kate_private_plugin_katekonsoleplugin"); console && console->isChecked()) console->trigger();
        main->activeView()->setViewInputMode(KTextEditor::View::ViInputMode);
        main->activeView()->setFocus();
        main->activeView()->removeSelection();
        main->activeView()->setCursorPosition(KTextEditor::Cursor(6, 4));
        label("GitHub colors on a true-black canvas", "Kate Terminal Theme");
        connect(&captureTimer, &QTimer::timeout, this, [this] { capture(); });
        at(250, [this] { captureTimer.start(83); capture(); });
        at(2100, [this] { label("Bring Projects into focus", ":Ex + Enter"); explore(); });
        at(3100, [this] {
            auto *view = tree(); if (!view) return;
            const auto folder = named(view, "src");
            view->setCurrentIndex(folder); view->scrollTo(folder);
            label("Expand folders without leaving the keyboard", "Enter");
            if (!view->isExpanded(folder)) QTest::keyClick(view, Qt::Key_Return);
        });
        at(3850, [this] { label("Move through the file tree", "j / k"); if (auto *v = tree()) QTest::keyClick(v, Qt::Key_J); });
        at(4250, [this] { if (auto *v = tree()) QTest::keyClick(v, Qt::Key_J); });
        at(4650, [this] { if (auto *v = tree()) QTest::keyClick(v, Qt::Key_K); });
        at(5050, [this] { if (auto *v = tree()) QTest::keyClick(v, Qt::Key_J); });
        at(5500, [this] {
            auto *v = tree(); if (!v) return;
            const auto file = named(v, "palette.py"); v->setCurrentIndex(file); v->scrollTo(file);
            label("Open a file; Projects gets out of the way", "Enter");
            QTest::keyClick(v, Qt::Key_Return);
        });
        at(6400, [this] {
            if (!main->activeView()->document()->url().path().endsWith("palette.py")) { fail("Enter did not open palette.py"); return; }
            label("Edit with Kate's Vim mode", "gg  O  ...  Esc");
            QTest::keyClicks(QApplication::focusWidget(), "gg", Qt::NoModifier, 90);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O, Qt::ShiftModifier);
            QTest::keyClicks(QApplication::focusWidget(), "# GitHub Dark. Pure black.", Qt::NoModifier, 48);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        });
        at(8800, [this] { label("Keep several files open", "Tabs"); clickTab("README.md"); });
        at(9700, [this] { clickTab("main.py"); });
        at(10400, [this] {
            label("A matching terminal, right inside Kate", "GitHub Black");
            paused = true;
            auto *console = action("kate_mdi_toolview_kate_private_plugin_katekonsoleplugin");
            if (!console) { fail("terminal tool view not available"); return; }
            if (!console->isChecked()) console->trigger();
        });
        at(11000, [this] {
            auto *part = terminal(); if (!part) return;
            QString directory = project; directory.replace("'", "'\\''");
            part->sendInput("PS1='❯ '; export PS1; cd '" + directory + "'; clear\n");
        });
        at(11500, [this] { paused = false; label("Run the project's checks", "Embedded terminal"); typeTerminal("python3 -m unittest discover -s tests -v"); });
        at(15100, [this] { label("The terminal keeps the same GitHub palette", "python3 main.py"); typeTerminal("python3 main.py"); });
        at(17500, [this] { label("Bring the file tree back whenever you need it", ":Ex + Enter"); explore(); });
        at(18700, [this] { label("Code, Projects, and terminal in one workspace", "j/k  ·  Enter  ·  :Ex"); });
        at(21600, [this] {
            if (auto *console = action("kate_mdi_toolview_kate_private_plugin_katekonsoleplugin"); console && console->isChecked()) console->trigger();
            main->activeView()->setFocus();
            clickTab("main.py");
            main->activeView()->removeSelection();
            main->activeView()->setCursorPosition(KTextEditor::Cursor(6, 4));
            label("GitHub colors on a true-black canvas", "Kate Terminal Theme");
        });
        at(23000, [this] {
            captureTimer.stop();
            fprintf(stderr, "Recorded %d frames of real Kate UI\n", frameNumber);
            qApp->exit(0);
        });
    }
    QPointer<KTextEditor::MainWindow> main;
    QString project, output, caption, badge;
    QTimer captureTimer;
    QImage lastFrame;
    int frameNumber = 0;
    bool paused = false;
};

class ShowcaseRecorder : public KTextEditor::Plugin {
    Q_OBJECT
public:
    ShowcaseRecorder(QObject *parent, const QVariantList &) : KTextEditor::Plugin(parent) {}
    QObject *createView(KTextEditor::MainWindow *window) override { return new Recording(window); }
};
K_PLUGIN_FACTORY_WITH_JSON(ShowcaseRecorderFactory, "showcase_recorder.json", registerPlugin<ShowcaseRecorder>();)
#include "showcase_recorder.moc"
