#include <QtWidgets>
#include <QWebChannel>
#include <QFileInfo>
#include <QCoreApplication>
#include "vmdtab.h"
#include "vdocument.h"
#include "vnote.h"
#include "utils/vutils.h"
#include "vpreviewpage.h"
#include "hgmarkdownhighlighter.h"
#include "vconfigmanager.h"
#include "vmarkdownconverter.h"
#include "vnotebook.h"
#include "vtableofcontent.h"
#include "dialog/vfindreplacedialog.h"
#include "veditarea.h"
#include "vconstants.h"
#include "vwebview.h"
#include "vmdeditor.h"
#include "vmainwindow.h"
#include "vsnippet.h"
#include "vinsertselector.h"
#include "vsnippetlist.h"

extern VMainWindow *g_mainWin;

extern VConfigManager *g_config;

VMdTab::VMdTab(VFile *p_file, VEditArea *p_editArea,
               OpenFileMode p_mode, QWidget *p_parent)
    : VEditTab(p_file, p_editArea, p_parent),
      m_editor(NULL),
      m_webViewer(NULL),
      m_document(NULL),
      m_mdConType(g_config->getMdConverterType()),
      m_enableHeadingSequence(false),
      m_backupFileChecked(false)
{
    V_ASSERT(m_file->getDocType() == DocType::Markdown);

    m_file->open();

    HeadingSequenceType headingSequenceType = g_config->getHeadingSequenceType();
    if (headingSequenceType == HeadingSequenceType::Enabled) {
        m_enableHeadingSequence = true;
    } else if (headingSequenceType == HeadingSequenceType::EnabledNoteOnly
               && m_file->getType() == FileType::Note) {
        m_enableHeadingSequence = true;
    }

    setupUI();

    m_backupTimer = new QTimer(this);
    m_backupTimer->setSingleShot(true);
    m_backupTimer->setInterval(g_config->getFileTimerInterval());
    connect(m_backupTimer, &QTimer::timeout,
            this, [this]() {
                writeBackupFile();
            });

    if (p_mode == OpenFileMode::Edit) {
        showFileEditMode();
    } else {
        showFileReadMode();
    }
}

void VMdTab::setupUI()
{
    m_stacks = new QStackedLayout(this);

    setupMarkdownViewer();

    // Setup editor when we really need it.
    m_editor = NULL;

    setLayout(m_stacks);
}

void VMdTab::showFileReadMode()
{
    m_isEditMode = false;

    VHeaderPointer header(m_currentHeader);

    if (m_mdConType == MarkdownConverterType::Hoedown) {
        viewWebByConverter();
    } else {
        m_document->updateText();
        updateOutlineFromHtml(m_document->getToc());
    }

    m_stacks->setCurrentWidget(m_webViewer);
    clearSearchedWordHighlight();

    scrollWebViewToHeader(header);

    updateStatus();
}

bool VMdTab::scrollWebViewToHeader(const VHeaderPointer &p_header)
{
    if (!m_outline.isMatched(p_header)
        || m_outline.getType() != VTableOfContentType::Anchor) {
        return false;
    }

    if (p_header.isValid()) {
        const VTableOfContentItem *item = m_outline.getItem(p_header);
        if (item) {
            if (item->m_anchor.isEmpty()) {
                return false;
            }

            m_currentHeader = p_header;
            m_document->scrollToAnchor(item->m_anchor);
        } else {
            return false;
        }
    } else {
        if (m_outline.isEmpty()) {
            // Let it be.
            m_currentHeader = p_header;
        } else {
            // Scroll to top.
            m_currentHeader = p_header;
            m_document->scrollToAnchor("");
        }
    }

    emit currentHeaderChanged(m_currentHeader);
    return true;
}

