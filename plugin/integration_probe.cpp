// SPDX-License-Identifier: MIT
#include <KPluginFactory>
#include <KTextEditor/Plugin>
#include <KTextEditor/Editor>
#include <KTextEditor/Command>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>
#include <KTextEditor/Document>
#include <QApplication>
#include <QAction>
#include <QTimer>
#include <QTreeView>
#include <QTest>
#include <QLineEdit>
#include <QPalette>
#include <kde_terminal_interface.h>
#include <cstdio>
class Probe : public KTextEditor::Plugin {
 Q_OBJECT
public:
 Probe(QObject *p,const QVariantList &) : KTextEditor::Plugin(p) {}
 QObject *createView(KTextEditor::MainWindow *w) override {
  auto *context=new QObject(w);
  QTimer::singleShot(1000,context,[w,context] {
   // Reproduce desktop palette and per-view theme resets in the real UI.
   QPalette gray=qApp->palette();
   for(auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled}) {
    gray.setColor(group,QPalette::Window,QColor("#303030"));
    gray.setColor(group,QPalette::Base,QColor("#24292e"));
   }
   qApp->setPalette(gray);
   w->activeView()->setConfigValue("theme",QString("GitHub Dark"));
   w->viewChanged(w->activeView());
   QTest::qWait(100);
   for(auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled}) {
    if(qApp->palette().color(group,QPalette::Window)!=QColor(Qt::black) || qApp->palette().color(group,QPalette::Base)!=QColor(Qt::black)) {
     fprintf(stderr,"FAIL: gray palette was not restored to pure black\n");qApp->exit(13);return;
    }
   }
   if(w->activeView()->configValue("theme").toString()!="GitHub Black") {fprintf(stderr,"FAIL: editor theme was not restored\n");qApp->exit(14);return;}
   auto *console=w->window()->findChild<QAction *>("kate_mdi_toolview_kate_private_plugin_katekonsoleplugin");
   if(console && !console->isChecked()) console->trigger();
   QTest::qWait(200);
   bool terminalFound=false;
   const QList<QObject *> roots{w->window(),w->pluginView("katekonsoleplugin")};
   for(auto *root:roots) {
    if(!root) continue;
    auto objects=root->findChildren<QObject *>(); objects.prepend(root);
    for(auto *object:objects) if(auto *terminal=qobject_cast<TerminalInterface *>(object)) {
     terminalFound=true;
     if(terminal->currentProfileName()!="Kate GitHub Black" || terminal->profileProperty("ColorScheme").toString()!="KateGitHubBlack") {
      fprintf(stderr,"FAIL: terminal profile=%s scheme=%s\n",qPrintable(terminal->currentProfileName()),qPrintable(terminal->profileProperty("ColorScheme").toString()));qApp->exit(8);return;
     }
    }
   }
   if(!terminalFound) {fprintf(stderr,"FAIL: terminal part missing\n");qApp->exit(9);return;}
   if(console && console->isChecked()) console->trigger();
   auto *action=w->window()->findChild<QAction *>("kate_mdi_toolview_kateproject");
   if(!action) {qApp->exit(2);return;}
   if(auto *button=w->window()->findChild<QAction *>("kate_mdi_show_toolview_button_kateproject")) {
    if(button->isChecked()) button->trigger();
   }
   if(action->isChecked()) action->trigger();
   w->window()->activateWindow(); QApplication::processEvents();
   action->trigger();
   QTest::qWait(30);
   QWidget *editorWidget=w->activeView()->focusProxy() ? w->activeView()->focusProxy() : w->activeView();
   QTest::mouseClick(editorWidget,Qt::LeftButton,Qt::NoModifier,QPoint(5,5));
   QTest::qWait(30);
   if(action->isChecked()) {fprintf(stderr,"FAIL: manually opened Projects did not close on editor click\n");qApp->exit(7);return;}
   w->activeView()->setViewInputMode(KTextEditor::View::ViInputMode);
   w->activeView()->setFocus();
   QTest::keyClick(QApplication::focusWidget(),Qt::Key_Escape);
   runCycle(w,context,action,0);

  });
  return context;
 }
 static void runCycle(KTextEditor::MainWindow *w,QObject *context,QAction *action,int cycle) {
   QTest::keyClicks(QApplication::focusWidget(),":");
   QApplication::processEvents();
   QTest::keyClicks(QApplication::focusWidget(),cycle==0 ? "Ex" : cycle==1 ? "ex" : "Explore");
   QApplication::processEvents();
   QTest::keyClick(QApplication::focusWidget(),Qt::Key_Return);
   QTimer::singleShot(1000,context,[w,context,action,cycle] {
    auto *tree=qobject_cast<QTreeView *>(QApplication::focusWidget());
    if(!action->isChecked() || !tree) {fprintf(stderr,"STATE checked=%d focus=%s doc=%s\n",action->isChecked(),QApplication::focusWidget()?QApplication::focusWidget()->metaObject()->className():"none",qPrintable(w->activeView()->document()->text()));fprintf(stderr,"FAIL: Vim Ex cycle %d tree/focus\n",cycle+1);qApp->exit(3);return;}
    auto indexes=tree->model()->match(tree->model()->index(0,0),Qt::DisplayRole,"test.txt",1,Qt::MatchExactly|Qt::MatchRecursive);
    if(indexes.isEmpty()) {fprintf(stderr,"FAIL: missing test file\n");qApp->exit(4);return;}
    const QModelIndex directory=tree->model()->index(0,0);
    tree->setCurrentIndex(directory);
    const QModelIndex below=tree->indexBelow(directory);
    QTest::keyClick(tree,Qt::Key_J);
    if(tree->currentIndex()!=below) {fprintf(stderr,"FAIL: j movement\n");qApp->exit(10);return;}
    QTest::keyClick(tree,Qt::Key_K);
    if(tree->currentIndex()!=directory) {fprintf(stderr,"FAIL: k movement\n");qApp->exit(11);return;}
    const bool wasExpanded=tree->isExpanded(directory);
    QTest::keyClick(tree,Qt::Key_Return);
    if(tree->isExpanded(directory)==wasExpanded || !action->isChecked()) {fprintf(stderr,"FAIL: Enter folder toggle\n");qApp->exit(12);return;}
    QTest::qWait(30);
    tree->setCurrentIndex(indexes.first());
    tree->scrollTo(indexes.first());
    QTest::qWait(30);
    if(cycle==1) QTest::keyClick(tree,Qt::Key_Return);
    else QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(indexes.first()).center());
    QTimer::singleShot(1000,context,[w,context,action,cycle] {
     bool pass=!action->isChecked() && w->activeView()->document()->text()=="Explorer test\n";
     if(!pass) {fprintf(stderr,"FAIL: click/contents cycle %d checked=%d text=%s\n",cycle+1,action->isChecked(),qPrintable(w->activeView()->document()->text()));qApp->exit(5);return;}
     if(cycle<2) runCycle(w,context,action,cycle+1);
     else {fprintf(stderr,"PASS: persistent pure-black palette after gray reset, editor theme, GitHub Black terminal profile, j/k movement, Enter folders/files, three Ex cycles, and mouse auto-close\n");qApp->exit(0);}
    });
   });
 }

};
K_PLUGIN_FACTORY_WITH_JSON(ProbeFactory,"probe.json",registerPlugin<Probe>();)
#include "integration_probe.moc"
