#pragma once

#include <QList>
#include <QString>
#include <QUrl>

namespace CameraPreviewSelection {
struct Stream {
    QString kind;
    QString label;
    QUrl url;
    bool available = false;
    QString reason;
};
inline bool isHttp(const QUrl &url) {
    return url.isValid() && !url.host().isEmpty() &&
        (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
}
inline bool isDepth(const QUrl &url) {
    return url.path() == QStringLiteral("/camera/depth/stream.mjpeg");
}
inline QUrl endpoint(QUrl base, const QString &path) {
    if (!isHttp(base)) return {};
    base.setPath(path);
    base.setQuery(QString());
    base.setFragment(QString());
    return base;
}
// These URLs select preview only. Photo/recording remain commands to the RGB
// camera service and never inherit the selected preview URL.
inline QList<Stream> streams(const QString &backend, const QString &configuredRgb,
    const QString &controlBase) {
    if (backend == QStringLiteral("oak_depthai")) {
        return {{"rgb", QStringLiteral("彩色画面"), {}, true, {}},
            {"depth", QStringLiteral("深度画面"), {}, false, QStringLiteral("本机 OAK 后端暂不提供深度预览，请使用工控机相机服务。")}};
    }
    QUrl base(controlBase.trimmed());
    QUrl rgb, depth;
    if (isHttp(base)) {
        // The configured camera service is the sole endpoint authority. Never
        // show RGB from one device while saving or reading depth from another.
        rgb = endpoint(base, QStringLiteral("/camera/stream.mjpeg"));
        depth = endpoint(base, QStringLiteral("/camera/depth/stream.mjpeg"));
    } else {
        rgb = QUrl(configuredRgb.trimmed());
        if (!isHttp(rgb)) rgb = QUrl();
        base = rgb;
        if (isDepth(rgb)) rgb = endpoint(base, QStringLiteral("/camera/stream.mjpeg"));
        depth = endpoint(base, QStringLiteral("/camera/depth/stream.mjpeg"));
    }
    return {{"rgb", QStringLiteral("彩色画面"), rgb, isHttp(rgb),
                isHttp(rgb) ? QString() : QStringLiteral("请先配置工控机相机地址。")},
        {"depth", QStringLiteral("深度画面"), depth, isHttp(depth),
                isHttp(depth) ? QString() : QStringLiteral("尚未配置工控机深度流地址。")}};
}
} // namespace CameraPreviewSelection