bool VMdTab::scrollEditorToHeader(const VHeaderPointer &p_header)
{
    if (!m_outline.isMatched(p_header)
        || m_outline.getType() != VTableOfContentType::BlockNumber) {
        return false;
    }

    VMdEditor *mdEdit = getEditor();

    int blockNumber = -1;
    if (p_header.isValid()) {
        const VTableOfContentItem *item = m_outline.getItem(p_header);
        if (item) {
            blockNumber = item->m_blockNumber;
            if (blockNumber == -1) {
                // Empty item.
                return false;
            }
        } else {
            return false;
        }
    } else {
        if (m_outline.isEmpty()) {
            // No outline and scroll to -1 index.
            // Just let it be.
            m_currentHeader = p_header;
            return true;
        } else {
            // Has outline and scroll to -1 index.
            // Scroll to top.
            blockNumber = 0;
        }
    }

    if (mdEdit->scrollToHeader(blockNumber)) {
        m_currentHeader = p_header;
        return true;
    } else {
        return false;
    }
}

bool VMdTab::scrollToHeaderInternal(const VHeaderPointer &p_header)
{
    if (m_isEditMode) {
        return scrollEditorToHeader(p_header);
    } else {
        return scrollWebViewToHeader(p_header);
    }
}

void VMdTab::viewWebByConverter()
{
    VMarkdownConverter mdConverter;
    QString toc;
    QString html = mdConverter.generateHtml(m_file->getContent(),
                                            g_config->getMarkdownExtensions(),
                                            toc);
    m_document->setHtml(html);
    updateOutlineFromHtml(toc);
}

void VMdTab::showFileEditMode()
{
    VHeaderPointer header(m_currentHeader);

    m_isEditMode = true;

    VMdEditor *mdEdit = getEditor();

    m_stacks->setCurrentWidget(mdEdit);
    mdEdit->beginEdit();

    // If editor is not init, we need to wait for it to init headers.
    // Generally, beginEdit() will generate the headers. Wait is needed when
    // highlight completion is going to re-generate the headers.
    int nrRetry = 5;
    while (header.m_index > -1 && m_outline.isEmpty() && nrRetry-- > 0) {
        qDebug() << "wait another 100 ms for editor's headers ready";
        VUtils::sleepWait(100);
    }

    scrollEditorToHeader(header);

    mdEdit->setFocus();
}

bool VMdTab::closeFile(bool p_forced)
{
    if (p_forced && m_isEditMode) {
        // Discard buffer content
        Q_ASSERT(m_editor);
        m_editor->reloadFile();
        m_editor->endEdit();

        showFileReadMode();
    } else {
        readFile();
    }

    return !m_isEditMode;
}

void VMdTab::editFile()
{
    if (m_isEditMode) {
        return;
    }

    showFileEditMode();
}

void VMdTab::readFile()
{
    if (!m_isEditMode) {
        return;
    }

    if (m_editor && isModified()) {
        // Prompt to save the changes.
        bool modifiable = m_file->isModifiable();
        int ret = VUtils::showMessage(QMessageBox::Information, tr("Information"),
                                      tr("Note <span style=\"%1\">%2</span> has been modified.")
                                        .arg(g_config->c_dataTextStyle).arg(m_file->getName()),
                                      tr("Do you want to save your changes?"),
                                      modifiable ? (QMessageBox::Save
                                                    | QMessageBox::Discard
                                                    | QMessageBox::Cancel)
                                                 : (QMessageBox::Discard
                                                    | QMessageBox::Cancel),
                                      modifiable ? QMessageBox::Save
                                                 : QMessageBox::Cancel,
                                      this);
        switch (ret) {
        case QMessageBox::Save:
            if (!saveFile()) {
                return;
            }

            V_FALLTHROUGH;

        case QMessageBox::Discard:
            m_editor->reloadFile();
            break;

        case QMessageBox::Cancel:
            // Nothing to do if user cancel this action
            return;

        default:
            qWarning() << "wrong return value from QMessageBox:" << ret;
            return;
        }
    }

    if (m_editor) {
        m_editor->endEdit();
    }

    showFileReadMode();
}

