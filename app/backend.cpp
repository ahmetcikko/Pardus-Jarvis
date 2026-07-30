#include "backend.h"
#include "apikey.h"
#include "audio_processor.h"
#include "whisper_wrapper.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <linux/limits.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static constexpr std::int64_t kHardCapMs = 10000;
static constexpr std::int64_t kSilenceStopMs = 1300;
static constexpr std::int64_t kNoSpeechCloseMs = 5000;
static constexpr float kVoiceRmsThreshold = 0.012f;
static constexpr int kMaxTokens = 512;

static const char *kPromptRoute =
    "You are Pardus Jarvis, a voice assistant for Pardus Linux. Input is "
    "Turkish speech transcribed by Whisper, so it may be slightly misheard - "
    "interpret it charitably, but never invent an action the user did not "
    "clearly ask for. Reply with ONE JSON object only.\n"
    "Decide the user's intent:\n"
    "open/launch/run an application -> {\"action\":\"open_app\"}\n"
    "open a website, or a browser named with a search term (even if they say "
    "'ac'/'open' first, e.g. 'Chrome'u ac ve X arat') -> "
    "{\"action\":\"open_url\",\"target\":\"<https url>\"}; for a search term "
    "build https://www.google.com/search?q=<term>. A browser with NO search "
    "term is open_app.\n"
    "close/quit/kill a RUNNING APPLICATION the user names -> "
    "{\"action\":\"close_app\"}. Only when they clearly mean an application. "
    "'Dosyalari kapat', 'sekmeyi kapat', 'pencereyi kapat' are about files, "
    "tabs or windows INSIDE an app, not the app itself - those are chat, NOT "
    "close_app. If no application is named, do NOT guess one.\n"
    "change volume -> "
    "{\"action\":\"volume\",\"target\":\"<0-100|up|down|mute|unmute>\"} - a "
    "number when they name a level, up/down for louder/quieter, mute/unmute "
    "for silencing.\n"
    "reboot/shut down/sleep the whole COMPUTER, only if they explicitly name "
    "the machine ('bilgisayari kapat', 'sistemi kapat'); a bare 'kapat' or a "
    "named app is close_app, never this -> "
    "{\"action\":\"system_power\",\"target\":\"<reboot|shutdown|sleep>\"}\n"
    "anything else -> {\"action\":\"chat\",\"reply\":\"<short Turkish "
    "answer>\"}\n"
    "Polite forms ('acar misin', 'can you open') are REQUESTS - always act, "
    "never chat about ability.\n"
    "Examples:\n"
    "'Firefox'u acabilir misin' -> {\"action\":\"open_app\"}\n"
    "'Chrome'u ac ve kedileri arat' -> "
    "{\"action\":\"open_url\",\"target\":\"https://www.google.com/"
    "search?q=kediler\"}\n"
    "'bilgisayari kapat' -> "
    "{\"action\":\"system_power\",\"target\":\"shutdown\"}\n";

static const char *kPromptOpenA =
    "Pardus Jarvis. The user wants an app opened. Installed apps: ";
static const char *kPromptOpenB =
    "\nReply with ONE JSON object: {\"action\":\"open_app\",\"target\":\"<name "
    "copied exactly from the list>\"} - pick the one they mean even if their "
    "words differ.";
static const char *kPromptCloseA =
    "Pardus Jarvis. The user wants a running application closed. These are "
    "process names of what is currently running: ";
static const char *kPromptCloseB =
    "\nReply with ONE JSON object only. If one of them is clearly the "
    "application the user named: "
    "{\"action\":\"close_app\",\"target\":\"<name copied exactly from the "
    "list>\"} - the process name may differ from the app's visible name "
    "(e.g. the Files app runs as 'nautilus'), so match on meaning.\n"
    "If NOTHING in the list is clearly what they named, do NOT guess and do "
    "NOT pick the closest item - reply this JSON instead: "
    "{\"action\":\"chat\",\"reply\":\"<short Turkish answer saying you could "
    "not find that application>\"}.";

