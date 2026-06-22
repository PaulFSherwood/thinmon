#include <QApplication>
#include <map>
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

enum class DisplayMode {
    Carousel,
    Cpu,
    Gpu,
    Ram,
    Vram,
    NetDown,
    NetUp,
    SplitCpuGpuRam,
    PhysicalCpus
};


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

    std::vector<double> physicalCpuPercents()
    {
        std::ifstream f("/proc/stat");
        if (!f.is_open()) return {};

        std::vector<CpuTick> current;
        std::string line;

        while (std::getline(f, line)) {
            if (line.rfind("cpu", 0) != 0) break;
            if (line.size() < 4 || !std::isdigit(line[3])) continue;

            std::istringstream ss(line);

            std::string label;
            unsigned long long user = 0;
            unsigned long long nice = 0;
            unsigned long long system = 0;
            unsigned long long idle = 0;
            unsigned long long iowait = 0;
            unsigned long long irq = 0;
            unsigned long long softirq = 0;
            unsigned long long steal = 0;

            ss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

            CpuTick t;
            t.idle = idle + iowait;
            t.total = user + nice + system + idle + iowait + irq + softirq + steal;

            current.push_back(t);
        }

        if (current.empty()) return {};

        auto groups = physicalCoreGroups(static_cast<int>(current.size()));

        if (!haveLogicalCpu || lastLogicalCpu.size() != current.size()) {
            lastLogicalCpu = current;
            haveLogicalCpu = true;
            return std::vector<double>(groups.size(), 0.0);
        }

        std::vector<double> result;

        for (const auto& group : groups) {
            unsigned long long totalDelta = 0;
            unsigned long long idleDelta = 0;

            for (int cpuIndex : group) {
                if (cpuIndex < 0 || cpuIndex >= static_cast<int>(current.size())) continue;

                totalDelta += current[cpuIndex].total - lastLogicalCpu[cpuIndex].total;
                idleDelta  += current[cpuIndex].idle  - lastLogicalCpu[cpuIndex].idle;
            }

            double usage = 0.0;

            if (totalDelta > 0) {
                usage = 100.0 * static_cast<double>(totalDelta - idleDelta)
                    / static_cast<double>(totalDelta);
            }

            result.push_back(std::clamp(usage, 0.0, 100.0));
        }

        lastLogicalCpu = current;
        return result;
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
    struct CpuTick {
        unsigned long long idle = 0;
        unsigned long long total = 0;
    };

    bool haveLogicalCpu = false;
    std::vector<CpuTick> lastLogicalCpu;
    std::vector<std::vector<int>> physicalCoreGroupsCache;

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

    std::vector<std::vector<int>> physicalCoreGroups(int logicalCpuCount)
    {
        if (!physicalCoreGroupsCache.empty()) {
            return physicalCoreGroupsCache;
        }

        std::map<std::pair<int, int>, std::vector<int>> grouped;

        for (int i = 0; i < logicalCpuCount; ++i) {
            fs::path base = fs::path("/sys/devices/system/cpu") / ("cpu" + std::to_string(i)) / "topology";

            auto packageValue = readNumberFile(base / "physical_package_id");
            auto coreValue = readNumberFile(base / "core_id");

            if (!packageValue || !coreValue) {
                physicalCoreGroupsCache.push_back({i});
                continue;
            }

            int packageId = static_cast<int>(*packageValue);
            int coreId = static_cast<int>(*coreValue);

            grouped[{packageId, coreId}].push_back(i);
        }

        if (!grouped.empty()) {
            physicalCoreGroupsCache.clear();

            for (const auto& item : grouped) {
                physicalCoreGroupsCache.push_back(item.second);
            }
        }

        if (physicalCoreGroupsCache.empty()) {
            for (int i = 0; i < logicalCpuCount; ++i) {
                physicalCoreGroupsCache.push_back({i});
            }
        }

        return physicalCoreGroupsCache;
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
            if (displayMode == DisplayMode::Carousel) {
                currentGraph = (currentGraph + 1) % graphs.size();
                update();
            }
        });
        swapTimer->start(5000);

        sample();
        moveToConfiguredScreen();
    }

