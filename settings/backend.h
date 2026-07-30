#pragma once

#include "miniaudio.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <string>

class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList devicenames MEMBER m_devicenames NOTIFY changed)
    Q_PROPERTY(QString language MEMBER m_language NOTIFY changed)
    Q_PROPERTY(int deviceindex MEMBER m_deviceindex NOTIFY changed)
    Q_PROPERTY(bool customkey MEMBER m_customkey NOTIFY changed)

  public:
    explicit Settings(QObject *parent = nullptr);
    ~Settings();

    Q_INVOKABLE void select_device(int index);
    Q_INVOKABLE void set_language(const QString &lang);
    Q_INVOKABLE void set_apikey(const QString &key);
    Q_INVOKABLE void reset_apikey();
    Q_INVOKABLE void uninstall();

  signals:
    void changed();

  private:
    void load();
    void save();
    void signal_daemon();
    std::string config_dir() const;

    QStringList m_devicenames;
    QString m_language;
    QString m_device;
    QString m_apikey;
    int m_deviceindex;
    bool m_customkey;
    ma_context m_context;
    ma_device_info *m_captureinfos;
    ma_uint32 m_capturecount;
};