static ma_decoder g_sounddecoder;
static ma_device g_sounddevice;
static bool g_soundactive = false;

static void sound_call_back(ma_device *pDevice, void *pOutput, const void *,
                            ma_uint32 frameCount) {
    ma_decoder *decoder = static_cast<ma_decoder *>((*pDevice).pUserData);
    if (!decoder)
        return;
    ma_decoder_read_pcm_frames(decoder, pOutput, frameCount, nullptr);
}

static void play_sound_if_exists(const QString &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path.toStdString(), ec))
        return;
    if (g_soundactive) {
        ma_device_uninit(&g_sounddevice);
        ma_decoder_uninit(&g_sounddecoder);
        g_soundactive = false;
    }
    if (ma_decoder_init_file(path.toUtf8().constData(), nullptr,
                             &g_sounddecoder) != MA_SUCCESS)
        return;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = g_sounddecoder.outputFormat;
    config.playback.channels = g_sounddecoder.outputChannels;
    config.sampleRate = g_sounddecoder.outputSampleRate;
    config.dataCallback = sound_call_back;
    config.pUserData = &g_sounddecoder;
    if (ma_device_init(nullptr, &config, &g_sounddevice) != MA_SUCCESS) {
        ma_decoder_uninit(&g_sounddecoder);
        return;
    }
    if (ma_device_start(&g_sounddevice) != MA_SUCCESS) {
        ma_device_uninit(&g_sounddevice);
        ma_decoder_uninit(&g_sounddecoder);
        return;
    }
    g_soundactive = true;
}

static QString normalize(QString s) {
    s = s.trimmed().toLower();
    s.remove(QChar(0x0307));
    QString out;
    out.reserve(s.size());
    for (QChar ch : s) {
        switch (ch.unicode()) {
        case 0x00E7:
            out += 'c';
            break;
        case 0x011F:
            out += 'g';
            break;
        case 0x0131:
            out += 'i';
            break;
        case 0x00F6:
            out += 'o';
            break;
        case 0x015F:
            out += 's';
            break;
        case 0x00FC:
            out += 'u';
            break;
        case 0x00E2:
            out += 'a';
            break;
        case 0x00EE:
            out += 'i';
            break;
        case 0x00FB:
            out += 'u';
            break;
        default:
            out += ch;
        }
    }
    return out.simplified();
}

static int levenshtein(const QString &a, const QString &b) {
    int n = a.size();
    int m = b.size();
    if (n == 0)
        return m;
    if (m == 0)
        return n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; j++)
        prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] =
                qMin(qMin(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev.swap(cur);
    }
    return prev[m];
}

static int score_pair(const QString &q, const QString &c) {
    if (q.isEmpty() || c.isEmpty())
        return 0;
    if (q == c)
        return 100;
    if (c.startsWith(q) || q.startsWith(c))
        return 88;
    if (c.contains(q))
        return 74;
    if (q.contains(c))
        return 70;
    int d = levenshtein(q, c);
    int m = qMax(q.size(), c.size());
    return int(100.0 * (1.0 - double(d) / m));
}

static bool spawn(const QStringList &args) {
    std::vector<std::string> strs;
    for (const QString &a : args)
        strs.push_back(a.toStdString());
    std::vector<char *> argv;
    for (std::string &s : strs)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        const char *home = getenv("HOME");
        if (home && *home)
            static_cast<void>(chdir(home));
        execvp(argv[0], argv.data());
        _exit(-1);
    }
    if (pid < 0)
        return false;
    std::thread([pid]() { waitpid(pid, nullptr, 0); }).detach();
    return true;
}

