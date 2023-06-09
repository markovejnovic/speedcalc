// This file is part of the SpeedCrunch project
// Copyright (C) 2007 Ariya Hidayat <ariya@kde.org>
// Copyright (C) 2004, 2005 Ariya Hidayat <ariya@kde.org>
// Copyright (C) 2005, 2006 Johan Thelin <e8johan@gmail.com>
// Copyright (C) 2007-2016 @heldercorreia
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
// Boston, MA 02110-1301, USA.

#include "gui/editor.h"
#include "gui/syntaxhighlighter.h"
#include "core/constants.h"
#include "core/evaluator.h"
#include "core/functions.h"
#include "core/numberformatter.h"
#include "core/settings.h"
#include "core/session.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStyle>
#include <QTimeLine>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <algorithm>

static void moveCursorToEnd(Editor* editor)
{
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor->setTextCursor(cursor);
}

Editor::Editor(QWidget* parent)
    : QPlainTextEdit(parent)
{
    m_evaluator = Evaluator::instance();
    m_currentHistoryIndex = 0;
    m_isAutoCompletionEnabled = true;
    m_completion = new EditorCompletion(this);
    m_constantCompletion = 0;
    m_completionTimer = new QTimer(this);
    m_isAutoCalcEnabled = true;
    m_highlighter = new SyntaxHighlighter(this);
    m_matchingTimer = new QTimer(this);
    m_shouldPaintCustomCursor = true;

    setViewportMargins(0, 0, 0, 0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setTabChangesFocus(true);
    setWordWrapMode(QTextOption::NoWrap);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setCursorWidth(0);

    connect(m_completion, &EditorCompletion::selectedCompletion,
            this, &Editor::autoComplete);
    connect(m_completionTimer, SIGNAL(timeout()), SLOT(triggerAutoComplete()));
    connect(m_matchingTimer, SIGNAL(timeout()), SLOT(doMatchingPar()));
    connect(this, &Editor::selectionChanged, this, &Editor::checkSelectionAutoCalc);
    connect(this, &Editor::textChanged, this, &Editor::checkAutoCalc);
    connect(this, &Editor::textChanged, this, &Editor::checkAutoComplete);
    connect(this, &Editor::textChanged, this, &Editor::checkMatching);

    adjustSize();
    setFixedHeight(sizeHint().height());
}

void Editor::refreshAutoCalc()
{
    if (m_isAutoCalcEnabled) {
      if (!textCursor().selectedText().isEmpty())
          checkSelectionAutoCalc();
      else
          checkAutoCalc();
    }
}

QString Editor::text() const
{
    return toPlainText();
}

void Editor::setText(const QString& text)
{
    setPlainText(text);
}

void Editor::insert(const QString& text)
{
    insertPlainText(text);
}

void Editor::doBackspace()
{
    QTextCursor cursor = textCursor();
    cursor.deletePreviousChar();
    setTextCursor(cursor);
}

char Editor::radixChar() const
{
    return Settings::instance()->radixCharacter();
}

int Editor::cursorPosition() const
{
    return textCursor().position();
}

void Editor::setCursorPosition(int position)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(position);
    setTextCursor(cursor);
}

QSize Editor::sizeHint() const
{
    ensurePolished();
    const QFontMetrics metrics = fontMetrics();
    const int width = metrics.horizontalAdvance('x') * 10;
    const int height = metrics.lineSpacing() + 6;
    return QSize(width, height);
}

void Editor::clearHistory()
{
    m_history.clear();
    m_currentHistoryIndex = 0;
}

bool Editor::isAutoCompletionEnabled() const
{
    return m_isAutoCompletionEnabled;
}

void Editor::setAutoCompletionEnabled(bool enable)
{
    m_isAutoCompletionEnabled = enable;
}

bool Editor::isAutoCalcEnabled() const
{
    return m_isAutoCalcEnabled;
}

void Editor::setAutoCalcEnabled(bool enable)
{
    m_isAutoCalcEnabled = enable;
}

void Editor::checkAutoComplete()
{
    if (!m_isAutoCompletionEnabled)
        return;

    m_completionTimer->stop();
    m_completionTimer->setSingleShot(true);
    m_completionTimer->start();
}


void Editor::checkMatching()
{
    if (!Settings::instance()->syntaxHighlighting)
        return;

    m_matchingTimer->stop();
    m_matchingTimer->setSingleShot(true);
    m_matchingTimer->start();
}

void Editor::checkAutoCalc()
{
    if (m_isAutoCalcEnabled)
        autoCalc();
}

void Editor::doMatchingPar()
{
    // Clear previous.
    setExtraSelections(QList<QTextEdit::ExtraSelection>());

    if (!Settings::instance()->syntaxHighlighting)
        return;

    doMatchingLeft();
    doMatchingRight();
}