bool VMdTab::saveFile()
{
    if (!m_isEditMode) {
        return true;
    }

    Q_ASSERT(m_editor);

    if (!isModified()) {
        return true;
    }

    QString filePath = m_file->fetchPath();

    if (!m_file->isModifiable()) {
        VUtils::showMessage(QMessageBox::Warning,
                            tr("Warning"),
                            tr("Could not modify a read-only note <span style=\"%1\">%2</span>.")
                              .arg(g_config->c_dataTextStyle).arg(filePath),
                            tr("Please save your changes to other notes manually."),
                            QMessageBox::Ok,
                            QMessageBox::Ok,
                            this);
        return false;
    }

    bool ret = true;
    // Make sure the file already exists. Temporary deal with cases when user delete or move
    // a file.
    if (!QFileInfo::exists(filePath)) {
        qWarning() << filePath << "being written has been removed";
        VUtils::showMessage(QMessageBox::Warning, tr("Warning"), tr("Fail to save note."),
                            tr("File <span style=\"%1\">%2</span> being written has been removed.")
                              .arg(g_config->c_dataTextStyle).arg(filePath),
                            QMessageBox::Ok, QMessageBox::Ok, this);
        ret = false;
    } else {
        m_editor->saveFile();
        ret = m_file->save();
        if (!ret) {
            VUtils::showMessage(QMessageBox::Warning,
                                tr("Warning"),
                                tr("Fail to save note."),
                                tr("Fail to write to disk when saving a note. Please try it again."),
                                QMessageBox::Ok,
                                QMessageBox::Ok,
                                this);
            m_editor->setModified(true);
        } else {
            m_fileDiverged = false;
            m_checkFileChange = true;
        }
    }

    updateStatus();

    return ret;
}

bool VMdTab::isModified() const
{
    return (m_editor ? m_editor->isModified() : false) || m_fileDiverged;
}

void VMdTab::saveAndRead()
{
    saveFile();
    readFile();
}

void VMdTab::discardAndRead()
{
    readFile();
}

void VMdTab::setupMarkdownViewer()
{
    m_webViewer = new VWebView(m_file, this);
    connect(m_webViewer, &VWebView::editNote,
            this, &VMdTab::editFile);

    VPreviewPage *page = new VPreviewPage(m_webViewer);
    m_webViewer->setPage(page);
    m_webViewer->setZoomFactor(g_config->getWebZoomFactor());

    // Avoid white flash before loading content.
    page->setBackgroundColor(Qt::transparent);

    m_document = new VDocument(m_file, m_webViewer);

    QWebChannel *channel = new QWebChannel(m_webViewer);
    channel->registerObject(QStringLiteral("content"), m_document);
    connect(m_document, &VDocument::tocChanged,
            this, &VMdTab::updateOutlineFromHtml);
    connect(m_document, SIGNAL(headerChanged(const QString &)),
            this, SLOT(updateCurrentHeader(const QString &)));
    connect(m_document, &VDocument::keyPressed,
            this, &VMdTab::handleWebKeyPressed);
    connect(m_document, &VDocument::logicsFinished,
            this, [this]() {
                if (m_ready & TabReady::ReadMode) {
                    return;
                }

                m_ready |= TabReady::ReadMode;

                tabIsReady(TabReady::ReadMode);
            });

    page->setWebChannel(channel);

    m_webViewer->setHtml(VUtils::generateHtmlTemplate(m_mdConType, false),
                         m_file->getBaseUrl());

    m_stacks->addWidget(m_webViewer);
}