static bool launch_app(const DesktopApp &app) {
    QStringList tokens;
    QString cur;
    bool inQuote = false;
    for (int i = 0; i < app.exec.size(); i++) {
        QChar ch = app.exec[i];
        if (inQuote) {
            if (ch == '\\' && i + 1 < app.exec.size())
                cur += app.exec[++i];
            else if (ch == '"')
                inQuote = false;
            else
                cur += ch;
        } else if (ch == '"') {
            inQuote = true;
        } else if (ch == ' ' || ch == '\t') {
            if (!cur.isEmpty()) {
                tokens << cur;
                cur.clear();
            }
        } else {
            cur += ch;
        }
    }
    if (!cur.isEmpty())
        tokens << cur;
    QStringList args;
    for (const QString &t : tokens) {
        QString clean;
        for (int i = 0; i < t.size(); i++) {
            if (t[i] == '%' && i + 1 < t.size() && t[i + 1] == '%') {
                clean += '%';
                i++;
            } else if (t[i] == '%' && i + 1 < t.size()) {
                i++;
            } else if (t[i] != '%') {
                clean += t[i];
            }
        }
        if (!clean.isEmpty())
            args << clean;
    }
    if (args.isEmpty())
        return false;
    return spawn(args);
}

static bool is_critical_comm(const QString &comm) {
    static const std::vector<QString> critical = {"systemd",
                                                  "init",
                                                  "dbus-daemon",
                                                  "dbus-broker",
                                                  "gnome-shell",
                                                  "kwin_x11",
                                                  "kwin_wayland",
                                                  "plasmashell",
                                                  "Xorg",
                                                  "Xwayland",
                                                  "gdm",
                                                  "gdm3",
                                                  "lightdm",
                                                  "sddm",
                                                  "pipewire",
                                                  "pipewire-pulse",
                                                  "wireplumber",
                                                  "NetworkManager",
                                                  "polkitd",
                                                  "systemd-logind",
                                                  "Pardus Jarvis",
                                                  "Pardus-Jarvis-Daemon",
                                                  "Pardus-Jarvis-Settings"};

    // /proc truncates comm to 15 chars, so long names need the truncated compare too
    for (const QString &name : critical)
        if (comm.compare(name, Qt::CaseInsensitive) == 0 ||
            comm.compare(name.left(15), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

static int parent_pid(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string content;
    std::getline(f, content);
    size_t p = content.rfind(')');
    if (p == std::string::npos)
        return 0;
    int ppid = 0;

    // comm can itself contain spaces/parens, so parse from the last ')' not the first
    if (std::sscanf(content.c_str() + p + 1, " %*c %d", &ppid) != 1)
        return 0;
    return ppid;
}

static bool is_own_lineage(int pid) {
    int cur = getpid();
    for (int i = 0; i < 64 && cur > 1; i++) {
        if (cur == pid)
            return true;
        cur = parent_pid(cur);
    }
    return false;
}

static bool is_system_path(const std::string &path) {
    static const std::vector<std::string> roots = {
        "/usr/lib/systemd/", "/lib/systemd/",  "/usr/libexec/",
        "/usr/sbin/",        "/sbin/",         "/usr/lib/gdm",
        "/usr/lib/polkit-1/", "/usr/lib/xorg/", "/usr/lib/dbus-1.0/"};
    for (const std::string &r : roots)
        if (path.rfind(r, 0) == 0)
            return true;
    return false;
}

static std::string cgroup_path(const std::string &line) {
    // cgroup v1 lines are "N:controller:path", v2 is "0::path" - skip past both colons either way
    size_t first = line.find(':');
    if (first == std::string::npos)
        return line;
    size_t second = line.find(':', first + 1);
    if (second == std::string::npos)
        return line;
    return line.substr(second + 1);
}

static bool in_app_slice(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line;
    while (std::getline(f, line))
        if (cgroup_path(line).find("/app.slice/") != std::string::npos)
            return true;
    return false;
}

static bool is_protected_pid(int pid) {
    if (pid <= 1)
        return true;
    std::string base = "/proc/" + std::to_string(pid);
    struct stat st;
    if (stat(base.c_str(), &st) != 0)
        return true;
    if (st.st_uid != getuid())
        return true;

    // only shield our own process tree from itself when it's not a real desktop app,
    // otherwise a terminal that launched us would become unkillable
    if (is_own_lineage(pid) && !in_app_slice(pid))
        return true;
    std::ifstream cmd(base + "/cmdline");
    std::string arg0;
    std::getline(cmd, arg0, '\0');
    if (arg0.empty())
        return true;
    char exe[PATH_MAX];
    ssize_t len = readlink((base + "/exe").c_str(), exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        if (is_system_path(exe))
            return true;
    }
    std::ifstream f(base + "/cgroup");
    std::string line;
    while (std::getline(f, line)) {
        std::string path = cgroup_path(line);
        if (path.find("/system.slice/") != std::string::npos ||
            path.find("/init.scope") != std::string::npos ||
            path.find("/session.slice/") != std::string::npos ||
            path.find("/background.slice/") != std::string::npos)
            return true;
        size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            std::string leaf = path.substr(slash + 1);
            if (leaf.size() > 8 &&
                leaf.compare(leaf.size() - 8, 8, ".service") == 0)
                return true;
        }
    }
    return false;
}

enum class KillOutcome { Killed, NotFound, Protected };

static KillOutcome terminate_comm(const QString &name) {
    if (is_critical_comm(name))
        return KillOutcome::Protected;
    QString short15 = name.left(15);
    bool any = false;
    bool blocked = false;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator("/proc", ec)) {
        std::string pidname = entry.path().filename().string();
        if (pidname.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream f("/proc/" + pidname + "/comm");
        std::string comm;
        std::getline(f, comm);
        QString c = QString::fromStdString(comm);
        if (c.compare(name, Qt::CaseInsensitive) != 0 &&
            c.compare(short15, Qt::CaseInsensitive) != 0)
            continue;
        int pid = std::stoi(pidname);
        if (is_critical_comm(c) || is_protected_pid(pid)) {
            blocked = true;
            continue;
        }
        if (kill(pid, SIGTERM) == 0)
            any = true;
    }
    if (any)
        return KillOutcome::Killed;
    return blocked ? KillOutcome::Protected : KillOutcome::NotFound;
}

static QString running_apps() {
    std::unordered_map<QString, std::uint64_t> apps;
    long page = sysconf(_SC_PAGESIZE);
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator("/proc", ec)) {
        std::string name = entry.path().filename().string();
        if (name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        struct stat st;
        if (stat(("/proc/" + name).c_str(), &st) != 0 || st.st_uid != getuid())
            continue;
        std::ifstream cmd("/proc/" + name + "/cmdline");
        std::string arg0;
        std::getline(cmd, arg0, '\0');
        if (arg0.empty())
            continue;
        std::ifstream cf("/proc/" + name + "/comm");
        std::string comm;
        std::getline(cf, comm);
        if (comm.empty())
            continue;
        QString qc = QString::fromStdString(comm);
        if (is_critical_comm(qc) || is_protected_pid(std::stoi(name)))
            continue;
        std::ifstream sm("/proc/" + name + "/statm");
        std::uint64_t sz = 0, rss = 0;
        sm >> sz >> rss;
        std::uint64_t bytes = rss * std::uint64_t(page);
        auto it = apps.find(qc);
        if (it == apps.end() || bytes > (*it).second)
            apps[qc] = bytes;
    }
    std::vector<std::pair<std::uint64_t, QString>> sorted;
    for (const auto &a : apps)
        sorted.push_back({a.second, a.first});
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &x, const auto &y) { return x.first > y.first; });
    QStringList names;
    for (size_t i = 0; i < sorted.size() && i < 40; i++)
        names.append(sorted[i].second);
    return names.join(", ");
}