void Editor::checkSelectionAutoCalc()
{
    if (m_isAutoCalcEnabled)
        autoCalcSelection();
}

void Editor::doMatchingLeft()
{
    // Tokenize the expression.
    const int currentPosition = textCursor().position();

    // Check for right par.
    QString subtext = text().left(currentPosition);
    Tokens tokens = m_evaluator->scan(subtext);
    if (!tokens.valid() || tokens.count() < 1)
        return;
    Token lastToken = tokens.at(tokens.count() - 1);

    // Right par?
    if (lastToken.type() == Token::stxClosePar
        && (lastToken.pos() + lastToken.size()) == currentPosition) {
        // Find the matching left par.
        unsigned par = 1;
        int matchPosition = -1;
        int closeParPos = lastToken.pos() + lastToken.size() - 1;

        for (int i = tokens.count() - 2; i >= 0 && par > 0; --i) {
            Token matchToken = tokens.at(i);
            switch (matchToken.type()) {
                case Token::stxOpenPar : --par; break;
                case Token::stxClosePar: ++par; break;
                default:;
            }
            matchPosition = matchToken.pos() + matchToken.size() - 1;
        }

        if (par == 0) {
            QTextEdit::ExtraSelection hilite1;
            hilite1.cursor = textCursor();
            hilite1.cursor.setPosition(matchPosition);
            hilite1.cursor.setPosition(matchPosition + 1,
                                       QTextCursor::KeepAnchor);
            hilite1.format.setBackground(
                m_highlighter->colorForRole(ColorScheme::Matched));

            QTextEdit::ExtraSelection hilite2;
            hilite2.cursor = textCursor();
            hilite2.cursor.setPosition(closeParPos);
            hilite2.cursor.setPosition(closeParPos + 1,
                                       QTextCursor::KeepAnchor);
            hilite2.format.setBackground(
                m_highlighter->colorForRole(ColorScheme::Matched));

            QList<QTextEdit::ExtraSelection> extras;
            extras << hilite1;
            extras << hilite2;
            setExtraSelections(extras);
        }
    }
}

void Editor::doMatchingRight()
{
    // Tokenize the expression.
    const int currentPosition = textCursor().position();

    // Check for left par.
    auto subtext = text().right(text().length() - currentPosition);
    auto tokens = m_evaluator->scan(subtext);
    if (!tokens.valid() || tokens.count() < 1)
        return;
    auto firstToken = tokens.at(0);

    // Left par?
    if (firstToken.type() == Token::stxOpenPar
        && (firstToken.pos() + firstToken.size()) == 1)
    {
        // Find the matching right par.
        unsigned par = 1;
        int k = 0;
        Token matchToken;
        int matchPosition = -1;
        int openParPos = firstToken.pos() + firstToken.size() - 1;

        for (k = 1; k < tokens.count() && par > 0; ++k) {
            const Token matchToken = tokens.at(k);
            switch (matchToken.type()) {
            case Token::stxOpenPar:
                ++par;
                break;
            case Token::stxClosePar:
                --par;
                break;
            default:;
            }
            matchPosition = matchToken.pos() + matchToken.size() - 1;
        }

        if (par == 0) {
            QTextEdit::ExtraSelection hilite1;
            hilite1.cursor = textCursor();
            hilite1.cursor.setPosition(currentPosition+matchPosition);
            hilite1.cursor.setPosition(currentPosition+matchPosition + 1,
                                       QTextCursor::KeepAnchor);
            hilite1.format.setBackground(
                m_highlighter->colorForRole(ColorScheme::Matched));

            QTextEdit::ExtraSelection hilite2;
            hilite2.cursor = textCursor();
            hilite2.cursor.setPosition(currentPosition+openParPos);
            hilite2.cursor.setPosition(currentPosition+openParPos + 1,
                                       QTextCursor::KeepAnchor);
            hilite2.format.setBackground(
                m_highlighter->colorForRole(ColorScheme::Matched));

            QList<QTextEdit::ExtraSelection> extras;
            extras << hilite1;
            extras << hilite2;
            setExtraSelections(extras);
        }
    }
}


