#include "mapframeadapter.h"
#include "mapgeometry.h"
#include <cmath>
#include <stdexcept>

MapFrameAdapter::MapFrameAdapter(MapFrameBinding binding) : m_binding(std::move(binding)) {
    if (!m_binding.isUsable())
        throw std::invalid_argument("map_frame_unconfirmed");
}
QPointF MapFrameAdapter::toEnu(const QPointF &p) const {
    const auto q = MapGeometry::mapToStandardPoint(p);
    const double a = m_binding.enuYawOffsetRad;
    return {std::cos(a) * q.x() - std::sin(a) * q.y() + m_binding.enuTranslation.x(),
            std::sin(a) * q.x() + std::cos(a) * q.y() + m_binding.enuTranslation.y()};
}
QPointF MapFrameAdapter::fromEnu(const QPointF &p) const {
    const auto q = p - m_binding.enuTranslation;
    const double a = m_binding.enuYawOffsetRad;
    return MapGeometry::standardToMapPoint(
        {std::cos(a) * q.x() + std::sin(a) * q.y(), -std::sin(a) * q.x() + std::cos(a) * q.y()});
}
double MapFrameAdapter::yawToEnu(double yaw) const {
    return MapGeometry::normalizeAngle(MapGeometry::mapToStandardAngle(yaw) + m_binding.enuYawOffsetRad);
}
double MapFrameAdapter::yawFromEnu(double yaw) const {
    return MapGeometry::standardToMapAngle(yaw - m_binding.enuYawOffsetRad);
}
