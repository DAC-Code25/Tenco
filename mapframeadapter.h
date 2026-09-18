#pragma once
#include "trackingtypes.h"

class MapFrameAdapter {
  public:
    explicit MapFrameAdapter(MapFrameBinding binding);
    QPointF toEnu(const QPointF &mapPoint) const;
    QPointF fromEnu(const QPointF &enuPoint) const;
    double yawToEnu(double mapYaw) const;
    double yawFromEnu(double enuYaw) const;
    const MapFrameBinding &binding() const { return m_binding; }

  private:
    MapFrameBinding m_binding;
};