// Matches a list of built-in functions and variables to a fragment of the name.
QStringList Editor::matchFragment(const QString& id) const
{
    // Find matches in function names.
    const auto fnames = FunctionRepo::instance()->getIdentifiers();
    QStringList choices;
    for (int i = 0; i < fnames.count(); ++i) {
        if (fnames.at(i).startsWith(id, Qt::CaseInsensitive)) {
            QString str = fnames.at(i);
            Function* f = FunctionRepo::instance()->find(str);
            if (f)
                str.append(':').append(f->name());
            choices.append(str);
        }
    }
    choices.sort();

    // Find matches in variables names.
    QStringList vchoices;
    QList<Variable> variables = m_evaluator->getVariables();
    for (int i = 0; i < variables.count(); ++i) {
        if (variables.at(i).identifier().startsWith(id, Qt::CaseInsensitive)) {
            vchoices.append(QString("%1:%2").arg(
                variables.at(i).identifier(),
                NumberFormatter::format(variables.at(i).value())));
        }
    }
    vchoices.sort();
    choices += vchoices;

    // Find matches in user functions.
    QStringList ufchoices;
    auto userFunctions = m_evaluator->getUserFunctions();
    for (int i = 0; i < userFunctions.count(); ++i) {
        if (userFunctions.at(i).name().startsWith(id, Qt::CaseInsensitive)) {
            ufchoices.append(QString("%1:" + tr("User function")).arg(
                userFunctions.at(i).name()));
        }
    }
    ufchoices.sort();
    choices += ufchoices;

    return choices;
}

QString Editor::getKeyword() const
{
    // Tokenize the expression.
    const int currentPosition = textCursor().position();
    const Tokens tokens = m_evaluator->scan(text());

    // Find the token at the cursor.
    for (int i = tokens.size() - 1; i >= 0; --i) {
        const auto& token = tokens[i];
        if (token.pos() > currentPosition)
            continue;
        if (token.isIdentifier()) {
            auto matches = matchFragment(token.text());
            if (!matches.empty())
                return matches.first().split(":").first();
        }

        // Try further to the left.
        continue;
    }
    return "";
}

void Editor::triggerAutoComplete()
{
    if (m_shouldBlockAutoCompletionOnce) {
        m_shouldBlockAutoCompletionOnce = false;
        return;
    }
    if (!m_isAutoCompletionEnabled)
        return;

    // Tokenize the expression (this is very fast).
    const int currentPosition = textCursor().position();
    auto subtext = text().left(currentPosition);
    const auto tokens = m_evaluator->scan(subtext);
    if (!tokens.valid() || tokens.count() < 1)
        return;

    auto lastToken = tokens.at(tokens.count()-1);

    // Last token must be an identifier.
    if (!lastToken.isIdentifier())
        return;
    if (!lastToken.size())  // Invisible unit token
        return;
    const auto id = lastToken.text();
    if (id.length() < 1)
        return;

    // No space after identifier.
    if (lastToken.pos() + lastToken.size() < subtext.length())
        return;

    QStringList choices(matchFragment(id));

    // If we are assigning a user function, find matches in its arguments names
    // and replace variables names that collide.
    if (Evaluator::instance()->isUserFunctionAssign()) {
        for (int i=2; i<tokens.size(); ++i) {
            if (tokens[i].asOperator() == Token::ListSeparator)
                continue;
            if (tokens[i].asOperator() == Token::AssociationEnd)
                break;
            if (tokens[i].isIdentifier()) {
                auto arg = tokens[i].text();
                if (!arg.startsWith(id, Qt::CaseInsensitive))
                    continue;
                for (int j = 0; j < choices.size(); ++j) {
                    if (choices[j].split(":")[0] == arg) {
                        choices.removeAt(j);
                        j--;
                    }
                }
                choices.append(arg + ": " + tr("Argument"));
            }
        }
    }

    // No match, don't bother with completion.
    if (!choices.count())
        return;

    // Single perfect match, no need to give choices.
    if (choices.count() == 1)
        if (choices.at(0).toLower() == id.toLower())
            return;

    // Present the user with completion choices.
    m_completion->showCompletion(choices);
}

