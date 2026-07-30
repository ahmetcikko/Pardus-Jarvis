#define MINIAUDIO_IMPLEMENTATION
#include "backend.h"
#include <QDebug>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

Settings::Settings(QObject *parent)
    : QObject(parent), m_language("tr"), m_deviceindex(0), m_customkey(false),
      m_captureinfos(nullptr), m_capturecount(0) {
    if (ma_context_init(nullptr, 0, nullptr, &(*this).m_context) !=
        MA_SUCCESS) {
        qDebug() << "Error for context_init() function";
    }
    if (ma_context_get_devices(&(*this).m_context, nullptr, nullptr,
                               &(*this).m_captureinfos,
                               &(*this).m_capturecount) != MA_SUCCESS) {
        qDebug() << "Error during getting devices.";
    }
    for (ma_uint32 device = 0; device < (*this).m_capturecount; device++) {
        (*this).m_devicenames.append((*this).m_captureinfos[device].name);
    }
    load();
    for (int i = 0; i < m_devicenames.size(); i++)
        if (m_devicenames[i] == m_device)
            m_deviceindex = i;
    emit changed();
}

Settings::~Settings() { ma_context_uninit(&m_context); }

std::string Settings::config_dir() const {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string base = xdg && *xdg ? std::string(xdg)
                                   : std::string(home ? home : "") + "/.config";
    return base + "/pardusjarvis";
}

void Settings::load() {
    std::ifstream f(config_dir() + "/config");
    std::string sline;
    while (std::getline(f, sline)) {
        QString line = QString::fromStdString(sline);
        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        QString key = line.left(eq);
        QString value = line.mid(eq + 1);
        if (key == "device")
            m_device = value;
        else if (key == "language" && !value.isEmpty())
            m_language = value;
        else if (key == "apikey")
            m_apikey = value;
    }
    m_customkey = !m_apikey.isEmpty();
}

void Settings::save() {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    std::string path = config_dir() + "/config";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return;
    f << "device=" << m_device.trimmed().toStdString() << "\n";
    f << "language=" << m_language.toStdString() << "\n";
    if (!m_apikey.isEmpty())
        f << "apikey=" << m_apikey.toStdString() << "\n";
    f.close();

    // config file holds the api key in plaintext, so lock it down to owner-only
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 ec);
    emit changed();
}

void Settings::signal_daemon() {
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {const_cast<char *>("pkill"), const_cast<char *>("-HUP"),
                        const_cast<char *>("-f"),
                        const_cast<char *>("Pardus-Jarvis-Daemon"), nullptr};
        execvp("pkill", args);
        _exit(-1);
    }
    if (pid > 0)
        waitpid(pid, nullptr, 0);
}

void Settings::select_device(int index) {
    if (index < 0 || index >= m_devicenames.size())
        return;
    m_deviceindex = index;
    m_device = m_devicenames[index];
    save();
    signal_daemon();
}

void Settings::set_language(const QString &lang) {
    m_language = lang;
    save();
}

void Settings::set_apikey(const QString &key) {
    QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) {
        reset_apikey();
        return;
    }
    m_apikey = trimmed;
    m_customkey = true;
    save();
}

void Settings::reset_apikey() {
    m_apikey.clear();
    m_customkey = false;
    save();
}

void Settings::uninstall() {
    pid_t kill_pid = fork();
    if (kill_pid == 0) {
        char *args[] = {const_cast<char *>("pkill"), const_cast<char *>("-f"),
                        const_cast<char *>("Pardus-Jarvis-Daemon"), nullptr};
        execvp("pkill", args);
        _exit(-1);
    }
    if (kill_pid > 0)
        waitpid(kill_pid, nullptr, 0);
    std::error_code ec;
    std::filesystem::remove_all(config_dir(), ec);
    std::filesystem::remove("/tmp/pardusjarvis.pid", ec);
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {
            const_cast<char *>("pkexec"),       const_cast<char *>("apt-get"),
            const_cast<char *>("purge"),        const_cast<char *>("-y"),
            const_cast<char *>("pardusjarvis"), nullptr};
        execvp("pkexec", args);
        _exit(-1);
    }
    std::exit(0);
}
