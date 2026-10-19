#include "Receiver.hpp"
#include "Discovery.hpp"
#include "NativeScreen.hpp"
#include "DisplayLayout.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace {
int logicalSize(int pixels, double scale) {
    return td::logicalDisplaySize(pixels, scale);
}

class ScaleBox final : public QDoubleSpinBox {
    QString textFromValue(double value) const override {
        QString text = locale().toString(value, 'f', 4);
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith(locale().decimalPoint()))
            text.chop(1);
        return text;
    }
};

QFrame *card(QVBoxLayout *layout, const QString &title) {
    auto *frame = new QFrame;
    frame->setObjectName("card");
    auto *body = new QVBoxLayout(frame);
    body->setContentsMargins(20, 18, 20, 18);
    body->setSpacing(14);
    auto *heading = new QLabel(title);
    heading->setObjectName("cardTitle");
    body->addWidget(heading);
    layout->addWidget(frame);
    return frame;
}

QFormLayout *form(QFrame *frame) {
    auto *rows = new QFormLayout;
    rows->setHorizontalSpacing(24);
    rows->setVerticalSpacing(14);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    static_cast<QVBoxLayout *>(frame->layout())->addLayout(rows);
    return rows;
}

class ClientWindow final : public QWidget {
    QSettings settings;
    QProcess receiver;
    Discovery discovery;
    QFile log{QStringLiteral("/tmp/thunder-display-receiver.log")};
    QLineEdit *host;
    QComboBox *presets, *fps, *link, *codec, *audioBuffer;
    QString bitrateCodec;
    QSpinBox *width, *height, *mbps;
    ScaleBox *scale;
    QCheckBox *fullscreen, *exclusive, *gameMouse, *audioEnabled, *muteMacAudio;
    QKeySequenceEdit *releaseShortcut, *fullscreenShortcut;
    QListWidget *devices;
    QLabel *preview, *status, *stats, *deviceEmpty;
    QPushButton *apply, *stop, *refresh;
    QFormLayout *displayForm;
    QWidget *dimensions;
    QByteArray pendingOutput;
    bool restartPending = false, closing = false, stopping = false;
    QStringList nextArguments;
    unsigned stopGeneration = 0, scanGeneration = 0;

    void updatePreview() {
        const double factor = scale->value();
        const int backing = factor == 1.0 ? 1 : 2;
        const int lw = logicalSize(width->value(), factor),
                  lh = logicalSize(height->value(), factor);
        preview->setText(QStringLiteral("桌面 %1 × %2   ·   渲染 %3 × %4")
                             .arg(lw)
                             .arg(lh)
                             .arg(lw * backing)
                             .arg(lh * backing));
    }

    void syncPreset() {
        const int i = presets->findData(QSize(width->value(), height->value()));
        presets->setCurrentIndex(std::max(0, i));
        displayForm->setRowVisible(dimensions, i < 0);
        updatePreview();
    }

    void save() {
        settings.setValue("host", host->text().trimmed());
        settings.setValue("width", width->value());
        settings.setValue("height", height->value());
        settings.setValue("scale", scale->value());
        settings.setValue("fps", fps->currentData());
        settings.setValue("codec", codec->currentData());
        settings.setValue(bitrateCodec + "Mbps", mbps->value());
        settings.setValue("fullscreen", fullscreen->isChecked());
        settings.setValue("mbps", mbps->value());
        settings.setValue("linkMbps", link->currentData());
        settings.setValue("exclusiveKeyboard", exclusive->isChecked());
        settings.setValue("gameMouse", gameMouse->isChecked());
        settings.setValue("audioEnabled", audioEnabled->isChecked());
        settings.setValue("audioBufferMs", audioBuffer->currentData());
        settings.setValue("muteMacAudio", muteMacAudio->isChecked());
        settings.setValue("releaseShortcut",
                          releaseShortcut->keySequence().toString(QKeySequence::PortableText));
        settings.setValue("fullscreenShortcut",
                          fullscreenShortcut->keySequence().toString(QKeySequence::PortableText));
        settings.sync();
    }

    void startReceiver() {
        stopping = false;
        pendingOutput.clear();
        stats->clear();
        if (log.isOpen())
            log.close();
        if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qWarning("Cannot open receiver log: %s", qPrintable(log.errorString()));
        status->setText(QStringLiteral("正在连接…"));
        apply->setEnabled(false);
        stop->setEnabled(true);
        receiver.start(QCoreApplication::applicationDirPath() + "/thunder-display-receiver",
                       nextArguments);
    }

