/*
 * obs-multistream -- the dock.
 *
 * A plain QWidget that OBS hosts in its window. All state lives in
 * multistream-core; this file only renders it and forwards user actions.
 */

#include "multistream.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

constexpr int kColumnEnabled = 0;
constexpr int kColumnName = 1;
constexpr int kColumnServer = 2;
constexpr int kColumnEncoder = 3;
constexpr int kColumnStatus = 4;
constexpr int kColumnCount = 5;

static QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString encoderLabel(int mode)
{
	return mode == MS_ENC_INDEPENDENT ? tr_("Editor.EncoderIndependent")
					  : tr_("Editor.EncoderShared");
}

/* ------------------------------------------------------------- editor -- */

class EditorDialog : public QDialog {
public:
	EditorDialog(uint64_t id, QWidget *parent) : QDialog(parent), m_id(id)
	{
		setWindowTitle(id ? tr_("Editor.TitleEdit") : tr_("Editor.TitleAdd"));
		setModal(true);

		m_name = new QLineEdit(this);
		m_name->setPlaceholderText(tr_("Editor.NamePlaceholder"));

		m_server = new QLineEdit(this);
		m_server->setPlaceholderText(
			QStringLiteral("rtmp://live.example.com/app"));

		m_key = new QLineEdit(this);
		m_key->setEchoMode(QLineEdit::Password);

		auto *reveal = new QCheckBox(tr_("Editor.RevealKey"), this);
		connect(reveal, &QCheckBox::toggled, this, [this](bool on) {
			m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		});

		auto *keyRow = new QWidget(this);
		auto *keyLayout = new QHBoxLayout(keyRow);
		keyLayout->setContentsMargins(0, 0, 0, 0);
		keyLayout->addWidget(m_key, 1);
		keyLayout->addWidget(reveal, 0);

		m_encoder = new QComboBox(this);
		m_encoder->addItem(tr_("Editor.EncoderSharedHint"), MS_ENC_SHARED);
		m_encoder->addItem(tr_("Editor.EncoderIndependentHint"),
				   MS_ENC_INDEPENDENT);

		m_reconnect = new QSpinBox(this);
		m_reconnect->setRange(1, 30);
		m_reconnect->setSuffix(tr_("Editor.SecondsSuffix"));

		auto *form = new QFormLayout;
		form->addRow(tr_("Editor.Name"), m_name);
		form->addRow(tr_("Editor.Server"), m_server);
		form->addRow(tr_("Editor.Key"), keyRow);
		form->addRow(tr_("Editor.Encoder"), m_encoder);
		form->addRow(tr_("Editor.Reconnect"), m_reconnect);

		auto *buttons = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this,
			&EditorDialog::onAccept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

		auto *layout = new QVBoxLayout(this);
		layout->addLayout(form);
		layout->addWidget(buttons);

		load();
	}

private:
	void load()
	{
		char buf[MS_STR_LEN];

		if (!m_id) {
			m_encoder->setCurrentIndex(
				m_encoder->findData(MS_ENC_SHARED));
			m_reconnect->setValue(2);
			return;
		}

		if (ms_target_get_name(m_id, buf, sizeof(buf)))
			m_name->setText(QString::fromUtf8(buf));
		if (ms_target_get_url(m_id, buf, sizeof(buf)))
			m_server->setText(QString::fromUtf8(buf));
		if (ms_target_get_key(m_id, buf, sizeof(buf)))
			m_key->setText(QString::fromUtf8(buf));

		int index = m_encoder->findData(ms_target_get_mode(m_id));
		if (index >= 0)
			m_encoder->setCurrentIndex(index);
		m_reconnect->setValue(ms_target_get_reconnect(m_id));
	}

