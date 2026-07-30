#pragma once

#include "miniaudio.h"
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <vector>

struct DesktopApp {
    QString name;
    QString exec;
    QStringList matchers;
};

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(qreal level READ level NOTIFY levelChanged)

  public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend();

    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void quitNow();

    QString state() const;
    qreal level() const;

  signals:
    void stateChanged();
    void levelChanged();
    void responseReady(const QString &text, int displayMs, const QString &kind);

  private slots:
    void poll();
    void handleReply();

  private:
    void setState(const QString &s);
    void send_llm(const QString &system, const QString &user);
    void dispatch(const QString &content);
    void finish(const QString &text, int displayMs, const QString &kind);
    void close();
    void scan_desktops();
    const DesktopApp *resolve_app(const QString &query) const;
    std::int64_t elapsed_ms() const;
    static void call_back(ma_device *pDevice, void *pOutput, const void *pInput,
                          ma_uint32 frameCount);
    void enumerate_devices();

    QNetworkAccessManager m_networkmanager;
    QString m_apikey;
    QString m_applist;
    QString m_stage;
    QString m_input;
    QByteArray m_audiodata;
    std::vector<DesktopApp> m_apps;
    ma_context m_context;
    ma_device_info *m_captureinfos;
    ma_uint32 m_capturecount;
    ma_device m_device;
    ma_device_config m_config;
    bool m_micOk;
    QString m_state;
    qreal m_level;
    QTimer m_poll;
    std::chrono::steady_clock::time_point m_clock;
    std::future<void> m_whisperReady;
    std::atomic<float> m_rms;
    std::atomic<std::int64_t> m_lastVoiceMs;
    std::atomic<bool> m_spoke;
};