    void updateGameMouse() {
        settings.setValue("gameMouse", gameMouse->isChecked());
        settings.sync();
        if (receiver.state() == QProcess::Running)
            receiver.write(gameMouse->isChecked() ? "GAME_MOUSE 1\n" : "GAME_MOUSE 0\n");
    }

    void stopReceiver() {
        stopping = true;
        apply->setEnabled(false);
        stop->setEnabled(false);
        status->setText(restartPending ? QStringLiteral("正在应用…") : QStringLiteral("正在断开…"));
        receiver.terminate();
        const unsigned generation = ++stopGeneration;
        QTimer::singleShot(2000, this, [this, generation] {
            if (stopGeneration == generation && receiver.state() != QProcess::NotRunning)
                receiver.kill();
        });
    }

    void connectReceiver() {
        width->interpretText();
        height->interpretText();
        scale->interpretText();
        mbps->interpretText();
        const int w = width->value(), h = height->value();
        if (host->text().trimmed().isEmpty()) {
            status->setText(QStringLiteral("选择 Mac 或输入地址"));
            host->setFocus();
            return;
        }
        if ((w & 1) || (h & 1) || int64_t(w) * h > 33554432) {
            status->setText(QStringLiteral("分辨率需要偶数宽高，总像素不能超过 33554432"));
            return;
        }
        const QString shortcut =
            releaseShortcut->keySequence().toString(QKeySequence::PortableText);
        ReleaseShortcut parsed;
        if (!parseReleaseShortcut(shortcut.toStdString(), parsed)) {
            status->setText(QStringLiteral("释放控制快捷键无效"));
            return;
        }
        const QString fullKey =
            fullscreenShortcut->keySequence().toString(QKeySequence::PortableText);
        ReleaseShortcut parsedFull;
        if (!parseReleaseShortcut(fullKey.toStdString(), parsedFull)) {
            status->setText(QStringLiteral("全屏快捷键无效"));
            return;
        }
        if (parsed.key == parsedFull.key && parsed.modifiers == parsedFull.modifiers) {
            status->setText(QStringLiteral("两个快捷键不能相同"));
            return;
        }
        if (!QFileInfo(QCoreApplication::applicationDirPath() + "/thunder-display-receiver")
                 .isExecutable()) {
            status->setText(QStringLiteral("接收组件缺失，请重新安装"));
            return;
        }
        if (codec->currentData() == "h264" && (w > 4096 || h > 4096 || int64_t(w) * h > 9437184)) {
            status->setText(QStringLiteral("H.264 每边最多 4096，总像素最多 9 MP"));
            return;
        }
        save();
        nextArguments = {"--host",
                         host->text().trimmed(),
                         "--width",
                         QString::number(w),
                         "--height",
                         QString::number(h),
                         scale->value() == 1.0 ? "--no-retina" : "--retina",
                         "--scale",
                         QString::number(scale->value(), 'f', 4),
                         "--fps",
                         fps->currentData().toString(),
                         "--mbps",
                         QString::number(mbps->value()),
                         "--link-mbps",
                         link->currentData().toString(),
                         "--codec",
                         codec->currentData().toString(),
                         "--444",
                         "--settings-stdin",
                         "--release-shortcut",
                         shortcut,
                         "--fullscreen-shortcut",
                         fullKey};
        if (fullscreen->isChecked())
            nextArguments << "--fullscreen";
        if (!exclusive->isChecked())
            nextArguments << "--local-shortcuts";
        if (gameMouse->isChecked())
            nextArguments << "--game-mouse";
        if (!audioEnabled->isChecked())
            nextArguments << "--no-audio";
        else {
            nextArguments << "--audio-buffer-ms" << audioBuffer->currentData().toString();
            if (muteMacAudio->isChecked())
                nextArguments << "--mute-mac-audio";
        }
        if (receiver.state() != QProcess::NotRunning) {
            restartPending = true;
            stopReceiver();
        } else
            startReceiver();
    }