void Editor::autoComplete(const QString& item)
{
    if (!m_isAutoCompletionEnabled || item.isEmpty())
        return;

    const int currentPosition = textCursor().position();
    const auto subtext = text().left(currentPosition);
    const auto tokens = m_evaluator->scan(subtext);
    if (!tokens.valid() || tokens.count() < 1)
        return;

    const auto lastToken = tokens.at(tokens.count() - 1);
    if (!lastToken.isIdentifier())
        return;

    const auto str = item.split(':');

    // Add leading space characters if any.
    auto newTokenText = str.at(0);
    const int leadingSpaces = lastToken.size() - lastToken.text().length();
    if (leadingSpaces > 0)
        newTokenText = newTokenText.rightJustified(
            leadingSpaces + newTokenText.length(), ' ');

    blockSignals(true);
    QTextCursor cursor = textCursor();
    cursor.setPosition(lastToken.pos());
    cursor.setPosition(lastToken.pos() + lastToken.size(),
                       QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    insert(newTokenText);
    blockSignals(false);

    cursor = textCursor();
    bool hasParensAlready = cursor.movePosition(QTextCursor::NextCharacter,
                                                QTextCursor::KeepAnchor);
    if (hasParensAlready) {
        auto nextChar = cursor.selectedText();
        hasParensAlready = (nextChar == "(");
    }
    bool isFunction = FunctionRepo::instance()->find(str.at(0))
                      || m_evaluator->hasUserFunction(str.at(0));
    bool shouldAutoInsertParens = isFunction && !hasParensAlready;
    if (shouldAutoInsertParens) {
        insert(QString::fromLatin1("()"));
        cursor = textCursor();
        cursor.movePosition(QTextCursor::PreviousCharacter);
        setTextCursor(cursor);
    }

    checkAutoCalc();
}

void Editor::insertFromMimeData(const QMimeData* source)
{
    QStringList expressions =
        source->text().split("\n", Qt::SkipEmptyParts, Qt::CaseSensitive);
    if (expressions.size() == 1) {
        // Insert text manually to make sure expression does not contain new line characters
        insert(expressions.at(0));
        return;
    }
    for (int i = 0; i < expressions.size(); ++i) {
        insert(expressions.at(i));
        evaluate();
    }
}

void Editor::autoCalc()
{
    if (!m_isAutoCalcEnabled)
        return;

    const auto str = m_evaluator->autoFix(text());
    if (str.isEmpty())
        return;

    // Same reason as above, do not update "ans".
    m_evaluator->setExpression(str);
    auto quantity = m_evaluator->evalNoAssign();

    if (m_evaluator->error().isEmpty()) {
        if (quantity.isNan() && m_evaluator->isUserFunctionAssign()) {
            // Result is not always available when assigning a user function.
            emit autoCalcDisabled();
        } else {
            auto formatted = NumberFormatter::format(quantity);
            auto message = tr("Current result: <b>%1</b>").arg(formatted);
            emit autoCalcMessageAvailable(message);
            emit autoCalcQuantityAvailable(quantity);
        }
    } else
        emit autoCalcMessageAvailable(m_evaluator->error());
}

void Editor::increaseFontPointSize()
{
    QFont newFont = font();
    const int newSize = newFont.pointSize() + 1;
    if (newSize > 96)
        return;
    newFont.setPointSize(newSize);
    setFont(newFont);
}

void Editor::decreaseFontPointSize()
{
    QFont newFont = font();
    const int newSize = newFont.pointSize() - 1;
    if (newSize < 8)
        return;
    newFont.setPointSize(newSize);
    setFont(newFont);
}

void Editor::autoCalcSelection(const QString& custom)
{
    if (!m_isAutoCalcEnabled)
        return;

    auto str = custom.isNull() ?
        m_evaluator->autoFix(textCursor().selectedText())
        : custom;
    if (str.isEmpty())
        return;

    // Same reason as above, do not update "ans".
    m_evaluator->setExpression(str);
    auto quantity = m_evaluator->evalNoAssign();

    if (m_evaluator->error().isEmpty()) {
        if (quantity.isNan() && m_evaluator->isUserFunctionAssign()) {
            // Result is not always available when assigning a user function.
            auto message = tr("Selection result: n/a");
            emit autoCalcMessageAvailable(message);
        } else {
            auto formatted = NumberFormatter::format(quantity);
            auto message = tr("Selection result: <b>%1</b>").arg(formatted);
            emit autoCalcMessageAvailable(message);
            emit autoCalcQuantityAvailable(quantity);
        }
    } else
        emit autoCalcMessageAvailable(m_evaluator->error());
}

void Editor::insertConstant(const QString& constant)
{
    auto formattedConstant = constant;
    if (radixChar() == ',')
        formattedConstant.replace('.', ',');
    if (!constant.isNull())
        insert(formattedConstant);
    if (m_constantCompletion) {
        disconnect(m_constantCompletion);
        m_constantCompletion->deleteLater();
        m_constantCompletion = 0;
    }
}

void Editor::cancelConstantCompletion()
{
    if (m_constantCompletion) {
        disconnect(m_constantCompletion);
        m_constantCompletion->deleteLater();
        m_constantCompletion = 0;
    }
}

void Editor::evaluate()
{
    triggerEnter();
}

void Editor::paintEvent(QPaintEvent* event)
{
    QPlainTextEdit::paintEvent(event);

    if (!m_shouldPaintCustomCursor) {
        m_shouldPaintCustomCursor = true;
        return;
    }
    m_shouldPaintCustomCursor = false;

    QRect cursor = cursorRect();
    cursor.setLeft(cursor.left() - 1);
    cursor.setRight(cursor.right() + 1);

    QPainter painter(viewport());
    painter.fillRect(cursor, m_highlighter->colorForRole(ColorScheme::Cursor));
}

void Editor::historyBack()
{
    if (!m_history.count())
        return;
    if (!m_currentHistoryIndex)
        return;

    m_shouldBlockAutoCompletionOnce = true;
    if (m_currentHistoryIndex == m_history.count())
        m_savedCurrentEditor = toPlainText();
    --m_currentHistoryIndex;
    setText(m_history.at(m_currentHistoryIndex).expr());
    moveCursorToEnd(this);
    ensureCursorVisible();
}

void Editor::historyForward()
{
    if (!m_history.count())
        return;
    if (m_currentHistoryIndex == m_history.count())
        return;

    m_shouldBlockAutoCompletionOnce = true;
    m_currentHistoryIndex++;
    if (m_currentHistoryIndex == m_history.count())
        setText(m_savedCurrentEditor);
    else
        setText(m_history.at(m_currentHistoryIndex).expr());
    moveCursorToEnd(this);
    ensureCursorVisible();
}

void Editor::triggerEnter()
{
    m_completionTimer->stop();
    m_matchingTimer->stop();
    m_currentHistoryIndex = m_history.count();
    emit returnPressed();
}

void Editor::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange)
        setFixedHeight(sizeHint().height());
    QPlainTextEdit::changeEvent(event);
}

