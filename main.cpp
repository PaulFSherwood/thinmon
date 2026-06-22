#include <QApplication>
#include <QMenu>
#include <QContextMenuEvent>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QString>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

enum class Unit {
    Percent,
    KiBPerSec
};

struct Graph {
    QString name;
    Unit unit;
    bool dynamicScale;
    double current;
    std::deque<double> samples;
};

static std::optional<double> readNumberFile(const fs::path& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;

    double v = 0.0;
    f >> v;

    if (!f.good() && !f.eof()) return std::nullopt;
    return v;
}

class SysStats {
public:
    SysStats()
    {
        locateGpu();
        lastNetTime = std::chrono::steady_clock::now();
    }

    double cpuPercent()
    {
        std::ifstream f("/proc/stat");
        if (!f.is_open()) return 0.0;

        std::string cpu;
        unsigned long long user = 0;
        unsigned long long nice = 0;
        unsigned long long system = 0;
        unsigned long long idle = 0;
        unsigned long long iowait = 0;
        unsigned long long irq = 0;
        unsigned long long softirq = 0;
        unsigned long long steal = 0;

        f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        unsigned long long idleAll = idle + iowait;
        unsigned long long nonIdle = user + nice + system + irq + softirq + steal;
        unsigned long long total = idleAll + nonIdle;

        if (!haveCpu) {
            lastCpuTotal = total;
            lastCpuIdle = idleAll;
            haveCpu = true;
            return 0.0;
        }

        unsigned long long totalDelta = total - lastCpuTotal;
        unsigned long long idleDelta = idleAll - lastCpuIdle;

        lastCpuTotal = total;
        lastCpuIdle = idleAll;

        if (totalDelta == 0) return 0.0;

        return 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
    }

    double memPercent()
    {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return 0.0;

        std::string key;
        unsigned long long value = 0;
        std::string unit;

        unsigned long long total = 0;
        unsigned long long available = 0;

        while (f >> key >> value >> unit) {
            if (key == "MemTotal:") total = value;
            if (key == "MemAvailable:") available = value;
        }

        if (total == 0) return 0.0;

        double used = static_cast<double>(total - available);
        return 100.0 * used / static_cast<double>(total);
    }

    double gpuPercent()
    {
        if (!gpuBusyPath) return 0.0;

        auto v = readNumberFile(*gpuBusyPath);
        if (!v) return 0.0;

        return std::clamp(*v, 0.0, 100.0);
    }

    double vramPercent()
    {
        if (!vramUsedPath || !vramTotalPath) return 0.0;

        auto used = readNumberFile(*vramUsedPath);
        auto total = readNumberFile(*vramTotalPath);

        if (!used || !total || *total <= 0.0) return 0.0;

        return std::clamp(100.0 * (*used / *total), 0.0, 100.0);
    }