	void onAccept()
	{
		const QString server = m_server->text().trimmed();
		const QString key = m_key->text().trimmed();
		QString name = m_name->text().trimmed();

		if (server.isEmpty() || key.isEmpty()) {
			QMessageBox::warning(this, tr_("Editor.TitleAdd"),
					     tr_("Editor.NeedServerAndKey"));
			return;
		}

		if (!server.startsWith(QStringLiteral("rtmp://")) &&
		    !server.startsWith(QStringLiteral("rtmps://"))) {
			QMessageBox::warning(this, tr_("Editor.TitleAdd"),
					     tr_("Editor.NeedRtmpUrl"));
			return;
		}

		if (name.isEmpty())
			name = tr_("Default.Name");

		const QByteArray serverUtf8 = server.toUtf8();
		const QByteArray keyUtf8 = key.toUtf8();
		const QByteArray nameUtf8 = name.toUtf8();
		const int mode = m_encoder->currentData().toInt();

		if (!m_id) {
			m_id = ms_target_add(nameUtf8.constData(),
					     serverUtf8.constData(),
					     keyUtf8.constData());
			if (!m_id) {
				QMessageBox::warning(this, tr_("Editor.TitleAdd"),
						     tr_("Editor.TableFull"));
				return;
			}
		} else {
			ms_target_set_name(m_id, nameUtf8.constData());
			ms_target_set_url(m_id, serverUtf8.constData());
			ms_target_set_key(m_id, keyUtf8.constData());
		}

		ms_target_set_mode(m_id, mode);
		ms_target_set_reconnect(m_id, m_reconnect->value());

		accept();
	}

	uint64_t m_id;
	QLineEdit *m_name = nullptr;
	QLineEdit *m_server = nullptr;
	QLineEdit *m_key = nullptr;
	QComboBox *m_encoder = nullptr;
	QSpinBox *m_reconnect = nullptr;
};

/* --------------------------------------------------------------- dock -- */

class MultiStreamDock : public QWidget {
	Q_OBJECT

public:
	explicit MultiStreamDock(QWidget *parent) : QWidget(parent)
	{
		m_table = new QTableWidget(this);
		m_table->setColumnCount(kColumnCount);
		m_table->setHorizontalHeaderLabels({tr_("Column.Enabled"),
						    tr_("Column.Name"),
						    tr_("Column.Server"),
						    tr_("Column.Encoder"),
						    tr_("Column.Status")});
		m_table->verticalHeader()->setVisible(false);
		m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_table->setSelectionMode(QAbstractItemView::SingleSelection);
		m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_table->horizontalHeader()->setSectionResizeMode(
			kColumnServer, QHeaderView::Stretch);
		m_table->setColumnWidth(kColumnEnabled, 48);

		auto *addButton = new QPushButton(tr_("Button.Add"), this);
		auto *editButton = new QPushButton(tr_("Button.Edit"), this);
		auto *removeButton = new QPushButton(tr_("Button.Remove"), this);

		connect(addButton, &QPushButton::clicked, this,
			[this] { openEditor(0); });
		connect(editButton, &QPushButton::clicked, this, [this] {
			const uint64_t id = selectedId();
			if (!id) {
				QMessageBox::information(this, tr_("Dock.Title"),
							 tr_("Hint.SelectRow"));
				return;
			}
			openEditor(id);
		});
		connect(removeButton, &QPushButton::clicked, this,
			[this] { removeSelected(); });
		connect(m_table, &QTableWidget::doubleClicked, this,
			[this](const QModelIndex &index) {
				const uint64_t id = idAtRow(index.row());
				if (id)
					openEditor(id);
			});

		auto *buttons = new QHBoxLayout;
		buttons->addWidget(addButton);
		buttons->addWidget(editButton);
		buttons->addWidget(removeButton);
		buttons->addStretch(1);

		m_hint = new QLabel(this);
		m_hint->setWordWrap(true);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->addWidget(m_table, 1);
		layout->addLayout(buttons);
		layout->addWidget(m_hint);

		connect(&m_timer, &QTimer::timeout, this,
			&MultiStreamDock::refresh);
		m_timer.start(500);

		rebuild();
	}

private:
	uint64_t idAtRow(int row) const
	{
		if (row < 0 || row >= m_ids.size())
			return 0;
		return m_ids.at(row);
	}

	uint64_t selectedId() const
	{
		const auto rows = m_table->selectionModel()->selectedRows();
		if (rows.isEmpty())
			return 0;
		return idAtRow(rows.first().row());
	}