static bool open_url(const QString &url) {
    QString u = url.trimmed();
    if (u.isEmpty())
        return false;
    if (!u.contains("://"))
        u = "https://" + u;
    return spawn({"xdg-open", u});
}

static QString config_value(const char *key) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string path = xdg && *xdg ? std::string(xdg)
                                   : std::string(home ? home : "") + "/.config";
    path += "/pardusjarvis/config";
    std::ifstream f(path);
    std::string line;
    std::string want = std::string(key) + "=";
    while (std::getline(f, line))
        if (line.rfind(want, 0) == 0)
            return QString::fromStdString(line.substr(want.size()));
    return "";
}

static QStringList xdg_application_dirs() {
    QStringList dirs;
    const char *home = getenv("HOME");
    const char *dataHome = getenv("XDG_DATA_HOME");
    std::string base = dataHome && *dataHome
                           ? std::string(dataHome)
                           : std::string(home ? home : "") + "/.local/share";
    dirs.append(QString::fromStdString(base + "/applications"));
    const char *dataDirs = getenv("XDG_DATA_DIRS");
    std::string list =
        dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";
    for (const QString &d :
         QString::fromStdString(list).split(':', Qt::SkipEmptyParts))
        dirs.append(d + "/applications");
    return dirs;
}

static QString kv_get(const std::unordered_map<QString, QString> &kv,
                      const QString &key) {
    auto it = kv.find(key);
    return it != kv.end() ? (*it).second : QString();
}