protected:
    void contextMenuEvent(QContextMenuEvent* event) override
    {
        QMenu menu(this);

        QAction* carouselAction = menu.addAction("Carousel / 5s swap");
        QAction* cpuAction      = menu.addAction("CPU");
        QAction* physicalCpuAction = menu.addAction("Physical CPUs");
        QAction* gpuAction      = menu.addAction("GPU");
        QAction* ramAction      = menu.addAction("RAM");
        QAction* vramAction     = menu.addAction("VRAM");
        QAction* downAction     = menu.addAction("Net Down");
        QAction* upAction       = menu.addAction("Net Up");

        menu.addSeparator();

        QAction* splitAction = menu.addAction("Split: CPU | GPU | RAM");

        menu.addSeparator();

        QAction* quitAction = menu.addAction("Quit ThinMon");

        QAction* selected = menu.exec(event->globalPos());

        if (selected == carouselAction) {
            displayMode = DisplayMode::Carousel;
        } else if (selected == cpuAction) {
            displayMode = DisplayMode::Cpu;
            currentGraph = 0;
        } else if (selected == physicalCpuAction) {
            displayMode = DisplayMode::PhysicalCpus;
        } else if (selected == gpuAction) {
            displayMode = DisplayMode::Gpu;
            currentGraph = 1;
        } else if (selected == ramAction) {
            displayMode = DisplayMode::Ram;
            currentGraph = 2;
        } else if (selected == vramAction) {
            displayMode = DisplayMode::Vram;
            currentGraph = 3;
        } else if (selected == downAction) {
            displayMode = DisplayMode::NetDown;
            currentGraph = 4;
        } else if (selected == upAction) {
            displayMode = DisplayMode::NetUp;
            currentGraph = 5;
        } else if (selected == splitAction) {
            displayMode = DisplayMode::SplitCpuGpuRam;
        } else if (selected == quitAction) {
            QApplication::quit();
            return;
        }

        update();
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

        if (displayMode == DisplayMode::PhysicalCpus) {
            drawPhysicalCpuPanel(p, rect());
            return;
        }

        if (displayMode == DisplayMode::SplitCpuGpuRam) {
            int third = width() / 3;

            QRect cpuRect(0, 0, third, height());
            QRect gpuRect(third, 0, third, height());
            QRect ramRect(third * 2, 0, width() - third * 2, height());

            p.setPen(QColor(60, 60, 60));
            p.drawLine(gpuRect.left(), 0, gpuRect.left(), height());
            p.drawLine(ramRect.left(), 0, ramRect.left(), height());

            drawGraphPanel(p, graphs[0], cpuRect, "split");
            drawGraphPanel(p, graphs[1], gpuRect, "split");
            drawGraphPanel(p, graphs[2], ramRect, "split");
            return;
        }

        int graphIndex = currentGraph;
        QString modeText = "5s swap";

        if (displayMode != DisplayMode::Carousel) {
            modeText = "locked";

            switch (displayMode) {
                case DisplayMode::Cpu:     graphIndex = 0; break;
                case DisplayMode::Gpu:     graphIndex = 1; break;
                case DisplayMode::Ram:     graphIndex = 2; break;
                case DisplayMode::Vram:    graphIndex = 3; break;
                case DisplayMode::NetDown: graphIndex = 4; break;
                case DisplayMode::NetUp:   graphIndex = 5; break;
                default: break;
            }
        }

        drawGraphPanel(p, graphs[graphIndex], rect(), modeText);
    }

private:
    SysStats stats;
    QVector<Graph> graphs;
    QVector<Graph> physicalCpuGraphs;

    QTimer* sampleTimer = nullptr;
    QTimer* swapTimer = nullptr;

    int currentGraph = 0;
    DisplayMode displayMode = DisplayMode::Carousel;

    void sample()
    {
        appendSample(graphs[0], stats.cpuPercent());
        appendSample(graphs[1], stats.gpuPercent());
        appendSample(graphs[2], stats.memPercent());
        appendSample(graphs[3], stats.vramPercent());

        auto physicalCpus = stats.physicalCpuPercents();

        if (physicalCpuGraphs.size() != static_cast<int>(physicalCpus.size())) {
            physicalCpuGraphs.clear();

            for (int i = 0; i < static_cast<int>(physicalCpus.size()); ++i) {
                physicalCpuGraphs.push_back(Graph{
                    "CPU " + QString::number(i),
                    Unit::Percent,
                    false,
                    0.0,
                    {}
                });
            }
        }

        for (int i = 0; i < static_cast<int>(physicalCpus.size()); ++i) {
            appendSample(physicalCpuGraphs[i], physicalCpus[i]);
        }

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
    void drawGraphPanel(QPainter& p, const Graph& g, const QRect& area, const QString& modeText)
    {
        int labelW = 125;
        int plotX = area.left() + labelW;
        int plotY = area.top() + 5;
        int plotW = area.width() - labelW - 8;
        int plotH = area.height() - 10;

        if (plotW <= 10 || plotH <= 10) return;

        p.setPen(QColor(230, 230, 230));
        p.drawText(area.left() + 8, area.top() + 18, g.name);

        p.setPen(valueColor(normalizedValue(g.current, scaleFor(g))));
        p.drawText(area.left() + 8, area.top() + 38, formatValue(g.current, g.unit));

        p.setPen(QColor(95, 95, 95));
        p.drawText(area.left() + 8, area.top() + 56, modeText);

        drawGrid(p, plotX, plotY, plotW, plotH);
        drawDotGraph(p, g, plotX, plotY, plotW, plotH);

        p.setPen(QColor(120, 120, 120));
        QString maxText = "max " + formatValue(scaleFor(g), g.unit);
        p.drawText(area.right() - 115, area.top() + 18, maxText);
    }
    void drawPhysicalCpuPanel(QPainter& p, const QRect& area)
    {
        if (physicalCpuGraphs.empty()) return;

        int cores = physicalCpuGraphs.size();
        int panelW = std::max(1, area.width() / cores);

        for (int i = 0; i < cores; ++i) {
            int left = area.left() + i * panelW;
            int right = (i == cores - 1) ? area.right() : left + panelW - 1;

            QRect panel(left, area.top(), right - left + 1, area.height());

            if (i > 0) {
                p.setPen(QColor(60, 60, 60));
                p.drawLine(panel.left(), panel.top(), panel.left(), panel.bottom());
            }

            const Graph& g = physicalCpuGraphs[i];

            p.setPen(QColor(230, 230, 230));
            p.drawText(panel.left() + 6, panel.top() + 14, g.name);

            p.setPen(valueColor(normalizedValue(g.current, 100.0)));
            p.drawText(panel.left() + 6, panel.top() + 30, formatValue(g.current, g.unit));

            int plotX = panel.left() + 6;
            int plotY = panel.top() + 34;
            int plotW = panel.width() - 12;
            int plotH = panel.height() - 38;

            if (plotW <= 10 || plotH <= 8) continue;

            drawGrid(p, plotX, plotY, plotW, plotH);
            drawDotGraph(p, g, plotX, plotY, plotW, plotH);
        }
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