void VMdTab::setupMarkdownEditor()
{
    Q_ASSERT(!m_editor);

    m_editor = new VMdEditor(m_file, m_document, m_mdConType, this);
    m_editor->setProperty("MainEditor", true);
    connect(m_editor, &VMdEditor::headersChanged,
            this, &VMdTab::updateOutlineFromHeaders);
    connect(m_editor, SIGNAL(currentHeaderChanged(int)),
            this, SLOT(updateCurrentHeader(int)));
    connect(m_editor, &VMdEditor::statusChanged,
            this, &VMdTab::updateStatus);
    connect(m_editor, &VMdEditor::textChanged,
            this, &VMdTab::updateStatus);
    connect(m_editor, &VMdEditor::cursorPositionChanged,
            this, &VMdTab::updateCursorStatus);
    connect(g_mainWin, &VMainWindow::editorConfigUpdated,
            m_editor, &VMdEditor::updateConfig);
    connect(m_editor->object(), &VEditorObject::saveAndRead,
            this, &VMdTab::saveAndRead);
    connect(m_editor->object(), &VEditorObject::discardAndRead,
            this, &VMdTab::discardAndRead);
    connect(m_editor->object(), &VEditorObject::saveNote,
            this, &VMdTab::saveFile);
    connect(m_editor->object(), &VEditorObject::statusMessage,
            this, &VEditTab::statusMessage);
    connect(m_editor->object(), &VEditorObject::vimStatusUpdated,
            this, &VEditTab::vimStatusUpdated);
    connect(m_editor->object(), &VEditorObject::requestCloseFindReplaceDialog,
            this, [this]() {
                this->m_editArea->getFindReplaceDialog()->closeDialog();
            });
    connect(m_editor->object(), &VEditorObject::ready,
            this, [this]() {
                if (m_ready & TabReady::EditMode) {
                    return;
                }

                m_ready |= TabReady::EditMode;

                tabIsReady(TabReady::EditMode);
            });

    enableHeadingSequence(m_enableHeadingSequence);
    m_editor->reloadFile();
    m_stacks->addWidget(m_editor);
}

void VMdTab::updateOutlineFromHtml(const QString &p_tocHtml)
{
    if (m_isEditMode) {
        return;
    }

    m_outline.clear();

    if (m_outline.parseTableFromHtml(p_tocHtml)) {
        m_outline.setFile(m_file);
        m_outline.setType(VTableOfContentType::Anchor);
    }

    m_currentHeader.reset();

    emit outlineChanged(m_outline);
}

void VMdTab::updateOutlineFromHeaders(const QVector<VTableOfContentItem> &p_headers)
{
    if (!m_isEditMode) {
        return;
    }

    m_outline.update(m_file,
                     p_headers,
                     VTableOfContentType::BlockNumber);

    m_currentHeader.reset();

    emit outlineChanged(m_outline);
}

void VMdTab::scrollToHeader(const VHeaderPointer &p_header)
{
    if (m_outline.isMatched(p_header)) {
        // Scroll only when @p_header is valid.
        scrollToHeaderInternal(p_header);
    }
}

void VMdTab::updateCurrentHeader(const QString &p_anchor)
{
    if (m_isEditMode) {
        return;
    }

    // Find the index of the anchor in outline.
    int idx = m_outline.indexOfItemByAnchor(p_anchor);
    m_currentHeader.update(m_file, idx);

    emit currentHeaderChanged(m_currentHeader);
}

void VMdTab::updateCurrentHeader(int p_blockNumber)
{
    if (!m_isEditMode) {
        return;
    }

    // Find the index of the block number in outline.
    int idx = m_outline.indexOfItemByBlockNumber(p_blockNumber);
    m_currentHeader.update(m_file, idx);

    emit currentHeaderChanged(m_currentHeader);
}

void VMdTab::insertImage()
{
    if (!m_isEditMode) {
        return;
    }

    Q_ASSERT(m_editor);
    m_editor->insertImage();
}

void VMdTab::insertLink()
{
    if (!m_isEditMode) {
        return;
    }

    Q_ASSERT(m_editor);
    m_editor->insertLink();
}

void VMdTab::findText(const QString &p_text, uint p_options, bool p_peek,
                      bool p_forward)
{
    if (m_isEditMode) {
        Q_ASSERT(m_editor);
        if (p_peek) {
            m_editor->peekText(p_text, p_options);
        } else {
            m_editor->findText(p_text, p_options, p_forward);
        }
    } else {
        findTextInWebView(p_text, p_options, p_peek, p_forward);
    }
}