static bool looks_like_url(const QString &t) {
    if (t.contains("://") || t.startsWith("www."))
        return true;
    for (const char *tld : {".com", ".org", ".net", ".io", ".tr", ".dev"})
        if (t.contains(tld))
            return true;
    return false;
}

Backend::Backend(QObject *parent)
    : QObject(parent), m_apikey(kDefaultApiKey), m_captureinfos(nullptr),
      m_capturecount(0), m_micOk(false), m_state("idle"), m_level(0.0),
      m_rms(0.0f), m_lastVoiceMs(0), m_spoke(false) {
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';

        // relative paths like sounds/ and the whisper model only resolve correctly from here
        chdir(std::filesystem::path(exe).parent_path().string().c_str());
    }
    m_config = ma_device_config_init(ma_device_type_capture);
    m_config.capture.format = ma_format_f32;
    m_config.capture.channels = 1;
    m_config.sampleRate = 16000;
    m_config.dataCallback = call_back;
    m_config.pUserData = this;
    if (ma_context_init(nullptr, 0, nullptr, &(*this).m_context) !=
        MA_SUCCESS) {
        qDebug() << "Error for context_init() function";
    }
    enumerate_devices();
    QString confkey = config_value("apikey");
    if (!confkey.isEmpty())
        m_apikey = confkey;
    QString confdev = config_value("device");
    for (ma_uint32 i = 0; i < m_capturecount && !confdev.isEmpty(); i++)
        if (confdev == m_captureinfos[i].name)
            m_config.capture.pDeviceID = &m_captureinfos[i].id;
    m_micOk = ma_device_init(&m_context, &m_config, &m_device) == MA_SUCCESS;
    m_whisperReady = std::async(std::launch::async, []() { whisper_init(); });
    scan_desktops();
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, &Backend::poll);
}

Backend::~Backend() {
    ma_device_uninit(&m_device);
    ma_context_uninit(&m_context);
}

QString Backend::state() const { return m_state; }

qreal Backend::level() const { return m_level; }

std::int64_t Backend::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - m_clock)
        .count();
}