    std::pair<double, double> netRatesKiB()
    {
        unsigned long long rxTotal = 0;
        unsigned long long txTotal = 0;

        std::ifstream f("/proc/net/dev");
        if (!f.is_open()) return {0.0, 0.0};

        std::string line;

        std::getline(f, line);
        std::getline(f, line);

        while (std::getline(f, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string iface = line.substr(0, colon);
            iface.erase(std::remove_if(iface.begin(), iface.end(), ::isspace), iface.end());

            if (iface == "lo") continue;

            std::string rest = line.substr(colon + 1);
            std::istringstream iss(rest);

            unsigned long long rxBytes = 0;
            unsigned long long dummy = 0;
            unsigned long long txBytes = 0;

            iss >> rxBytes;

            for (int i = 0; i < 7; ++i) {
                iss >> dummy;
            }

            iss >> txBytes;

            rxTotal += rxBytes;
            txTotal += txBytes;
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastNetTime).count();

        if (!haveNet || dt <= 0.0) {
            lastRx = rxTotal;
            lastTx = txTotal;
            lastNetTime = now;
            haveNet = true;
            return {0.0, 0.0};
        }

        double downKiB = static_cast<double>(rxTotal - lastRx) / dt / 1024.0;
        double upKiB = static_cast<double>(txTotal - lastTx) / dt / 1024.0;

        lastRx = rxTotal;
        lastTx = txTotal;
        lastNetTime = now;

        if (downKiB < 0.0) downKiB = 0.0;
        if (upKiB < 0.0) upKiB = 0.0;

        return {downKiB, upKiB};
    }

private:
    bool haveCpu = false;
    unsigned long long lastCpuTotal = 0;
    unsigned long long lastCpuIdle = 0;

    bool haveNet = false;
    unsigned long long lastRx = 0;
    unsigned long long lastTx = 0;
    std::chrono::steady_clock::time_point lastNetTime;

    std::optional<fs::path> gpuBusyPath;
    std::optional<fs::path> vramUsedPath;
    std::optional<fs::path> vramTotalPath;

    void locateGpu()
    {
        std::error_code ec;

        fs::path drmRoot("/sys/class/drm");
        if (!fs::exists(drmRoot, ec)) return;

        for (const auto& entry : fs::directory_iterator(drmRoot, ec)) {
            if (ec) return;

            std::string name = entry.path().filename().string();

            if (name.rfind("card", 0) != 0) continue;
            if (name.find('-') != std::string::npos) continue;

            fs::path dev = entry.path() / "device";

            fs::path busy = dev / "gpu_busy_percent";
            fs::path vramUsed = dev / "mem_info_vram_used";
            fs::path vramTotal = dev / "mem_info_vram_total";

            if (fs::exists(busy, ec)) {
                gpuBusyPath = busy;
            }

            if (fs::exists(vramUsed, ec) && fs::exists(vramTotal, ec)) {
                vramUsedPath = vramUsed;
                vramTotalPath = vramTotal;
            }

            if (gpuBusyPath || vramUsedPath) {
                return;
            }
        }
    }
};

class ThinMon : public QWidget {
public:
    ThinMon()
    {
        graphs.push_back(Graph{"CPU",      Unit::Percent,   false, 0.0, {}});
        graphs.push_back(Graph{"GPU",      Unit::Percent,   false, 0.0, {}});
        graphs.push_back(Graph{"RAM",      Unit::Percent,   false, 0.0, {}});
        graphs.push_back(Graph{"VRAM",     Unit::Percent,   false, 0.0, {}});
        graphs.push_back(Graph{"NET DOWN", Unit::KiBPerSec, true,  0.0, {}});
        graphs.push_back(Graph{"NET UP",   Unit::KiBPerSec, true,  0.0, {}});

        setWindowFlags(
            Qt::FramelessWindowHint |
            Qt::WindowStaysOnTopHint |
            Qt::Tool |
            Qt::WindowDoesNotAcceptFocus
        );

        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowOpacity(0.94);

        QFont font("Monospace");
        font.setStyleHint(QFont::TypeWriter);
        font.setPixelSize(12);
        setFont(font);

        sampleTimer = new QTimer(this);
        connect(sampleTimer, &QTimer::timeout, this, [this]() {
            sample();
        });
        sampleTimer->start(500);

        swapTimer = new QTimer(this);
        connect(swapTimer, &QTimer::timeout, this, [this]() {
            currentGraph = (currentGraph + 1) % graphs.size();
            update();
        });
        swapTimer->start(5000);

        sample();
        moveToConfiguredScreen();
    }

protected:
    void contextMenuEvent(QContextMenuEvent* event) override
    {
        QMenu menu(this);
        QAction* quitAction = menu.addAction("Quit ThinMon");
    
        QAction* selected = menu.exec(event->globalPos());
    
        if (selected == quitAction) {
            QApplication::quit();
        }
    }

    void showEvent(QShowEvent*) override
    {
        moveToConfiguredScreen();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        p.fillRect(rect(), QColor(0, 0, 0, 235));

        p.setPen(QColor(80, 80, 80));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        if (graphs.empty()) return;

        const Graph& g = graphs[currentGraph];

        int labelW = 125;
        int plotX = labelW;
        int plotY = 5;
        int plotW = width() - plotX - 8;
        int plotH = height() - 10;

        if (plotW <= 10 || plotH <= 10) return;

        p.setPen(QColor(230, 230, 230));
        p.drawText(8, 18, g.name);

        p.setPen(valueColor(normalizedValue(g.current, scaleFor(g))));
        p.drawText(8, 38, formatValue(g.current, g.unit));

        p.setPen(QColor(95, 95, 95));
        p.drawText(8, 56, "5s swap");

        drawGrid(p, plotX, plotY, plotW, plotH);
        drawDotGraph(p, g, plotX, plotY, plotW, plotH);

        p.setPen(QColor(120, 120, 120));
        QString maxText = "max " + formatValue(scaleFor(g), g.unit);
        p.drawText(width() - 115, 18, maxText);
    }

private:
    SysStats stats;
    QVector<Graph> graphs;

    QTimer* sampleTimer = nullptr;
    QTimer* swapTimer = nullptr;

    int currentGraph = 0;