void Editor::focusOutEvent(QFocusEvent* event)
{
    m_shouldPaintCustomCursor = false;
    QPlainTextEdit::focusOutEvent(event);
}

void Editor::keyPressEvent(QKeyEvent* event)
{
    int key = event->key();

    switch (key) {
    case Qt::Key_Tab:
        // setTabChangesFocus() still allows entering a Tab character when
        // there's no other widgets to change focus to. To avoid that,
        // explicitly consume any Tabs that make it here.
        event->accept();
        return;

    case Qt::Key_Enter:
    case Qt::Key_Return:
        QTimer::singleShot(0, this, SLOT(triggerEnter()));
        event->accept();
        return;

    case Qt::Key_Up:
        if (event->modifiers() & Qt::ShiftModifier)
            emit shiftUpPressed();
        else
            historyBack();
        event->accept();
        return;

    case Qt::Key_Down:
        if (event->modifiers() & Qt::ShiftModifier)
            emit shiftDownPressed();
        else
            historyForward();
        event->accept();
        return;

    case Qt::Key_PageUp:
        if (event->modifiers() & Qt::ShiftModifier)
            emit shiftPageUpPressed();
        else if (event->modifiers() & Qt::ControlModifier)
            emit controlPageUpPressed();
        else
            emit pageUpPressed();
        event->accept();
        return;

    case Qt::Key_PageDown:
        if (event->modifiers() & Qt::ShiftModifier)
            emit shiftPageDownPressed();
        else if (event->modifiers() & Qt::ControlModifier)
            emit controlPageDownPressed();
        else
            emit pageDownPressed();
        event->accept();
        return;

    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
        checkMatching();
        checkAutoCalc();
        QPlainTextEdit::keyPressEvent(event);
        event->accept();
        return;

    case Qt::Key_Space:
        if (event->modifiers() == Qt::ControlModifier
            && !m_constantCompletion)
        {
            m_constantCompletion = new ConstantCompletion(this);
            connect(m_constantCompletion,
                    SIGNAL(selectedCompletion(const QString&)),
                    SLOT(insertConstant(const QString&)));
            connect(m_constantCompletion,
                    &ConstantCompletion::canceledCompletion,
                    this, &Editor::cancelConstantCompletion);
            m_constantCompletion->showCompletion();
            event->accept();
            return;
        }
        break;

    case Qt::Key_Period:
    case Qt::Key_Comma:
        if (event->modifiers() == Qt::KeypadModifier) {
            insert(QChar(this->radixChar()));
            event->accept();
            return;
        }
        break;

    case Qt::Key_Asterisk: {
        auto position = textCursor().position();
        if (position > 0 && QString("*×").contains(text().at(position - 1))) {
          // Replace ×* by ^ operator
          auto cursor = textCursor();
          cursor.removeSelectedText();  // just in case some text is selected
          cursor.deletePreviousChar();
          insert(QString::fromUtf8("^"));
        } else {
          insert(QString::fromUtf8("×")); // U+00D7 × MULTIPLICATION SIGN.
        }
        event->accept();
        return;
    }

    case Qt::Key_Minus:
        insert(QString::fromUtf8("−")); // U+2212 − MINUS SIGN.
        event->accept();
        return;
    case Qt::Key_At:
        insert(QString::fromUtf8("°")); // U+00B0 ° DEGREE SIGN
        event->accept();
        return;
    default:;
    }

    if (event->matches(QKeySequence::Copy)) {
        emit copySequencePressed();
        event->accept();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void Editor::scrollContentsBy(int dx, int dy)
{
    if (dy)
        return;
    QPlainTextEdit::scrollContentsBy(dx, dy);
    verticalScrollBar()->setMaximum(0);
    verticalScrollBar()->setMinimum(0);
}

void Editor::wheelEvent(QWheelEvent* event)
{
    if (event->angleDelta().y() > 0)
        historyBack();
    else if (event->angleDelta().y() < 0)
        historyForward();
    event->accept();
}

void Editor::rehighlight()
{
    m_highlighter->update();
    auto color = m_highlighter->colorForRole(ColorScheme::EditorBackground);
    auto colorName = color.name();
    setStyleSheet(QString("QPlainTextEdit { background: %1; }").arg(colorName));
}

void Editor::updateHistory()
{
    m_history = Evaluator::instance()->session()->historyToList();
    m_currentHistoryIndex = m_history.count();
}

void Editor::stopAutoCalc()
{
    emit autoCalcDisabled();
}

void Editor::stopAutoComplete()
{
    m_completionTimer->stop();
    m_completion->selectItem(QString());
    m_completion->doneCompletion();
    setFocus();
}

void Editor::wrapSelection()
{
    auto cursor = textCursor();
    if (cursor.hasSelection()) {
        const int selectionStart = cursor.selectionStart();
        const int selectionEnd = cursor.selectionEnd();
        cursor.setPosition(selectionStart);
        cursor.insertText("(");
        cursor.setPosition(selectionEnd + 1);
        cursor.insertText(")");
    } else {
        cursor.movePosition(QTextCursor::Start);
        cursor.insertText("(");
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(")");
    }
    setTextCursor(cursor);
}

EditorCompletion::EditorCompletion(Editor* editor)
    : QObject(editor)
{
    m_editor = editor;

    m_popup = new QTreeWidget();
    m_popup->setFrameShape(QFrame::NoFrame);
    m_popup->setColumnCount(2);
    m_popup->setRootIsDecorated(false);
    m_popup->header()->hide();
    m_popup->header()->setStretchLastSection(false);
    m_popup->setEditTriggers(QTreeWidget::NoEditTriggers);
    m_popup->setSelectionBehavior(QTreeWidget::SelectRows);
    m_popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_popup->setMouseTracking(true);
    m_popup->installEventFilter(this);

    connect(m_popup, SIGNAL(itemClicked(QTreeWidgetItem*, int)),
            SLOT(doneCompletion()));

    m_popup->hide();
    m_popup->setParent(0, Qt::Popup);
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setFocusProxy(editor);
    m_popup->setFrameStyle(QFrame::Box | QFrame::Plain);
}

EditorCompletion::~EditorCompletion()
{
    delete m_popup;
}

bool EditorCompletion::eventFilter(QObject* object, QEvent* event)
{
    if (object != m_popup)
        return false;

    if (event->type() == QEvent::KeyPress) {
        int key = static_cast<QKeyEvent*>(event)->key();

        switch (key) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            doneCompletion();
            return true;

        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            return false;

        default:
            m_popup->hide();
            m_editor->setFocus();
            if (key != Qt::Key_Escape)
                QApplication::sendEvent(m_editor, event);
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        m_popup->hide();
        m_editor->setFocus();
        return true;
    }

    return false;
}

void EditorCompletion::doneCompletion()
{
    m_popup->hide();
    m_editor->setFocus();
    QTreeWidgetItem* item = m_popup->currentItem();
    emit selectedCompletion(item ? item->text(0) : QString());
}

void EditorCompletion::showCompletion(const QStringList& choices)
{
    if (!choices.count())
        return;

    int maxIdentifierLength = 0;
    int maxDescriptionLength = 0;
    QFontMetrics metrics(m_editor->font());

    m_popup->setUpdatesEnabled(false);
    m_popup->clear();

    for (int i = 0; i < choices.count(); ++i) {
        QStringList pair = choices.at(i).split(':');
        QTreeWidgetItem* item = new QTreeWidgetItem(m_popup, pair);

        if (item && m_editor->layoutDirection() == Qt::RightToLeft)
            item->setTextAlignment(0, Qt::AlignRight);

        int length = metrics.boundingRect(pair.at(0)).width();
        if (length > maxIdentifierLength)
            maxIdentifierLength = length;

        length = metrics.boundingRect(pair.at(1)).width();
        if (length > maxDescriptionLength)
            maxDescriptionLength = length;
    }

    m_popup->sortItems(1, Qt::AscendingOrder);
    m_popup->sortItems(0, Qt::AscendingOrder);
    m_popup->setCurrentItem(m_popup->topLevelItem(0));

    // Size of the pop-up.
    m_popup->resizeColumnToContents(0);
    m_popup->setColumnWidth(0, m_popup->columnWidth(0) + 25);
    m_popup->resizeColumnToContents(1);
    m_popup->setColumnWidth(1, m_popup->columnWidth(1) + 25);

    const int maxVisibleItems = 8;
    const int height =
        m_popup->sizeHintForRow(0) * qMin(maxVisibleItems, choices.count()) + 3;
    const int width = m_popup->columnWidth(0) + m_popup->columnWidth(1) + 1;

    // Position, reference is editor's cursor position in global coord.
    auto cursor = m_editor->textCursor();
    cursor.movePosition(QTextCursor::StartOfWord);
    const int pixelsOffset = metrics.horizontalAdvance(m_editor->text(), cursor.position());
    auto point = QPoint(pixelsOffset, m_editor->height());
    QPoint position = m_editor->mapToGlobal(point);

    // If popup is partially invisible, move to other position.
    auto screen = QApplication::desktop()->availableGeometry(m_editor);
    if (position.y() + height > screen.y() + screen.height())
        position.setY(position.y() - height - m_editor->height());
    if (position.x() + width > screen.x() + screen.width())
        position.setX(screen.x() + screen.width() - width);

    m_popup->setUpdatesEnabled(true);
    m_popup->setGeometry(QRect(position, QSize(width, height)));
    m_popup->show();
    m_popup->setFocus();
}

void EditorCompletion::selectItem(const QString& item)
{
    if (item.isNull()) {
        m_popup->setCurrentItem(0);
        return;
    }

    auto targets = m_popup->findItems(item, Qt::MatchExactly);
    if (targets.count() > 0)
        m_popup->setCurrentItem(targets.at(0));
}

ConstantCompletion::ConstantCompletion(Editor* editor)
    : QObject(editor)
{
    m_editor = editor;

    m_popup = new QFrame;
    m_popup->setParent(0, Qt::Popup);
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setFocusProxy(editor);
    m_popup->setFrameStyle(QFrame::Box | QFrame::Plain);

    m_categoryWidget = new QTreeWidget(m_popup);
    m_categoryWidget->setFrameShape(QFrame::NoFrame);
    m_categoryWidget->setColumnCount(1);
    m_categoryWidget->setRootIsDecorated(false);
    m_categoryWidget->header()->hide();
    m_categoryWidget->setEditTriggers(QTreeWidget::NoEditTriggers);
    m_categoryWidget->setSelectionBehavior(QTreeWidget::SelectRows);
    m_categoryWidget->setMouseTracking(true);
    m_categoryWidget->installEventFilter(this);
    m_categoryWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(m_categoryWidget, SIGNAL(itemClicked(QTreeWidgetItem*, int)),
                              SLOT(showConstants()));

    m_constantWidget = new QTreeWidget(m_popup);
    m_constantWidget->setFrameShape(QFrame::NoFrame);
    m_constantWidget->setColumnCount(2);
    m_constantWidget->setColumnHidden(1, true);
    m_constantWidget->setRootIsDecorated(false);
    m_constantWidget->header()->hide();
    m_constantWidget->setEditTriggers(QTreeWidget::NoEditTriggers);
    m_constantWidget->setSelectionBehavior(QTreeWidget::SelectRows);
    m_constantWidget->setMouseTracking(true);
    m_constantWidget->installEventFilter(this);
    m_constantWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(m_constantWidget, SIGNAL(itemClicked(QTreeWidgetItem*, int)),
                              SLOT(doneCompletion()));

    m_slider = new QTimeLine(100, m_popup);
    m_slider->setEasingCurve(QEasingCurve(QEasingCurve::Linear));
    connect(m_slider, SIGNAL(frameChanged(int)),
                      SLOT(setHorizontalPosition(int)));

    const Constants* constants = Constants::instance();
    m_constantList = constants->list();

    // Populate categories.
    QStringList categoryList;
    categoryList << tr("All");
    QTreeWidgetItem* all = new QTreeWidgetItem(m_categoryWidget, categoryList);
    for (int i = 0; i < constants->categories().count(); ++i) {
        categoryList.clear();
        categoryList << constants->categories().at(i);
        new QTreeWidgetItem(m_categoryWidget, categoryList);
    }
    m_categoryWidget->setCurrentItem(all);

    // Populate constants.
    m_lastCategory = tr("All");
    for (int i = 0; i < constants->list().count(); ++i) {
        QStringList names;
        names << constants->list().at(i).name;
        names << constants->list().at(i).name.toUpper();
        new QTreeWidgetItem(m_constantWidget, names);
    }
    m_constantWidget->sortItems(0, Qt::AscendingOrder);

    // Find size, the biggest between both.
    m_constantWidget->resizeColumnToContents(0);
    m_categoryWidget->resizeColumnToContents(0);
    int width = qMax(m_constantWidget->width(), m_categoryWidget->width());
    const int constantsHeight =
        m_constantWidget->sizeHintForRow(0)
            * qMin(7, m_constantList.count()) + 3;
    const int categoriesHeight =
        m_categoryWidget->sizeHintForRow(0)
            * qMin(7, constants->categories().count()) + 3;
    const int height = qMax(constantsHeight, categoriesHeight);
    width += 200; // Extra space (FIXME: scrollbar size?).

    // Adjust dimensions.
    m_popup->resize(width, height);
    m_constantWidget->resize(width, height);
    m_categoryWidget->resize(width, height);
}

ConstantCompletion::~ConstantCompletion()
{
    delete m_popup;
    m_editor->setFocus();
}

void ConstantCompletion::showCategory()
{
    m_slider->setFrameRange(m_popup->width(), 0);
    m_slider->stop();
    m_slider->start();
    m_categoryWidget->setFocus();
}

void ConstantCompletion::showConstants()
{
    m_slider->setFrameRange(0, m_popup->width());
    m_slider->stop();
    m_slider->start();
    m_constantWidget->setFocus();

    QString chosenCategory;
    if (m_categoryWidget->currentItem())
        chosenCategory = m_categoryWidget->currentItem()->text(0);

    if (m_lastCategory == chosenCategory)
        return;

    m_constantWidget->clear();

    for (int i = 0; i < m_constantList.count(); ++i) {
        QStringList names;
        names << m_constantList.at(i).name;
        names << m_constantList.at(i).name.toUpper();

        const bool include = (chosenCategory == tr("All")) ?
            true : (m_constantList.at(i).category == chosenCategory);

        if (!include)
            continue;

        new QTreeWidgetItem(m_constantWidget, names);
    }

    m_constantWidget->sortItems(0, Qt::AscendingOrder);
    m_constantWidget->setCurrentItem(m_constantWidget->itemAt(0, 0));
    m_lastCategory = chosenCategory;
}

bool ConstantCompletion::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::Hide) {
        emit canceledCompletion();
        return true;
    }

    if (object == m_constantWidget) {

        if (event->type() == QEvent::KeyPress) {
            int key = static_cast<QKeyEvent*>(event)->key();

            switch (key) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Tab:
                doneCompletion();
                return true;

            case Qt::Key_Left:
                showCategory();
                return true;

            case Qt::Key_Right:
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Home:
            case Qt::Key_End:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
                return false;
            }

            if (key != Qt::Key_Escape)
                QApplication::sendEvent(m_editor, event);
            emit canceledCompletion();
            return true;
        }
    }

    if (object == m_categoryWidget) {

        if (event->type() == QEvent::KeyPress) {
            int key = static_cast<QKeyEvent*>(event)->key();

            switch (key) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Right:
                showConstants();
                return true;

            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Home:
            case Qt::Key_End:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
                return false;
            }

            if (key != Qt::Key_Escape)
                QApplication::sendEvent(m_editor, event);
            emit canceledCompletion();
            return true;
        }
    }

    return false;
}