	void rebuild()
	{
		m_table->setRowCount(0);
		m_ids.clear();

		const size_t count = ms_target_count();
		for (size_t i = 0; i < count; i++) {
			const uint64_t id = ms_target_id_at(i);
			if (!id)
				continue;

			const int row = m_table->rowCount();
			m_table->insertRow(row);
			m_ids.append(id);

			auto *enabled = new QCheckBox(m_table);
			enabled->setChecked(ms_target_get_enabled(id));
			connect(enabled, &QCheckBox::toggled, this,
				[this, id](bool on) {
					ms_target_set_enabled(id, on);
					if (on)
						ms_target_start_now(id);
					else
						ms_target_stop_now(id);
					refresh();
				});
			m_table->setCellWidget(row, kColumnEnabled, enabled);

			char buf[MS_STR_LEN] = {0};

			ms_target_get_name(id, buf, sizeof(buf));
			m_table->setItem(row, kColumnName,
					 new QTableWidgetItem(QString::fromUtf8(buf)));

			ms_target_get_url(id, buf, sizeof(buf));
			m_table->setItem(row, kColumnServer,
					 new QTableWidgetItem(QString::fromUtf8(buf)));

			m_table->setItem(row, kColumnEncoder,
					 new QTableWidgetItem(
						 encoderLabel(ms_target_get_mode(id))));

			ms_target_get_status(id, buf, sizeof(buf));
			m_table->setItem(row, kColumnStatus,
					 new QTableWidgetItem(QString::fromUtf8(buf)));
		}

		m_generation = ms_generation();
	}

	void refresh()
	{
		if (ms_generation() != m_generation) {
			rebuild();
		} else {
			for (int row = 0; row < m_ids.size(); row++) {
				const uint64_t id = m_ids.at(row);
				const bool running = ms_target_is_running(id);

				char buf[MS_STATUS_LEN] = {0};
				ms_target_get_status(id, buf, sizeof(buf));

				auto *status =
					m_table->item(row, kColumnStatus);
				if (status) {
					status->setText(QString::fromUtf8(buf));
					status->setForeground(
						running ? QColor(0x3c, 0xa5, 0x4a)
							: palette().color(
								  QPalette::Text));
				}

				auto *enabled = qobject_cast<QCheckBox *>(
					m_table->cellWidget(row, kColumnEnabled));
				if (enabled) {
					QSignalBlocker blocker(enabled);
					enabled->setChecked(
						ms_target_get_enabled(id));
				}
			}
		}

		m_hint->setText(obs_frontend_streaming_active()
					? tr_("Hint.Streaming")
					: tr_("Hint.Idle"));
	}

	void openEditor(uint64_t id)
	{
		EditorDialog dialog(id, this);
		if (dialog.exec() == QDialog::Accepted) {
			rebuild();
			if (id)
				ms_target_start_now(id);
		}
	}

	void removeSelected()
	{
		const uint64_t id = selectedId();
		if (!id) {
			QMessageBox::information(this, tr_("Dock.Title"),
						 tr_("Hint.SelectRow"));
			return;
		}

		const auto answer = QMessageBox::question(
			this, tr_("Dock.Title"), tr_("Hint.ConfirmRemove"),
			QMessageBox::Yes | QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;

		ms_target_remove(id);
		rebuild();
	}

	QTableWidget *m_table = nullptr;
	QLabel *m_hint = nullptr;
	QTimer m_timer;
	QVector<uint64_t> m_ids;
	uint64_t m_generation = 0;
};

static MultiStreamDock *g_dock = nullptr;

bool ms_dock_register(void)
{
	if (g_dock)
		return true;

	g_dock = new MultiStreamDock(nullptr);
	if (!obs_frontend_add_dock_by_id("obs-multistream-dock",
					 obs_module_text("Dock.Title"), g_dock)) {
		delete g_dock;
		g_dock = nullptr;
		return false;
	}

	return true;
}

void ms_dock_unregister(void)
{
	/* OBS owns the dock widget and destroys it together with the main
	 * window, so there is nothing to release here. Removing it from inside
	 * obs_module_unload() would touch an already torn-down UI. */
}

#include "multistream-dock.moc"