    void sample()
    {
        appendSample(graphs[0], stats.cpuPercent());
        appendSample(graphs[1], stats.gpuPercent());
        appendSample(graphs[2], stats.memPercent());
        appendSample(graphs[3], stats.vramPercent());

        auto [downKiB, upKiB] = stats.netRatesKiB();

        appendSample(graphs[4], downKiB);
        appendSample(graphs[5], upKiB);

        update();
    }

    void appendSample(Graph& g, double value)
    {
        if (std::isnan(value) || std::isinf(value)) value = 0.0;
        if (value < 0.0) value = 0.0;

        g.current = value;
        g.samples.push_back(value);

        constexpr size_t maxSamples = 1200;
        while (g.samples.size() > maxSamples) {
            g.samples.pop_front();
        }
    }

    void moveToConfiguredScreen()
    {
        QList<QScreen*> screens = QGuiApplication::screens();
        if (screens.isEmpty()) return;

        bool okScreen = false;
        int screenIndex = qEnvironmentVariableIntValue("THINMON_SCREEN", &okScreen);
        if (!okScreen) screenIndex = 1;

        if (screenIndex < 0 || screenIndex >= screens.size()) {
            screenIndex = 0;
        }

        bool okHeight = false;
        int h = qEnvironmentVariableIntValue("THINMON_HEIGHT", &okHeight);
        if (!okHeight) h = 60;

        h = std::clamp(h, 30, 120);

        QRect g = screens[screenIndex]->geometry();

        setGeometry(g.x(), g.y() + g.height() - h, g.width(), h);
    }

    double scaleFor(const Graph& g) const
    {
        if (!g.dynamicScale) return 100.0;

        double maxValue = 1.0;

        for (double v : g.samples) {
            maxValue = std::max(maxValue, v);
        }

        double scale = 64.0;
        while (scale < maxValue * 1.20) {
            scale *= 2.0;
        }

        return scale;
    }

    double normalizedValue(double value, double scale) const
    {
        if (scale <= 0.0) return 0.0;
        return std::clamp(value / scale, 0.0, 1.0);
    }

    QColor valueColor(double n) const
    {
        n = std::clamp(n, 0.0, 1.0);
    
        if (n < 0.5) {
            double t = n / 0.5;
    
            int r = static_cast<int>(80  + (230 - 80)  * t);
            int g = static_cast<int>(230 + (210 - 230) * t);
            int b = static_cast<int>(130 + (80  - 130) * t);
    
            return QColor(r, g, b);
        } else {
            double t = (n - 0.5) / 0.5;
    
            int r = static_cast<int>(230 + (240 - 230) * t);
            int g = static_cast<int>(210 + (80  - 210) * t);
            int b = static_cast<int>(80  + (80  - 80)  * t);
    
            return QColor(r, g, b);
        }
    }

    QString formatValue(double value, Unit unit) const
    {
        if (unit == Unit::Percent) {
            return QString::number(value, 'f', 0) + "%";
        }

        if (value >= 1024.0) {
            return QString::number(value / 1024.0, 'f', 2) + " MiB/s";
        }

        return QString::number(value, 'f', 0) + " KiB/s";
    }

    void drawGrid(QPainter& p, int x, int y, int w, int h)
    {
        p.setPen(QColor(35, 35, 35));

        for (int i = 1; i < 4; ++i) {
            int gy = y + (h * i) / 4;
            p.drawLine(x, gy, x + w, gy);
        }
    }

    void drawDotGraph(QPainter& p, const Graph& g, int x, int y, int w, int h)
    {
        constexpr int cellW = 5;
        constexpr int cellH = 4;
        constexpr int dotW = 2;
        constexpr int dotH = 2;

        int rows = std::max(1, h / cellH);
        int cols = std::max(1, w / cellW);

        int sampleCount = static_cast<int>(g.samples.size());
        int start = std::max(0, sampleCount - cols);

        double scale = scaleFor(g);

        int drawCol = 0;

        for (int i = start; i < sampleCount; ++i) {
            double value = g.samples[static_cast<size_t>(i)];
            double n = normalizedValue(value, scale);

            int dotsHigh = static_cast<int>(std::ceil(n * rows));
            if (value > 0.0 && dotsHigh < 1) dotsHigh = 1;

            int px = x + drawCol * cellW;
            
            for (int r = 0; r < dotsHigh; ++r) {
                double heightN = 0.0;
            
                if (rows > 1) {
                    heightN = static_cast<double>(r) / static_cast<double>(rows - 1);
                }
            
                QColor c = valueColor(heightN);
            
                int py = y + h - ((r + 1) * cellH);
            
                p.fillRect(px + 1, py + 1, dotW, dotH, c);
            }
            
            ++drawCol;
        }
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    ThinMon w;
    w.show();

    return app.exec();
}