void ConstantCompletion::doneCompletion()
{
    m_editor->setFocus();
    const auto* item = m_constantWidget->currentItem();
    auto found = std::find_if(m_constantList.begin(), m_constantList.end(),
        [&](const Constant& c) { return item->text(0) == c.name; }
    );
    emit selectedCompletion(
        (item && found != m_constantList.end()) ?
            found->value : QString()
    );
}

void ConstantCompletion::showCompletion()
{
    // Position, reference is editor's cursor position in global coord.
    QFontMetrics metrics(m_editor->font());
    const int currentPosition = m_editor->textCursor().position();
    const int pixelsOffset = metrics.horizontalAdvance(m_editor->text(), currentPosition);
    auto pos = m_editor->mapToGlobal(QPoint(pixelsOffset, m_editor->height()));

    const int height = m_popup->height();
    const int width = m_popup->width();

    // If popup is partially invisible, move to other position.
    const QRect screen = QApplication::desktop()->availableGeometry(m_editor);
    if (pos.y() + height > screen.y() + screen.height())
        pos.setY(pos.y() - height - m_editor->height());
    if (pos.x() + width > screen.x() + screen.width())
        pos.setX(screen.x() + screen.width() - width);

    // Start with category.
    m_categoryWidget->setFocus();
    setHorizontalPosition(0);

    m_popup->move(pos);
    m_popup->show();
}

void ConstantCompletion::setHorizontalPosition(int x)
{
    m_categoryWidget->move(-x, 0);
    m_constantWidget->move(m_popup->width() - x, 0);
}