void Backend::setState(const QString &s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

void Backend::startRecording() {
    if (m_state != "idle")
        return;
    play_sound_if_exists("sounds/opening.mp3");
    if (!m_micOk) {
        finish("Mikrofon bulunamadı.", 3000, "error");
        return;
    }
    m_audiodata.clear();
    m_spoke = false;
    m_rms = 0.0f;
    m_lastVoiceMs = 0;
    m_clock = std::chrono::steady_clock::now();
    ma_device_start(&m_device);
    setState("listening");
    m_poll.start();
}

void Backend::call_back(ma_device *pDevice, void *, const void *pInput,
                        ma_uint32 frameCount) {
    Backend *self = static_cast<Backend *>((*pDevice).pUserData);
    if (!self || !pInput)
        return;
    ma_uint32 byteCount =
        frameCount * ma_get_bytes_per_frame((*pDevice).capture.format,
                                            (*pDevice).capture.channels);
    (*self).m_audiodata.append(static_cast<const char *>(pInput), byteCount);
    const float *samples = static_cast<const float *>(pInput);
    double sum = 0.0;
    for (ma_uint32 i = 0; i < frameCount; i++)
        sum += double(samples[i]) * samples[i];
    float rms = frameCount ? float(std::sqrt(sum / frameCount)) : 0.0f;
    (*self).m_rms.store(rms);
    if (rms > kVoiceRmsThreshold) {
        (*self).m_spoke.store(true);
        (*self).m_lastVoiceMs.store((*self).elapsed_ms());
    }
}

void Backend::poll() {
    qreal lv = qBound(0.0, qreal(m_rms.load()) * 8.0, 1.0);
    if (!qFuzzyCompare(lv + 1.0, m_level + 1.0)) {
        m_level = lv;
        emit levelChanged();
    }
    if (m_state != "listening")
        return;
    std::int64_t elapsed = elapsed_ms();
    if (m_spoke.load()) {
        if (elapsed >= kHardCapMs ||
            elapsed - m_lastVoiceMs.load() >= kSilenceStopMs)
            stopRecording();
    } else if (elapsed >= kNoSpeechCloseMs) {
        m_poll.stop();
        ma_device_stop(&m_device);
        close();
    }
}

void Backend::stopRecording() {
    m_poll.stop();
    ma_device_stop(&m_device);
    setState("thinking");
    QByteArray audiocopy = m_audiodata;
    std::thread([this, audiocopy]() {
        m_whisperReady.wait();
        QByteArray processed = audiocopy;
        float *samples_rw = reinterpret_cast<float *>(processed.data());
        int n_samples = processed.size() / sizeof(float);
        process_audio(samples_rw, n_samples);
        QString finalinput = QString::fromStdString(transcribe(
            reinterpret_cast<const float *>(processed.constData()), n_samples));
        QMetaObject::invokeMethod(this, [this, finalinput]() {
            QString t = finalinput.trimmed();
            qDebug() << "Transcription" << t;
            if (t.isEmpty()) {
                finish("Sizi duyamadım.", 2000, "error");
                return;
            }
            m_input = t;
            m_stage = "route";
            send_llm(QString(kPromptRoute), t);
        });
    }).detach();
}

void Backend::send_llm(const QString &system, const QString &user) {
    QUrl url("https://api.groq.com/openai/v1/chat/completions");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_apikey.toUtf8());
    request.setTransferTimeout(15000);
    QJsonObject body{
        {"model", "llama-3.1-8b-instant"},
        {"temperature", 0},
        {"max_tokens", kMaxTokens},
        {"response_format", QJsonObject{{"type", "json_object"}}},
        {"messages",
         QJsonArray{QJsonObject{{"role", "system"}, {"content", system}},
                    QJsonObject{{"role", "user"}, {"content", user}}}}};
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_networkmanager.post(request, data);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     &Backend::handleReply);
}

void Backend::handleReply() {
    auto *reply = qobject_cast<QNetworkReply *>((*this).sender());
    if (!reply)
        return;
    if ((*reply).error() == QNetworkReply::NoError) {
        QJsonObject root = QJsonDocument::fromJson((*reply).readAll()).object();
        QString content = root["choices"]
                              .toArray()
                              .at(0)
                              .toObject()["message"]
                              .toObject()["content"]
                              .toString();
        dispatch(content);
    } else {
        finish("Bağlantı hatası.", 3500, "error");
    }
    (*reply).deleteLater();
}

