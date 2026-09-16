#include <QApplication>

#include <memory>
#include <string>
#include <vector>

#include "ruby/gpu/GpuDevice.h"
#include "ruby/ui/DemoProject.h"
#include "ruby/ui/MainWindow.h"
#include "ruby/script/LuaHost.h"
#include "ruby/ui/Theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ruby"));
    QApplication::setOrganizationName(QStringLiteral("Ruby"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setPalette(ruby::ui::theme::palette());
    app.setStyleSheet(ruby::ui::theme::styleSheet());
    // UI transitions are capped at 80ms; tooltip fade blows past that.
    QApplication::setStyle(QApplication::style());
    qApp->setEffectEnabled(Qt::UI_AnimateTooltip, false);

    // .rbypr opens a project; everything else is demo media (matches Finder double-click args).
    std::string projectPath;
    std::vector<std::string> mediaPaths;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.size() > 6 && arg.compare(arg.size() - 6, 6, ".rbypr") == 0) {
            projectPath = arg;
        } else {
            mediaPaths.push_back(arg);
        }
    }
    ruby::ui::demo::setMediaPaths(std::move(mediaPaths));

    // Installed eagerly (not lazily) so its lifetime is well-defined; must outlive every render.
    // Null is legal — expressions just fall back to keyframed values.
    std::unique_ptr<ruby::script::LuaHost> expressions = ruby::script::LuaHost::create();
    ruby::script::ScopedHost installed(expressions.get());

    // Owned by the app; outlives every window that uses it.
    std::unique_ptr<ruby::gpu::GpuDevice> gpu = ruby::gpu::create_dawn_device();

    ruby::ui::MainWindow window(gpu.get());
    if (!projectPath.empty()) {
        window.openProject(QString::fromStdString(projectPath));
    }
    window.show();

    return QApplication::exec();
}