void VMdTab::replaceText(const QString &p_text, uint p_options,
                         const QString &p_replaceText, bool p_findNext)
{
    if (m_isEditMode) {
        Q_ASSERT(m_editor);
        m_editor->replaceText(p_text, p_options, p_replaceText, p_findNext);
    }
}

void VMdTab::replaceTextAll(const QString &p_text, uint p_options,
                            const QString &p_replaceText)
{
    if (m_isEditMode) {
        Q_ASSERT(m_editor);
        m_editor->replaceTextAll(p_text, p_options, p_replaceText);
    }
}

void VMdTab::findTextInWebView(const QString &p_text, uint p_options,
                               bool /* p_peek */, bool p_forward)
{
    V_ASSERT(m_webViewer);

    QWebEnginePage::FindFlags flags;
    if (p_options & FindOption::CaseSensitive) {
        flags |= QWebEnginePage::FindCaseSensitively;
    }

    if (!p_forward) {
        flags |= QWebEnginePage::FindBackward;
    }

    m_webViewer->findText(p_text, flags);
}

QString VMdTab::getSelectedText() const
{
    if (m_isEditMode) {
        Q_ASSERT(m_editor);
        QTextCursor cursor = m_editor->textCursor();
        return cursor.selectedText();
    } else {
        return m_webViewer->selectedText();
    }
}

void VMdTab::clearSearchedWordHighlight()
{
    if (m_webViewer) {
        m_webViewer->findText("");
    }

    if (m_editor) {
        m_editor->clearSearchedWordHighlight();
    }
}

void VMdTab::handleWebKeyPressed(int p_key, bool p_ctrl, bool /* p_shift */)
{
    V_ASSERT(m_webViewer);

    switch (p_key) {
    // Esc
    case 27:
        m_editArea->getFindReplaceDialog()->closeDialog();
        break;

    // Dash
    case 189:
        if (p_ctrl) {
            // Zoom out.
            zoomWebPage(false);
        }
        break;

    // Equal
    case 187:
        if (p_ctrl) {
            // Zoom in.
            zoomWebPage(true);
        }
        break;

    // 0
    case 48:
        if (p_ctrl) {
            // Recover zoom.
            m_webViewer->setZoomFactor(1);
        }
        break;

    default:
        break;
    }
}

void VMdTab::zoom(bool p_zoomIn, qreal p_step)
{
    if (m_isEditMode) {
        // TODO
    } else {
        zoomWebPage(p_zoomIn, p_step);
    }
}

void VMdTab::zoomWebPage(bool p_zoomIn, qreal p_step)
{
    V_ASSERT(m_webViewer);

    qreal curFactor = m_webViewer->zoomFactor();
    qreal newFactor = p_zoomIn ? curFactor + p_step : curFactor - p_step;
    if (newFactor < c_webZoomFactorMin) {
        newFactor = c_webZoomFactorMin;
    } else if (newFactor > c_webZoomFactorMax) {
        newFactor = c_webZoomFactorMax;
    }

    m_webViewer->setZoomFactor(newFactor);
}

VWebView *VMdTab::getWebViewer() const
{
    return m_webViewer;
}

MarkdownConverterType VMdTab::getMarkdownConverterType() const
{
    return m_mdConType;
}

void VMdTab::focusChild()
{
    m_stacks->currentWidget()->setFocus();
}

void VMdTab::requestUpdateVimStatus()
{
    if (m_editor) {
        m_editor->requestUpdateVimStatus();
    } else {
        emit vimStatusUpdated(NULL);
    }
}

VEditTabInfo VMdTab::fetchTabInfo(VEditTabInfo::InfoType p_type) const
{
    VEditTabInfo info = VEditTab::fetchTabInfo(p_type);

    if (m_editor) {
        QTextCursor cursor = m_editor->textCursor();
        info.m_cursorBlockNumber = cursor.block().blockNumber();
        info.m_cursorPositionInBlock = cursor.positionInBlock();
        info.m_blockCount = m_editor->document()->blockCount();
    }

    info.m_headerIndex = m_currentHeader.m_index;

    return info;
}

