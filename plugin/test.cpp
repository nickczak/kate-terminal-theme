// SPDX-License-Identifier: MIT
#include <KPluginFactory>
#include <KTextEditor/Plugin>
#include <KTextEditor/Editor>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Command>
#include <QApplication>
#include <QPluginLoader>
#include <QAction>
#include <QTreeView>
#include <QHBoxLayout>
#include <QTest>
#include <cstdio>

class Host : public QObject {
    Q_OBJECT
public:
    QWidget shell;
    Q_INVOKABLE QWidget *window() { return &shell; }
    Q_INVOKABLE QList<KTextEditor::View *> views() { return shell.findChildren<KTextEditor::View *>(); }
};
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s at %d\n", #x, __LINE__); return 1; } } while (false)
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    auto *editor=KTextEditor::Editor::instance();
    Host host;
    KTextEditor::MainWindow main(&host);
    auto *layout=new QHBoxLayout(&host.shell);
    auto *tree=new QTreeView(&host.shell); tree->setObjectName("kateproject"); layout->addWidget(tree);
    auto *doc=editor->createDocument(&host);
    auto *view=doc->createView(&host.shell,&main); layout->addWidget(view);
    auto *action=new QAction(&host.shell); action->setObjectName("kate_mdi_toolview_kateproject"); action->setCheckable(true);
    QObject::connect(action,&QAction::toggled,tree,&QWidget::setVisible);
    auto *focusAction=new QAction(&host.shell);
    focusAction->setObjectName("kate_mdi_focus_toolview_kateproject");
    int focusInvocations=0;
    QObject::connect(focusAction,&QAction::triggered,&host,[&] { ++focusInvocations; action->setChecked(true); tree->setFocus(); });
    tree->hide(); host.shell.show(); host.shell.activateWindow(); view->setFocus(); QTest::qWait(30);
    QPluginLoader loader(QString::fromLocal8Bit(argv[1]));
    auto *factory=qobject_cast<KPluginFactory *>(loader.instance()); CHECK(factory);
    auto *plugin=factory->create<KTextEditor::Plugin>(&host); CHECK(plugin);
    QObject *pv=plugin->createView(&main); CHECK(pv);
    auto *cmd=editor->queryCommand("Ex"); CHECK(cmd);
    CHECK(editor->queryCommand("ex")==cmd);
    CHECK(editor->queryCommand("explore")==cmd);
    // Projects opened manually must close even when the editor already has focus.
    action->trigger(); QTest::qWait(20); CHECK(action->isChecked());
    QWidget *editorWidget=view->focusProxy() ? view->focusProxy() : view;
    QTest::mouseClick(editorWidget,Qt::LeftButton,Qt::NoModifier,QPoint(5,5));
    QTest::qWait(20); CHECK(!action->isChecked());
    QString msg;
    CHECK(cmd->exec(view,"Ex",msg)); QTest::qWait(30);
    CHECK(action->isChecked()); CHECK(tree->hasFocus()); CHECK(focusInvocations==1);
    view->setFocus(); QTest::qWait(30); CHECK(!action->isChecked()); CHECK(!tree->isVisible());
    CHECK(cmd->exec(view,"ex",msg)); QTest::qWait(30); CHECK(action->isChecked());
    main.viewChanged(view); QTest::qWait(30); CHECK(!action->isChecked());
    CHECK(!cmd->exec(view,"Ex unsupported",msg));
    delete focusAction;
    delete action;
    CHECK(!cmd->exec(view,"Ex",msg));
    delete pv; delete plugin;
    CHECK(!editor->queryCommand("Ex"));
    delete view;
    puts("PASS: manually opened Projects closes on editor click; Ex reopens it; editor focus and file activation close it; command unloads cleanly.");
}
#include "test.moc"