    void readOutput() {
        const QByteArray data = receiver.readAllStandardOutput();
        if (log.isOpen()) {
            log.write(data);
            log.flush();
        }
        pendingOutput += data;
        while (true) {
            const auto newline = pendingOutput.indexOf('\n');
            if (newline < 0)
                break;
            const QString line = QString::fromUtf8(pendingOutput.left(newline));
            pendingOutput.remove(0, newline + 1);
            if (stopping)
                continue;
            if (line.startsWith("Capture state: ")) {
                const QString state = line.mid(15);
                status->setText(state == "READY"    ? QStringLiteral("已连接")
                                : state == "WAKE"   ? QStringLiteral("正在唤醒 Mac…")
                                : state == "LOCKED" ? QStringLiteral("等待 Mac 解锁…")
                                                    : QStringLiteral("等待 Mac 显示会话…"));
                if (state != "READY")
                    stats->clear();
            } else if (line.startsWith("Connected: ") || line == "Connection restored")
                status->setText(QStringLiteral("已连接"));
            else if (line == "Connection lost") {
                status->setText(QStringLiteral("连接中断，正在重连…"));
                stats->clear();
            } else if (line.startsWith("ThunderDisplay · "))
                stats->setText(line.section(" · ", 1, 2));
            else if (line == "Audio warning: Mac local mute unavailable")
                status->setText(QStringLiteral("已连接 · Mac 本机静音不可用"));
            else if (line.startsWith("Mac: ") || line.startsWith("ThunderDisplay: ")) {
                QString error = line.section(": ", 1);
                if (error == "Screen recording permission required")
                    error = QStringLiteral("Mac 尚未授权屏幕录制");
                else if (error == "Mac is in use")
                    error = QStringLiteral("Mac 正被另一客户端使用");
                status->setText(error);
            } else if (line.startsWith("Fullscreen: ")) {
                fullscreen->setChecked(line.endsWith("on"));
                settings.setValue("fullscreen", fullscreen->isChecked());
            } else if (line == "Disconnected by Mac")
                status->setText(QStringLiteral("Mac 已断开"));
        }
        if (pendingOutput.size() > 16384)
            pendingOutput.clear();
    }

    void scan() {
        const unsigned generation = ++scanGeneration;
        devices->clear();
        devices->hide();
        deviceEmpty->show();
        deviceEmpty->setText(QStringLiteral("正在搜索…"));
        refresh->setEnabled(false);
        auto probe = [this, generation] {
            if (generation == scanGeneration && !discovery.scan(host->text().trimmed()))
                deviceEmpty->setText(QStringLiteral("搜索不可用"));
        };
        probe();
        QTimer::singleShot(400, this, probe);
        QTimer::singleShot(900, this, probe);
        QTimer::singleShot(1600, this, [this, generation] {
            if (generation != scanGeneration)
                return;
            refresh->setEnabled(true);
            if (!devices->count())
                deviceEmpty->setText(QStringLiteral("未发现 Mac"));
        });
    }

  protected:
    void closeEvent(QCloseEvent *event) override {
        save();
        restartPending = false;
        if (receiver.state() != QProcess::NotRunning) {
            closing = true;
            stopReceiver();
            event->ignore();
        } else
            event->accept();
    }