void VMdTab::decorateText(TextDecoration p_decoration, int p_level)
{
    if (m_editor) {
        m_editor->decorateText(p_decoration, p_level);
    }
}

bool VMdTab::restoreFromTabInfo(const VEditTabInfo &p_info)
{
    if (p_info.m_editTab != this) {
        return false;
    }

    // Restore header.
    VHeaderPointer header(m_file, p_info.m_headerIndex);
    bool ret = scrollToHeaderInternal(header);
    return ret;
}

void VMdTab::restoreFromTabInfo()
{
    restoreFromTabInfo(m_infoToRestore);

    // Clear it anyway.
    m_infoToRestore.clear();
}

void VMdTab::enableHeadingSequence(bool p_enabled)
{
    m_enableHeadingSequence = p_enabled;

    if (m_editor) {
        VEditConfig &config = m_editor->getConfig();
        config.m_enableHeadingSequence = m_enableHeadingSequence;
    }
}

bool VMdTab::isHeadingSequenceEnabled() const
{
    return m_enableHeadingSequence;
}

void VMdTab::evaluateMagicWords()
{
    if (isEditMode() && m_file->isModifiable()) {
        getEditor()->evaluateMagicWords();
    }
}

void VMdTab::applySnippet(const VSnippet *p_snippet)
{
    Q_ASSERT(p_snippet);

    if (isEditMode()
        && m_file->isModifiable()
        && p_snippet->getType() == VSnippet::Type::PlainText) {
        Q_ASSERT(m_editor);
        QTextCursor cursor = m_editor->textCursor();
        bool changed = p_snippet->apply(cursor);
        if (changed) {
            m_editor->setTextCursor(cursor);

            m_editor->setVimMode(VimMode::Insert);

            g_mainWin->showStatusMessage(tr("Snippet applied"));

            focusTab();
        }
    } else {
        g_mainWin->showStatusMessage(tr("Snippet %1 is not applicable").arg(p_snippet->getName()));
    }
}

void VMdTab::applySnippet()
{
    if (!isEditMode() || !m_file->isModifiable()) {
        g_mainWin->showStatusMessage(tr("Snippets are not applicable"));
        return;
    }

    QPoint pos(m_editor->cursorRect().bottomRight());
    QMenu menu(this);
    VInsertSelector *sel = prepareSnippetSelector(&menu);
    if (!sel) {
        g_mainWin->showStatusMessage(tr("No available snippets defined with shortcuts"));
        return;
    }

    QWidgetAction *act = new QWidgetAction(&menu);
    act->setDefaultWidget(sel);
    connect(sel, &VInsertSelector::accepted,
            this, [this, &menu]() {
                QKeyEvent *escEvent = new QKeyEvent(QEvent::KeyPress, Qt::Key_Escape,
                                                    Qt::NoModifier);
                QCoreApplication::postEvent(&menu, escEvent);
            });

    menu.addAction(act);

    menu.exec(m_editor->mapToGlobal(pos));

    QString chosenItem = sel->getClickedItem();
    if (!chosenItem.isEmpty()) {
        const VSnippet *snip = g_mainWin->getSnippetList()->getSnippet(chosenItem);
        if (snip) {
            applySnippet(snip);
        }
    }
}

static bool selectorItemCmp(const VInsertSelectorItem &p_a, const VInsertSelectorItem &p_b)
{
    if (p_a.m_shortcut < p_b.m_shortcut) {
        return true;
    }

    return false;
}

VInsertSelector *VMdTab::prepareSnippetSelector(QWidget *p_parent)
{
    auto snippets = g_mainWin->getSnippetList()->getSnippets();
    QVector<VInsertSelectorItem> items;
    for (auto const & snip : snippets) {
        if (!snip.getShortcut().isNull()) {
            items.push_back(VInsertSelectorItem(snip.getName(),
                                                snip.getName(),
                                                snip.getShortcut()));
        }
    }

    if (items.isEmpty()) {
        return NULL;
    }

    // Sort items by shortcut.
    std::sort(items.begin(), items.end(), selectorItemCmp);

    VInsertSelector *sel = new VInsertSelector(7, items, p_parent);
    return sel;
}