void Backend::dispatch(const QString &content) {
    QString raw = content.trimmed();
    int a = raw.indexOf('{');
    int b = raw.lastIndexOf('}');
    QJsonObject obj;
    if (a >= 0 && b > a)
        obj = QJsonDocument::fromJson(raw.mid(a, b - a + 1).toUtf8()).object();
    QString action = obj["action"].toString();

    // m_stage tracks whether this reply is the initial intent routing or a follow-up
    // pick from a specific app/process list, since both go through the same dispatch path
    if (m_stage == "route" && action == "open_app") {
        m_stage = "open";
        send_llm(QString(kPromptOpenA) + m_applist + kPromptOpenB, m_input);
        return;
    }
    if (m_stage == "route" && action == "close_app") {
        m_stage = "close";
        send_llm(QString(kPromptCloseA) + running_apps() + kPromptCloseB,
                 m_input);
        return;
    }
    if (action == "open_app") {
        QString target = obj["target"].toString().trimmed();
        if (looks_like_url(target)) {
            open_url(target);
            finish("Açılıyor…", 2200, "action");
        } else if (const DesktopApp *app = resolve_app(target)) {
            if (launch_app(*app))
                finish((*app).name + " açılıyor…", 2200, "action");
            else
                finish("\"" + target + "\" bulunamadı.", 3000, "error");
        } else {
            finish("\"" + target + "\" bulunamadı.", 3000, "error");
        }
    } else if (action == "close_app") {
        QString target = obj["target"].toString().trimmed();
        KillOutcome outcome = terminate_comm(target);
        if (outcome == KillOutcome::Killed)
            finish(target + " kapatılıyor…", 2200, "action");
        else if (outcome == KillOutcome::Protected)
            finish("\"" + target + "\" korumalı bir sistem işlemi, kapatamam.",
                   3500, "error");
        else
            finish("\"" + target + "\" çalışmıyor.", 3000, "error");
    } else if (action == "volume") {
        QString t = obj["target"].toString().trimmed().toLower();
        if (t == "mute") {
            spawn({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "1"});
            finish("Ses kapatıldı.", 2200, "action");
        } else if (t == "unmute") {
            spawn({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "0"});
            finish("Ses açıldı.", 2200, "action");
        } else if (t == "up") {
            spawn({"pactl", "set-sink-volume", "@DEFAULT_SINK@", "+10%"});
            finish("Ses yükseltildi.", 2200, "action");
        } else if (t == "down") {
            spawn({"pactl", "set-sink-volume", "@DEFAULT_SINK@", "-10%"});
            finish("Ses kısıldı.", 2200, "action");
        } else {
            bool okNum = false;
            int level = t.remove('%').toInt(&okNum);
            if (okNum && level >= 0 && level <= 100) {
                spawn({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "0"});
                spawn({"pactl", "set-sink-volume", "@DEFAULT_SINK@",
                       QString::number(level) + "%"});
                finish("Ses %" + QString::number(level) + ".", 2200, "action");
            } else {
                finish("Anlayamadım.", 2200, "error");
            }
        }
    } else if (action == "system_power") {
        QString t = obj["target"].toString().trimmed().toLower();
        if (t == "shutdown") {
            spawn({"systemctl", "poweroff"});
            finish("Bilgisayar kapatılıyor…", 2500, "action");
        } else if (t == "reboot") {
            spawn({"systemctl", "reboot"});
            finish("Bilgisayar yeniden başlatılıyor…", 2500, "action");
        } else if (t == "sleep") {
            spawn({"systemctl", "suspend"});
            finish("Uyku moduna geçiliyor…", 2500, "action");
        } else {
            finish("Anlayamadım.", 2200, "error");
        }
    } else if (action == "open_url") {
        open_url(obj["target"].toString());
        finish("Açılıyor…", 2200, "action");
    } else if (action == "chat") {
        QString text = obj["reply"].toString().trimmed();
        if (text.isEmpty()) {
            finish("Anlayamadım.", 2200, "error");
        } else {
            int ms = qBound(3500, 2500 + 45 * int(text.size()), 12000);
            finish(text, ms, "chat");
        }
    } else if (!raw.isEmpty()) {
        int ms = qBound(3500, 2500 + 45 * int(raw.size()), 12000);
        finish(raw, ms, "chat");
    } else {
        finish("Anlayamadım.", 2200, "error");
    }
}

void Backend::finish(const QString &text, int displayMs, const QString &kind) {
    setState("responding");
    emit responseReady(text, displayMs, kind);
    QTimer::singleShot(displayMs, this, &Backend::close);
}

void Backend::close() {
    play_sound_if_exists("sounds/shutdown.mp3");
    setState("closing");
}