  public:
    ClientWindow() {
        settings.remove("sharpness");
        settings.remove("sharpener");
        settings.sync();
        setWindowTitle(QStringLiteral("ThunderDisplay"));
        setMinimumSize(760, 600);
        resize(840, 680);
        QPalette colors = palette();
        const bool dark = colors.color(QPalette::Window).lightness() < 128;
        const QString border = dark ? "#39434f" : "#dce3eb", surface = dark ? "#252c35" : "#ffffff",
                      muted = dark ? "#a9b6c6" : "#64748b";
        setStyleSheet(
            QStringLiteral(
                "QWidget { font-size: 14px; } QFrame#card { background: %1; border: 1px solid %2; border-radius: 12px; }"
                "QLabel#brand { font-size: 22px; font-weight: 700; } QLabel#pageTitle { font-size: 22px; font-weight: 600; }"
                "QLabel#cardTitle { font-size: 15px; font-weight: 600; } QLabel#muted { color: %3; font-size: 12px; }"
                "QListWidget#navigation { border: 0; background: transparent; outline: 0; } QListWidget#navigation::item { padding: 12px 18px; margin: 3px 0; border-radius: 8px; }"
                "QListWidget#navigation::item:selected { background: #087f9d; color: white; }"
                "QListWidget#devices { border: 0; background: transparent; outline: 0; } QListWidget#devices::item { padding: 10px; border-radius: 6px; }"
                "QPushButton { padding: 8px 18px; border-radius: 7px; } QPushButton#primary { background: #087f9d; color: white; border: 0; font-weight: 600; }"
                "QPushButton#primary:disabled { background: %2; color: %3; } QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox,QKeySequenceEdit { min-height: 28px; }"
                "QScrollArea { border: 0; background: transparent; }")
                .arg(surface, border, muted));
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(24, 22, 24, 18);
        root->setSpacing(20);
        auto *header = new QHBoxLayout;
        auto *brand = new QLabel(QStringLiteral("ThunderDisplay"));
        brand->setObjectName("brand");
        header->addWidget(brand);
        header->addStretch();
        auto *subtitle = new QLabel(QStringLiteral("macOS 局域网副屏"));
        subtitle->setObjectName("muted");
        header->addWidget(subtitle);
        root->addLayout(header);
        auto *body = new QHBoxLayout;
        body->setSpacing(24);
        auto *nav = new QListWidget;
        nav->setObjectName("navigation");
        nav->setFixedWidth(136);
        nav->addItems({QStringLiteral("连接"), QStringLiteral("显示"), QStringLiteral("音频"),
                       QStringLiteral("键鼠")});
        body->addWidget(nav);
        auto *pages = new QStackedWidget;
        body->addWidget(pages, 1);
        root->addLayout(body, 1);
        auto page = [&](const QString &title) {
            auto *scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            auto *content = new QWidget;
            auto *layout = new QVBoxLayout(content);
            layout->setContentsMargins(0, 0, 6, 0);
            layout->setSpacing(16);
            auto *label = new QLabel(title);
            label->setObjectName("pageTitle");
            layout->addWidget(label);
            scroll->setWidget(content);
            pages->addWidget(scroll);
            return layout;
        };
        auto *connectionPage = page(QStringLiteral("连接 Mac"));
        auto *nearby = card(connectionPage, QStringLiteral("附近的 Mac"));
        auto *nearbyBody = static_cast<QVBoxLayout *>(nearby->layout());
        auto *titleItem = nearbyBody->takeAt(0);
        auto *nearbyTitle = titleItem->widget();
        delete titleItem;
        auto *searchRow = new QHBoxLayout;
        searchRow->addWidget(nearbyTitle);
        searchRow->addStretch();
        refresh = new QPushButton(QStringLiteral("刷新"));
        searchRow->addWidget(refresh);
        nearbyBody->addLayout(searchRow);
        deviceEmpty = new QLabel(QStringLiteral("正在搜索…"));
        deviceEmpty->setObjectName("muted");
        nearbyBody->addWidget(deviceEmpty);
        devices = new QListWidget;
        devices->setObjectName("devices");
        devices->setMinimumHeight(112);
        devices->setMaximumHeight(180);
        devices->hide();
        nearbyBody->addWidget(devices);
        auto *addressRows = new QFormLayout;
        host = new QLineEdit(settings.value("host").toString());
        host->setPlaceholderText(QStringLiteral("IP 地址或主机名"));
        host->setClearButtonEnabled(true);
        addressRows->addRow(QStringLiteral("Mac 地址"), host);
        nearbyBody->addLayout(addressRows);
        auto *network = form(card(connectionPage, QStringLiteral("网络")));
        link = new QComboBox;
        for (int speed : {100, 1000, 2500, 10000})
            link->addItem(speed < 1000 ? QStringLiteral("%1 Mbps").arg(speed)
                                       : QStringLiteral("%1 Gbps").arg(speed / 1000.0),
                          speed);
        const int savedLink =
            settings.value("linkMbps", settings.contains("host") ? 10000 : 1000).toInt();
        link->setCurrentIndex(std::max(0, link->findData(savedLink)));
        network->addRow(QStringLiteral("链路速率"), link);
        codec = new QComboBox;
        codec->addItem(QStringLiteral("PyroWave"), "pyrowave");
        codec->addItem(QStringLiteral("硬件 H.264"), "h264");
        codec->setCurrentIndex(std::max(0, codec->findData(settings.value("codec", "pyrowave"))));
        bitrateCodec = codec->currentData().toString();
        network->addRow(QStringLiteral("视频编码"), codec);
        auto updateRateLimit = [this] {
            mbps->setMaximum(std::min(codec->currentData() == "h264" ? 200 : 3000,
                                      link->currentData().toInt() * 7 / 10));
        };
        mbps = new QSpinBox;
        mbps->setMinimum(20);
        updateRateLimit();
        mbps->setSingleStep(10);
        mbps->setSuffix(QStringLiteral(" Mbps"));
        mbps->setKeyboardTracking(false);
        mbps->setValue(settings
                           .value(bitrateCodec + "Mbps",
                                  bitrateCodec == "h264" ? 50 : settings.value("mbps", 200))
                           .toInt());
        network->addRow(QStringLiteral("视频码率"), mbps);
        QObject::connect(
            codec, &QComboBox::currentIndexChanged, this, [this, updateRateLimit](int) {
                settings.setValue(bitrateCodec + "Mbps", mbps->value());
                bitrateCodec = codec->currentData().toString();
                updateRateLimit();
                mbps->setValue(
                    settings.value(bitrateCodec + "Mbps", bitrateCodec == "h264" ? 50 : 200)
                        .toInt());
            });
        QObject::connect(link, &QComboBox::currentIndexChanged, this, [updateRateLimit](int) {
            updateRateLimit();
        });
        connectionPage->addStretch();
        auto *displayPage = page(QStringLiteral("显示设置"));
        displayForm = form(card(displayPage, QStringLiteral("虚拟显示器")));
        presets = new QComboBox;
        presets->addItem(QStringLiteral("自定义"), QSize());
        QScreen *screen = QGuiApplication::primaryScreen();
        const QSize physical = nativeScreenPixels(screen);
        const QSize native(physical.width() & ~1, physical.height() & ~1);
        presets->addItem(
            QStringLiteral("本机屏幕 · %1 × %2").arg(native.width()).arg(native.height()), native);
        for (const auto size :
             {QSize(1920, 1080), QSize(1920, 1200), QSize(2240, 1400), QSize(2560, 1440),
              QSize(2560, 1600), QSize(2880, 1800), QSize(3840, 2160)})
            if (size != native)
                presets->addItem(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()),
                                 size);
        displayForm->addRow(QStringLiteral("分辨率"), presets);
        dimensions = new QWidget;
        auto *dimensionsRow = new QHBoxLayout(dimensions);
        dimensionsRow->setContentsMargins(0, 0, 0, 0);
        width = new QSpinBox;
        height = new QSpinBox;
        for (auto *box : {width, height}) {
            box->setRange(128, 8192);
            box->setSingleStep(2);
            box->setKeyboardTracking(false);
        }
        width->setValue(settings.value("width", native.width()).toInt());
        height->setValue(settings.value("height", native.height()).toInt());
        dimensionsRow->addWidget(width);
        dimensionsRow->addWidget(new QLabel(QStringLiteral("×")));
        dimensionsRow->addWidget(height);
        displayForm->addRow(QStringLiteral("宽 × 高"), dimensions);
        scale = new ScaleBox;
        scale->setRange(.5, 4);
        scale->setDecimals(4);
        scale->setSingleStep(.25);
        scale->setSuffix(QStringLiteral(" ×"));
        scale->setKeyboardTracking(false);
        scale->setValue(settings.value("scale", 1.5).toDouble());
        displayForm->addRow(QStringLiteral("界面缩放"), scale);
        fps = new QComboBox;
        for (int rate : {30, 60, 90, 120, 144})
            fps->addItem(QStringLiteral("%1 Hz").arg(rate), rate);
        fps->setCurrentIndex(std::max(0, fps->findData(settings.value("fps", 60).toInt())));
        displayForm->addRow(QStringLiteral("刷新率"), fps);
        fullscreen = new QCheckBox(QStringLiteral("全屏显示"));
        fullscreen->setChecked(settings.value("fullscreen", true).toBool());
        displayForm->addRow(QString(), fullscreen);
        preview = new QLabel;
        preview->setObjectName("muted");
        preview->setWordWrap(true);
        static_cast<QVBoxLayout *>(displayForm->parentWidget()->layout())->addWidget(preview);
        displayPage->addStretch();
        auto *audioPage = page(QStringLiteral("音频设置"));
        auto *audioForm = form(card(audioPage, QStringLiteral("播放")));
        audioEnabled = new QCheckBox(QStringLiteral("启用音频"));
        audioEnabled->setChecked(settings.value("audioEnabled", true).toBool());
        audioForm->addRow(QString(), audioEnabled);
        audioBuffer = new QComboBox;
        audioBuffer->addItem(QStringLiteral("自动"), 0);
        for (int ms : {1, 2, 4, 8, 10, 12, 16, 20, 30, 50, 80, 120, 200})
            audioBuffer->addItem(QStringLiteral("%1 ms").arg(ms), ms);
        audioBuffer->setCurrentIndex(
            std::max(0, audioBuffer->findData(settings.value("audioBufferMs", 0).toInt())));
        audioBuffer->setToolTip(
            QStringLiteral("更小延迟更低，更大播放更稳。数值为软件播放延迟目标，不含声卡延迟。"));
        audioForm->addRow(QStringLiteral("缓冲区大小"), audioBuffer);
        muteMacAudio = new QCheckBox(QStringLiteral("静音 Mac 本机播放"));
        muteMacAudio->setChecked(settings.value("muteMacAudio", false).toBool());
        audioForm->addRow(QString(), muteMacAudio);
        auto updateAudioControls = [this] {
            audioBuffer->setEnabled(audioEnabled->isChecked());
            muteMacAudio->setEnabled(audioEnabled->isChecked());
        };
        QObject::connect(audioEnabled, &QCheckBox::toggled, this, [updateAudioControls](bool) {
            updateAudioControls();
        });
        updateAudioControls();
        audioPage->addStretch();
        auto *inputPage = page(QStringLiteral("键盘与鼠标"));
        auto *inputForm = form(card(inputPage, QStringLiteral("输入控制")));
        exclusive = new QCheckBox(QStringLiteral("捕获系统快捷键"));
        exclusive->setChecked(settings.value("exclusiveKeyboard", true).toBool());
        inputForm->addRow(QString(), exclusive);
        gameMouse = new QCheckBox(QStringLiteral("游戏鼠标"));
        gameMouse->setChecked(settings.value("gameMouse", false).toBool());
        inputForm->addRow(QString(), gameMouse);
        auto *shortcutForm = form(card(inputPage, QStringLiteral("客户端快捷键")));
        releaseShortcut = new QKeySequenceEdit(QKeySequence::fromString(
            settings.value("releaseShortcut", "F8").toString(), QKeySequence::PortableText));
        releaseShortcut->setMaximumSequenceLength(1);
        shortcutForm->addRow(QStringLiteral("释放 / 恢复控制"), releaseShortcut);
        fullscreenShortcut = new QKeySequenceEdit(QKeySequence::fromString(
            settings.value("fullscreenShortcut", "Ctrl+Shift+F11").toString(),
            QKeySequence::PortableText));
        fullscreenShortcut->setMaximumSequenceLength(1);
        shortcutForm->addRow(QStringLiteral("切换全屏"), fullscreenShortcut);
        inputPage->addStretch();
        auto *footer = new QHBoxLayout;
        auto *state = new QVBoxLayout;
        state->setSpacing(4);
        status = new QLabel(QStringLiteral("未连接"));
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        stats = new QLabel;
        stats->setObjectName("muted");
        state->addWidget(status);
        state->addWidget(stats);
        footer->addLayout(state, 1);
        stop = new QPushButton(QStringLiteral("断开"));
        stop->setEnabled(false);
        apply = new QPushButton(QStringLiteral("连接"));
        apply->setObjectName("primary");
        apply->setDefault(true);
        footer->addWidget(stop);
        footer->addWidget(apply);
        root->addLayout(footer);
        QObject::connect(nav, &QListWidget::currentRowChanged, pages,
                         &QStackedWidget::setCurrentIndex);
        nav->setCurrentRow(0);
        QObject::connect(presets, &QComboBox::activated, this, [this](int i) {
            const QSize size = presets->itemData(i).toSize();
            if (size.isValid()) {
                width->setValue(size.width());
                height->setValue(size.height());
                syncPreset();
            } else
                displayForm->setRowVisible(dimensions, true);
        });
        QObject::connect(width, &QSpinBox::valueChanged, this, [this](int) {
            syncPreset();
        });
        QObject::connect(height, &QSpinBox::valueChanged, this, [this](int) {
            syncPreset();
        });
        QObject::connect(scale, &QDoubleSpinBox::valueChanged, this, [this](double) {
            updatePreview();
        });
        syncPreset();
        QObject::connect(gameMouse, &QCheckBox::toggled, this, [this](bool) {
            updateGameMouse();
        });
        QObject::connect(refresh, &QPushButton::clicked, this, [this] {
            scan();
        });
        discovery.found = [this](const QString &name, const QHostAddress &address) {
            const QString ip = address.toString();
            for (int i = 0; i < devices->count(); i++)
                if (devices->item(i)->data(Qt::UserRole).toString() == ip)
                    return;
            auto *item = new QListWidgetItem(name + "\n" + ip, devices);
            item->setData(Qt::UserRole, ip);
            devices->show();
            deviceEmpty->hide();
            if (host->text().isEmpty()) {
                devices->setCurrentItem(item);
                host->setText(ip);
            }
        };
        QObject::connect(devices, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            host->setText(item->data(Qt::UserRole).toString());
        });
        QObject::connect(devices, &QListWidget::itemDoubleClicked, this,
                         [this](QListWidgetItem *item) {
                             host->setText(item->data(Qt::UserRole).toString());
                             connectReceiver();
                         });
        QObject::connect(apply, &QPushButton::clicked, this, [this] {
            connectReceiver();
        });
        QObject::connect(host, &QLineEdit::returnPressed, this, [this] {
            connectReceiver();
        });
        QObject::connect(stop, &QPushButton::clicked, this, [this] {
            restartPending = false;
            stopReceiver();
        });
        receiver.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&receiver, &QProcess::readyReadStandardOutput, this, [this] {
            readOutput();
        });
        QObject::connect(&receiver, &QProcess::started, this, [this] {
            apply->setText(QStringLiteral("应用并重连"));
            apply->setEnabled(true);
            updateGameMouse();
        });
        QObject::connect(
            &receiver, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    status->setText(QStringLiteral("启动失败：") + receiver.errorString());
                    apply->setEnabled(true);
                    stop->setEnabled(false);
                    if (closing)
                        close();
                }
            });
        QObject::connect(&receiver, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                         [this](int code, QProcess::ExitStatus reason) {
                             readOutput();
                             ++stopGeneration;
                             if (closing) {
                                 close();
                                 return;
                             }
                             if (restartPending) {
                                 restartPending = false;
                                 startReceiver();
                                 return;
                             }
                             status->setText(stopping || code == 0
                                                 ? QStringLiteral("已断开")
                                                 : QStringLiteral("连接结束（%1）")
                                                       .arg(reason == QProcess::CrashExit
                                                                ? QStringLiteral("异常退出")
                                                                : QString::number(code)));
                             apply->setText(QStringLiteral("连接"));
                             apply->setEnabled(true);
                             stop->setEnabled(false);
                             stopping = false;
                             stats->clear();
                         });
        QTimer::singleShot(0, this, [this] {
            scan();
        });
    }

    void setInitialHost(const QString &address) {
        if (!address.isEmpty())
            host->setText(address);
    }

    void setInitialBitrate(int value) {
        if (value)
            mbps->setValue(value);
    }

    void connectOnStart() {
        QTimer::singleShot(0, this, [this] {
            connectReceiver();
        });
    }
};
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ThunderDisplay");
    QCoreApplication::setApplicationName("ThunderDisplayClient");
    QCoreApplication::setApplicationVersion("0.2.0");
    app.setWindowIcon(QIcon(":/assets/thunderdisplay.svg"));
    QString host;
    bool connectNow = false;
    int initialBitrate = 0;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "--host" && i + 1 < args.size())
            host = args[++i];
        else if (args[i] == "--mbps" && i + 1 < args.size()) {
            bool valid = false;
            initialBitrate = args[++i].toInt(&valid);
            if (!valid || initialBitrate < 20 || initialBitrate > 3000) {
                qCritical("Bitrate must be between 20 and 3000 Mbps");
                return 1;
            }
        } else if (args[i] == "--connect")
            connectNow = true;
        else if (args[i] == "--help") {
            qInfo("Usage: thunder-display-client [--host MAC_ADDRESS] [--mbps 200] [--connect]");
            return 0;
        } else {
            qCritical(
                "Usage: thunder-display-client [--host MAC_ADDRESS] [--mbps 200] [--connect]");
            return 1;
        }
    }
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QLockFile lock(runtime + "/thunder-display-client.lock");
    if (!lock.tryLock(0)) {
        qCritical("ThunderDisplay is already running");
        return 1;
    }
    ClientWindow window;
    window.setInitialHost(host);
    window.setInitialBitrate(initialBitrate);
    window.show();
    if (connectNow)
        window.connectOnStart();
    return app.exec();
}