void VMdTab::reload()
{
    if (m_isEditMode) {
        m_editor->reloadFile();
        m_editor->endEdit();
        m_editor->beginEdit();
        updateStatus();
    } else {
        if (m_editor) {
            m_editor->reloadFile();
        }

        showFileReadMode();
    }
}

void VMdTab::tabIsReady(TabReady p_mode)
{
    bool isCurrentMode = (m_isEditMode && p_mode == TabReady::EditMode)
                         || (!m_isEditMode && p_mode == TabReady::ReadMode);

    if (isCurrentMode) {
        restoreFromTabInfo();

        if (m_enableBackupFile
            && !m_backupFileChecked
            && m_file->isModifiable()) {
            if (!checkPreviousBackupFile()) {
                return;
            }
        }
    }

    if (m_enableBackupFile
        && m_file->isModifiable()
        && p_mode == TabReady::EditMode) {
        // contentsChanged will be emitted even the content is not changed.
        connect(m_editor->document(), &QTextDocument::contentsChange,
                this, [this]() {
                    if (m_isEditMode) {
                        m_backupTimer->stop();
                        m_backupTimer->start();
                    }
                });
    }
}

void VMdTab::writeBackupFile()
{
    Q_ASSERT(m_enableBackupFile && m_file->isModifiable());
    m_file->writeBackupFile(m_editor->getContent());
}

bool VMdTab::checkPreviousBackupFile()
{
    m_backupFileChecked = true;

    QString preFile = m_file->backupFileOfPreviousSession();
    if (preFile.isEmpty()) {
        return true;
    }

    QMessageBox box(QMessageBox::Warning,
                    tr("Backup File Found"),
                    tr("Found backup file <span style=\"%1\">%2</span> "
                       "when opening note <span style=\"%1\">%3</span>.")
                      .arg(g_config->c_dataTextStyle)
                      .arg(preFile)
                      .arg(m_file->fetchPath()),
                    QMessageBox::NoButton,
                    this);
    QString backupContent = m_file->readBackupFile(preFile);
    QString info = tr("VNote may crash while editing this note before.<br/>"
                      "Please choose to recover from the backup file or delete it.<br/><br/>"
                      "Note file last modified: <span style=\"%1\">%2</span><br/>"
                      "Backup file last modified: <span style=\"%1\">%3</span><br/>"
                      "Content comparison: <span style=\"%1\">%4</span>")
                     .arg(g_config->c_dataTextStyle)
                     .arg(VUtils::displayDateTime(QFileInfo(m_file->fetchPath()).lastModified()))
                     .arg(VUtils::displayDateTime(QFileInfo(preFile).lastModified()))
                     .arg(m_file->getContent() == backupContent ? tr("Identical")
                                                                : tr("Different"));
    box.setInformativeText(info);
    QPushButton *recoverBtn = box.addButton(tr("Recover From Backup File"), QMessageBox::YesRole);
    box.addButton(tr("Discard Backup File"), QMessageBox::NoRole);
    QPushButton *cancelBtn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);

    box.setDefaultButton(cancelBtn);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);

    box.exec();
    QAbstractButton *btn = box.clickedButton();
    if (btn == cancelBtn || !btn) {
        // Close current tab.
        emit closeRequested(this);
        return false;
    } else if (btn == recoverBtn) {
        // Load content from the backup file.
        if (!m_isEditMode) {
            showFileEditMode();
        }

        Q_ASSERT(m_editor);
        m_editor->setContent(backupContent, true);

        updateStatus();
    }

    VUtils::deleteFile(preFile);

    return true;
}

void VMdTab::updateCursorStatus()
{
    emit statusUpdated(fetchTabInfo(VEditTabInfo::InfoType::Cursor));
}