void Backend::quitNow() {
    QTimer::singleShot(500, qApp, &QCoreApplication::quit);
}

void Backend::scan_desktops() {
    m_apps.clear();
    QStringList dirs = xdg_application_dirs();
    std::unordered_set<QString> seen;
    for (const QString &dir : dirs) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            dir.toStdString(),
            std::filesystem::directory_options::skip_permission_denied, ec);
        for (const std::filesystem::directory_entry &entry : it) {
            std::filesystem::path path = entry.path();
            if (path.extension() != ".desktop")
                continue;
            QString id = QString::fromStdString(path.filename().string());
            if (seen.contains(id))
                continue;
            seen.insert(id);
            std::ifstream f(path);
            if (!f.is_open())
                continue;
            bool inEntry = false;
            std::unordered_map<QString, QString> kv;
            std::string sline;
            while (std::getline(f, sline)) {
                QString line = QString::fromStdString(sline).trimmed();
                if (line.startsWith('[')) {
                    if (inEntry)
                        break;
                    inEntry = (line == "[Desktop Entry]");
                    continue;
                }
                if (!inEntry || line.isEmpty() || line.startsWith('#'))
                    continue;
                int eq = line.indexOf('=');
                if (eq <= 0)
                    continue;
                kv.insert_or_assign(line.left(eq).trimmed(),
                                    line.mid(eq + 1).trimmed());
            }
            if (kv_get(kv, "Type") != "Application")
                continue;
            if (kv_get(kv, "NoDisplay") == "true" ||
                kv_get(kv, "Hidden") == "true")
                continue;
            QString exec = kv_get(kv, "Exec");
            if (exec.isEmpty())
                continue;
            DesktopApp app;
            app.exec = exec;
            app.name = kv.contains("Name[tr]") ? kv_get(kv, "Name[tr]")
                                               : kv_get(kv, "Name");
            for (const char *key :
                 {"Name", "Name[tr]", "Name[tr_TR]", "GenericName",
                  "GenericName[tr]", "GenericName[tr_TR]"}) {
                QString v = normalize(kv_get(kv, key));
                if (!v.isEmpty() && !app.matchers.contains(v))
                    app.matchers.append(v);
            }
            for (const char *key :
                 {"Keywords", "Keywords[tr]", "Keywords[tr_TR]"}) {
                for (const QString &kw :
                     kv_get(kv, key).split(';', Qt::SkipEmptyParts)) {
                    QString v = normalize(kw);
                    if (!v.isEmpty() && !app.matchers.contains(v))
                        app.matchers.append(v);
                }
            }
            QString stem = normalize(QString::fromStdString(
                std::filesystem::path(exec.section(' ', 0, 0).toStdString())
                    .stem()
                    .string()));
            if (!stem.isEmpty() && !app.matchers.contains(stem))
                app.matchers.append(stem);
            if (!app.name.isEmpty() && !app.matchers.isEmpty())
                m_apps.push_back(app);
        }
    }
    QStringList names;
    for (const DesktopApp &app : m_apps)
        names.append(app.name);
    m_applist = names.join(", ");
}

const DesktopApp *Backend::resolve_app(const QString &query) const {
    QString q = normalize(query);
    if (q.isEmpty())
        return nullptr;
    const DesktopApp *best = nullptr;
    int bestScore = 0;
    for (const DesktopApp &app : m_apps) {
        int s = 0;
        for (const QString &c : app.matchers)
            s = qMax(s, score_pair(q, c));
        if (s > bestScore ||
            (s == bestScore && best && app.name.size() < (*best).name.size())) {
            bestScore = s;
            best = &app;
        }
    }
    return bestScore >= 62 ? best : nullptr;
}

void Backend::enumerate_devices() {
    if (ma_context_get_devices(&(*this).m_context, nullptr, nullptr,
                               &(*this).m_captureinfos,
                               &(*this).m_capturecount) != MA_SUCCESS) {
        qDebug() << "Error during getting devices.";
    }
}
