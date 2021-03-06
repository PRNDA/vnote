#include "lineeditdelegate.h"

#include <QLineEdit>


LineEditDelegate::LineEditDelegate(QObject *p_parent)
    : QStyledItemDelegate(p_parent)
{
}

QWidget *LineEditDelegate::createEditor(QWidget *p_parent,
                                        const QStyleOptionViewItem &p_option,
                                        const QModelIndex &p_index) const
{
    Q_UNUSED(p_option);
    Q_UNUSED(p_index);

    QLineEdit *edit = new QLineEdit(p_parent);
    return edit;
}

void LineEditDelegate::setEditorData(QWidget *p_editor, const QModelIndex &p_index) const
{
    QString text = p_index.model()->data(p_index, Qt::EditRole).toString();

    QLineEdit *edit = static_cast<QLineEdit *>(p_editor);
    edit->setText(text);
}

void LineEditDelegate::setModelData(QWidget *p_editor,
                                    QAbstractItemModel *p_model,
                                    const QModelIndex &p_index) const
{
    QLineEdit *edit = static_cast<QLineEdit *>(p_editor);

    p_model->setData(p_index, edit->text(), Qt::EditRole);
}

void LineEditDelegate::updateEditorGeometry(QWidget *p_editor,
                                            const QStyleOptionViewItem &p_option,
                                            const QModelIndex &p_index) const
{
    Q_UNUSED(p_index);
    p_editor->setGeometry(p_option.rect);
}
