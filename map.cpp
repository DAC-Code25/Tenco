#include <functional>
#include "map.h"

#include "mapgraphicsview.h"
#include "mapdocument.h"
#include "mapgeometry.h"
#include "maprouteplanner.h"
#include "controlsessioncoordinator.h"
#include "taskcompiler.h"
#include <QDialog>
#include <QDialogButtonBox>
#include "ui_mainwindow.h"
#include "configmanager.h"
#include "loggingmanager.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPalette>
#include <QCursor>
#include <QPen>
#include <QPushButton>
#include <QUrl>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QMenu>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVariant>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector2D>
#include <QSlider>
#include <QSizePolicy>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QtMath>

namespace {
Q_LOGGING_CATEGORY(lcMapModule, "tenco.map")

constexpr int kDefaultGridWidth = 50;
constexpr int kDefaultGridHeight = 50;
constexpr int kMaxGridSize = 500;
constexpr double kCellSizeMeters = 1.0;
constexpr double kPointMarkerRadiusPx = 4.0;
constexpr double kPointArrowLengthPx = 14.0;
constexpr double kPointArrowWidthPx = 6.0;
constexpr double kVehicleLengthMeters = 0.9;
constexpr double kVehicleWidthMeters = 0.45;
constexpr double kVehicleSnapThresholdMeters = 0.35;
constexpr double kRouteReplanSnapThresholdMeters = 1.0;
constexpr double kPointSelectionThresholdMeters = 0.5;
constexpr int kDefaultRouteLoopIntervalSeconds = 3;
constexpr double kArcSagittaEpsilon = 1e-3;
constexpr double kGeometryEpsilon = 1e-6;
constexpr int kMinArcSegments = 24;
constexpr int kVehiclePoseRefreshMinIntervalMs = 120;
constexpr double kVehiclePoseMinDistanceDeltaMeters = 0.02;
constexpr double kVehiclePoseMinAngleDeltaRad = 1.5 * M_PI / 180.0;
constexpr int kMapSchemaVersion = 2;
constexpr int kRowWorkCheckpointTableNameColumn = 0;
constexpr int kRowWorkCheckpointTableProgressColumn = 1;
constexpr int kRowWorkCheckpointTableDwellColumn = 2;
constexpr int kRowWorkCheckpointTableForwardColumn = 3;
constexpr int kRowWorkCheckpointTableBackwardColumn = 4;
constexpr int kRowWorkCheckpointTableCaptureColumn = 5;
constexpr int kRowWorkCheckpointTableTimeoutColumn = 6;
constexpr int kRowMissionStepEnabledColumn = 0;
constexpr int kRowMissionStepNameColumn = 1;
constexpr int kRowMissionStepTypeColumn = 2;
constexpr int kRowMissionStepSummaryColumn = 3;

using MapGeometry::formatNumber;
using MapGeometry::mapToStandardAngle;
using MapGeometry::mapToStandardPoint;
using MapGeometry::normalizeAngle;
using MapGeometry::pointsAlmostEqual;
using MapGeometry::polylineLength;
using MapGeometry::standardToMapAngle;
using MapGeometry::standardToMapPoint;


QPainterPath makePointArrowPath()
{
    QPainterPath arrowPath;
    const double tipX = kPointArrowLengthPx;
    const double stemLength = kPointArrowLengthPx * 0.28;
    const double baseHalfWidth = kPointArrowWidthPx * 0.5;
    const double neckHalfWidth = baseHalfWidth * 0.35;
    const double neckStartX = kPointMarkerRadiusPx;
    arrowPath.moveTo(tipX, 0.0);
    arrowPath.lineTo(tipX - stemLength, baseHalfWidth);
    arrowPath.lineTo(tipX - stemLength, neckHalfWidth);
    arrowPath.lineTo(neckStartX, neckHalfWidth);
    arrowPath.lineTo(neckStartX, -neckHalfWidth);
    arrowPath.lineTo(tipX - stemLength, -neckHalfWidth);
    arrowPath.lineTo(tipX - stemLength, -baseHalfWidth);
    arrowPath.closeSubpath();
    return arrowPath;
}

QPainterPath makeDirectionArrowPath()
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    path.lineTo(26.0, 0.0);
    path.moveTo(26.0, 0.0);
    path.lineTo(18.0, -5.0);
    path.moveTo(26.0, 0.0);
    path.lineTo(18.0, 5.0);
    return path;
}

} // namespace

QString Map::s_lastMapFilePath;

Map::Map(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
{
    m_cellSizeMeters = kCellSizeMeters;
    m_cellSizePixels = 50.0;
    m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    m_rowWorkPlan.frameId = QStringLiteral("map");
    m_rowWorkPlan.params.loopEnabled = true;
    m_rowMissionPlan.missionId = RowMissionJson::generateMissionId();
    m_rowMissionPlan.frameId = QStringLiteral("map");

    initializeUi();
    ensureScene();
    ensureVehicleItem();
    connect(&ConfigManager::instance(), &ConfigManager::configChanged, this, &Map::applyRuntimeConfig);
    m_vehiclePoseRefreshClock.start();
    handleModuleActivated();
}

void Map::applyRuntimeConfig()
{
    if (m_tracking) m_tracking->invalidatePreparedPlan();
    const auto &geo = ConfigManager::instance().geo();
    m_baseLatitudeDeg = geo.baseLatitudeDeg;
    m_baseLongitudeDeg = geo.baseLongitudeDeg;
    refreshRowWorkControlState();
    setRowWorkStatusText(tr("地图运行配置已应用"));
}

void Map::handleModuleActivated()
{
    if (!m_initialized) {
        const ConfigManager &config = ConfigManager::instance();
        const auto geo = config.geo();
        m_baseLatitudeDeg = geo.baseLatitudeDeg;
        m_baseLongitudeDeg = geo.baseLongitudeDeg;

        clearMapData();

        m_gridWidth = kDefaultGridWidth;
        m_gridHeight = kDefaultGridHeight;
        setMapRotation(0.0);

        if (m_gridWidthSpin) {
            QSignalBlocker blocker(m_gridWidthSpin);
            m_gridWidthSpin->setValue(m_gridWidth);
        }
        if (m_gridHeightSpin) {
            QSignalBlocker blocker(m_gridHeightSpin);
            m_gridHeightSpin->setValue(m_gridHeight);
        }

        m_hasVehiclePose = false;
        m_hasRenderedVehiclePose = false;
        applyPresentationUpdates(true);

        m_currentMapFilePath.clear();
        updateMapNameDisplay();
        updateVehiclePointBinding();
        syncCommittedMapState();

        m_initialized = true;
        setRouteStatusText(tr("Grid reset to %1x%2 (rotation %3°, base lat %4, lon %5)")
                               .arg(m_gridWidth)
                               .arg(m_gridHeight)
                               .arg(m_mapRotationDeg, 0, 'f', 1)
                               .arg(m_baseLatitudeDeg, 0, 'f', 6)
                               .arg(m_baseLongitudeDeg, 0, 'f', 6));
    }

}

void Map::updateVehiclePose(double x, double y, double theta)
{
    const double normalizedTheta = normalizeAngle(theta);
    const bool hasPreviousRender = m_hasRenderedVehiclePose;
    const double deltaDistance = std::hypot(x - m_lastRenderedVehiclePoseX, y - m_lastRenderedVehiclePoseY);
    const double deltaTheta = std::abs(normalizeAngle(normalizedTheta - m_lastRenderedVehiclePoseTheta));
    const bool reachedRefreshInterval =
        !m_vehiclePoseRefreshClock.isValid() || m_vehiclePoseRefreshClock.elapsed() >= kVehiclePoseRefreshMinIntervalMs;
    const bool shouldRefreshNow = !hasPreviousRender
                                  || deltaDistance >= kVehiclePoseMinDistanceDeltaMeters
                                  || deltaTheta >= kVehiclePoseMinAngleDeltaRad
                                  || reachedRefreshInterval;

    m_vehiclePoseX = x;
    m_vehiclePoseY = y;
    m_vehiclePoseTheta = normalizedTheta;
    m_hasVehiclePose = true;
    if (shouldRefreshNow) {
        refreshVehicleGraphics();
        updateVehiclePointBinding();
        m_lastRenderedVehiclePoseX = m_vehiclePoseX;
        m_lastRenderedVehiclePoseY = m_vehiclePoseY;
        m_lastRenderedVehiclePoseTheta = m_vehiclePoseTheta;
        m_hasRenderedVehiclePose = true;
        if (m_vehiclePoseRefreshClock.isValid()) {
            m_vehiclePoseRefreshClock.restart();
        } else {
            m_vehiclePoseRefreshClock.start();
        }
    }
}


void Map::handleTogglePathsVisibility()
{
    m_pathsVisible = !m_pathsVisible;
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
        if (it.value().pathItem) {
            it.value().pathItem->setVisible(m_pathsVisible);
        }
    }
    setRouteStatusText(m_pathsVisible ? tr("路径线路已显示") : tr("路径线路已隐藏"));
}


bool Map::eventFilter(QObject *watched, QEvent *event)
{
    if (m_view && watched == m_view->viewport()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            m_draggedPointMoved = false;
            m_draggedPointRotated = false;
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_editModeEnabled) {
                const QPointF scenePos = m_view->mapToScene(mouseEvent->pos());
                const QPointF mapPos = sceneToMap(scenePos);
                double dist = 0.0;
                const Qt::KeyboardModifiers mods = mouseEvent->modifiers();

                if ((mods & (Qt::AltModifier | Qt::ControlModifier)) == (Qt::AltModifier | Qt::ControlModifier)) {
                    const int pointId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &dist);
                    if (pointId != -1) {
                        if (!m_ctrlAltPathActive || (m_ctrlAltStartPointId && m_ctrlAltStartPointId.value() == pointId)) {
                            m_ctrlAltPathActive = true;
                            m_ctrlAltStartPointId = pointId;
                            setRouteStatusText(tr("已选择起点 %1，请点击终点").arg(pointId));
                        } else {
                            const int startId = m_ctrlAltStartPointId.value();
                            const int endId = pointId;
                            const PathType type = static_cast<PathType>(m_pathTypeCombo ? m_pathTypeCombo->currentIndex() : 0);
                            const double sagitta = m_arcSagittaSpin ? m_arcSagittaSpin->value() : 0.0;
                            const int newId = addPathOrWarn(startId, endId, type, sagitta);
                            if (newId != -1) {
                                if (m_pathTable) {
                                    const QList<QTableWidgetItem *> matches =
                                        m_pathTable->findItems(QString::number(newId), Qt::MatchExactly);
                                    if (!matches.isEmpty()) {
                                        const int row = matches.first()->row();
                                        m_pathTable->setCurrentCell(row, 0,
                                                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
                                    }
                                }
                                setRouteStatusText(tr("已自动创建路径: 点%1 → 点%2").arg(startId).arg(endId));
                            }
                            m_ctrlAltPathActive = false;
                            m_ctrlAltStartPointId.reset();
                        }
                        return true;
                    }
                } else if ((mods & (Qt::AltModifier | Qt::ControlModifier)) == Qt::AltModifier) {
                    const int pointId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &dist);
                    if (pointId != -1) {
                        m_altRotationActive = true;
                        m_ctrlDragActive = false;
                        m_draggedPointId = pointId;
                        if (const MapPoint *point = pointById(pointId)) {
                            m_rotationStartTheta = point->theta;
                            const double dx = mapPos.x() - point->mapPosition.x();
                            const double dy = mapPos.y() - point->mapPosition.y();
                            m_rotationReferenceAngle = angleFromMapVector(dx, dy);
                        } else {
                            m_rotationReferenceAngle = 0.0;
                        }
                        m_pendingPointSelection = pointId;
                        if (m_pointTable) {
                            const QList<QTableWidgetItem *> matches =
                                m_pointTable->findItems(QString::number(pointId), Qt::MatchExactly);
                            if (!matches.isEmpty()) {
                                const int row = matches.first()->row();
                                m_pointTable->setCurrentCell(row, 0,
                                                             QItemSelectionModel::Select | QItemSelectionModel::Rows);
                            }
                        }
                        return true;
                    }
                } else if ((mods & (Qt::AltModifier | Qt::ControlModifier)) == Qt::ControlModifier) {
                    const int pointId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &dist);
                    if (pointId != -1) {
                        m_ctrlDragActive = true;
                        m_altRotationActive = false;
                        m_draggedPointId = pointId;
                        if (const MapPoint *point = pointById(pointId)) {
                            m_ctrlDragOffset = point->mapPosition - mapPos;
                            if (m_pointXSpin) {
                                QSignalBlocker blocker(m_pointXSpin);
                                m_pointXSpin->setValue(point->mapPosition.x());
                            }
                            if (m_pointYSpin) {
                                QSignalBlocker blocker(m_pointYSpin);
                                m_pointYSpin->setValue(point->mapPosition.y());
                            }
                            if (m_pointThetaSpin) {
                                QSignalBlocker blocker(m_pointThetaSpin);
                                m_pointThetaSpin->setValue(point->theta);
                            }
                            updatePointTableRow(pointId);
                        } else {
                            m_ctrlDragOffset = QPointF();
                        }
                        m_pendingPointSelection = pointId;
                        if (m_pointTable) {
                            const QList<QTableWidgetItem *> matches =
                                m_pointTable->findItems(QString::number(pointId), Qt::MatchExactly);
                            if (!matches.isEmpty()) {
                                const int row = matches.first()->row();
                                m_pointTable->setCurrentCell(row, 0,
                                                             QItemSelectionModel::Select | QItemSelectionModel::Rows);
                            }
                        }
                        return true;
                    }
                }
            }
            m_altRotationActive = false;
            m_ctrlDragActive = false;
            if (m_ctrlAltPathActive) {
                m_ctrlAltPathActive = false;
                m_ctrlAltStartPointId.reset();
            }
            m_draggedPointId = -1;
            break;
        }
case QEvent::MouseMove: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPointF scenePos = m_view->mapToScene(mouseEvent->pos());
            const QPointF mapPos = sceneToMap(scenePos);
            if (m_ctrlDragActive && m_draggedPointId != -1) {
                if (!(mouseEvent->buttons() & Qt::LeftButton)) {
                    return true;
                }
                MapPoint *point = pointById(m_draggedPointId);
                if (!point) {
                    return true;
                }
                point->mapPosition = mapPos + m_ctrlDragOffset;
                updatePointGraphics(*point);
                refreshPathsForPoint(point->id);
                m_draggedPointMoved = true;
                if (m_pointXSpin) {
                    QSignalBlocker blocker(m_pointXSpin);
                    m_pointXSpin->setValue(point->mapPosition.x());
                }
                if (m_pointYSpin) {
                    QSignalBlocker blocker(m_pointYSpin);
                    m_pointYSpin->setValue(point->mapPosition.y());
                }
                updatePointTableRow(point->id);
                setRouteStatusText(tr("正在移动点 %1").arg(point->id));
                return true;
            }
            if (m_altRotationActive && m_draggedPointId != -1) {
                if (!(mouseEvent->buttons() & Qt::LeftButton)) {
                    return true;
                }
                MapPoint *point = pointById(m_draggedPointId);
                if (!point) {
                    return true;
                }
                const double dx = mapPos.x() - point->mapPosition.x();
                const double dy = mapPos.y() - point->mapPosition.y();
                if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy)) {
                    return true;
                }
                const double currentAngle = angleFromMapVector(dx, dy);
                const double delta = normalizeAngle(currentAngle - m_rotationReferenceAngle);
                const double newTheta = normalizeAngle(m_rotationStartTheta - delta);
                if (!qFuzzyCompare(point->theta + 1.0, newTheta + 1.0)) {
                    point->theta = newTheta;
                    updatePointGraphics(*point);
                    m_draggedPointRotated = true;
                    if (m_pointThetaSpin) {
                        QSignalBlocker blocker(m_pointThetaSpin);
                        m_pointThetaSpin->setValue(point->theta);
                    }
                    updatePointTableRow(point->id);
                    setRouteStatusText(tr("正在旋转点 %1").arg(point->id));
                }
                return true;
            }
            break;
        }
                case QEvent::MouseButtonRelease: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (m_altRotationActive) {
                    if (m_draggedPointId != -1 && m_draggedPointRotated) {
                        refreshPathsForPoint(m_draggedPointId);
                        refreshPointUi();
                        setRouteStatusText(tr("已调整点 %1 朝向").arg(m_draggedPointId));
                    }
                    m_altRotationActive = false;
                    m_draggedPointId = -1;
                    return true;
                }
                if (m_ctrlDragActive) {
                    if (m_draggedPointId != -1 && m_draggedPointMoved) {
                        refreshPathsForPoint(m_draggedPointId);
                        refreshPointUi();
                        setRouteStatusText(tr("已移动点 %1").arg(m_draggedPointId));
                    }
                    m_ctrlDragActive = false;
                    m_draggedPointId = -1;
                    return true;
                }
            }
            m_altRotationActive = false;
            m_ctrlDragActive = false;
            m_draggedPointId = -1;
            break;
        }
default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void Map::handleSceneClick(const QPointF &scenePos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);

    const QPointF mapPos = sceneToMap(scenePos);

    if (button == Qt::RightButton) {
        if (!m_editModeEnabled) {
            return;
        }

        double bestDist = 0.0;
        const int nearestId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &bestDist);

        QMenu menu(m_mapPage);
        QAction *createAction = nullptr;
        QAction *deleteAction = nullptr;
        if (nearestId != -1) {
            deleteAction = menu.addAction(tr("删除地图点"));
        } else {
            createAction = menu.addAction(tr("新建地图点"));
        }

        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen) {
            return;
        }

        if (chosen == createAction) {
            const double theta = m_pointThetaSpin ? m_pointThetaSpin->value() : 0.0;
            if (addPointInternal(mapPos.x(), mapPos.y(), theta)) {
                m_waitingForClickPlacement = false;
                if (m_addPointFromClickButton) {
                    QSignalBlocker blocker(m_addPointFromClickButton);
                    m_addPointFromClickButton->setChecked(false);
                }
                if (m_pointXSpin) {
                    QSignalBlocker blocker(m_pointXSpin);
                    m_pointXSpin->setValue(mapPos.x());
                }
                if (m_pointYSpin) {
                    QSignalBlocker blocker(m_pointYSpin);
                    m_pointYSpin->setValue(mapPos.y());
                }
                if (m_pointThetaSpin) {
                    QSignalBlocker blocker(m_pointThetaSpin);
                    m_pointThetaSpin->setValue(theta);
                }
                refreshPointUi();
                refreshSelectors();
                setRouteStatusText(tr("已新建点 (X=%1, Y=%2)").arg(formatNumber(mapPos.x(), 2),
                                                               formatNumber(mapPos.y(), 2)));
    }
    return;
}

        if (chosen == deleteAction && nearestId != -1) {
            removePointInternal(nearestId);
            resetNextPointId();

            for (int i = m_routeQueue.size() - 1; i >= 0; --i) {
                const RouteStep &step = m_routeQueue.at(i);
                if (!m_points.contains(step.fromId) || !m_points.contains(step.toId)) {
                    m_routeQueue.removeAt(i);
                }
            }

            refreshPointUi();
            refreshPathUi();
            refreshSelectors();
            refreshRouteQueueUi();
            updateRouteControlState();
            setRouteStatusText(tr("已删除点 %1").arg(nearestId));
            return;
        }

        return;
    }

    if (button == Qt::LeftButton) {
        if (m_rowWorkClickPlacementMode) {
            addRowWorkCheckpoint(mapPos);
            m_rowWorkClickPlacementMode = false;
            if (m_rowWorkAddCheckpointFromMapButton) {
                QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
                m_rowWorkAddCheckpointFromMapButton->setChecked(false);
            }
            setRowWorkStatusText(tr("已添加地图中间点"));
            return;
        }

        if (m_waitingForClickPlacement) {
            const double theta = m_pointThetaSpin ? m_pointThetaSpin->value() : 0.0;
            if (addPointInternal(mapPos.x(), mapPos.y(), theta)) {
                m_waitingForClickPlacement = false;
                if (m_addPointFromClickButton) {
                    QSignalBlocker blocker(m_addPointFromClickButton);
                    m_addPointFromClickButton->setChecked(false);
                }
                refreshPointUi();
                refreshSelectors();
                setRouteStatusText(tr("已添加点 (X=%1, Y=%2)").arg(formatNumber(mapPos.x(), 2),
                                                               formatNumber(mapPos.y(), 2)));
            }
            return;
        }

        double bestDist = 0.0;
        const int nearestId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &bestDist);

        if (nearestId != -1) {
            m_pendingPointSelection = nearestId;
            if (m_pointTable) {
                const QList<QTableWidgetItem *> items =
                    m_pointTable->findItems(QString::number(nearestId), Qt::MatchExactly);
                if (!items.isEmpty()) {
                    const int row = items.first()->row();
                    m_pointTable->setCurrentCell(row, 0, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                }
            }
            if (m_pointThetaSpin) {
                QSignalBlocker blocker(m_pointThetaSpin);
                if (const MapPoint *point = pointById(nearestId)) {
                    m_pointThetaSpin->setValue(point->theta);
                }
            }
        }

        if (m_pointXSpin) {
            QSignalBlocker blocker(m_pointXSpin);
            m_pointXSpin->setValue(mapPos.x());
        }
        if (m_pointYSpin) {
            QSignalBlocker blocker(m_pointYSpin);
            m_pointYSpin->setValue(mapPos.y());
        }
    }
}

void Map::handleSceneMouseMoved(const QPointF &scenePos)
{
    if (!m_mousePositionLabel) {
        return;
    }
    const QPointF mapPos = sceneToMap(scenePos);
    m_mousePositionLabel->setText(tr("X: %1  Y: %2")
                                      .arg(mapPos.x(), 0, 'f', 3)
                                      .arg(mapPos.y(), 0, 'f', 3));
}

void Map::handleRotationSliderValueChanged(int value)
{
    const double rotationDeg = static_cast<double>(value) / 10.0;
    if (qFuzzyCompare(rotationDeg + 1.0, m_mapRotationDeg + 1.0)) {
        return;
    }
    setMapRotation(rotationDeg);
    applyPresentationUpdates(false);
    setRouteStatusText(tr("地图旋转 %1°").arg(m_mapRotationDeg, 0, 'f', 1));
}

void Map::handleRotationSpinChanged(double value)
{
    if (qFuzzyCompare(value + 1.0, m_mapRotationDeg + 1.0)) {
        return;
    }
    setMapRotation(value);
    applyPresentationUpdates(false);
    setRouteStatusText(tr("地图旋转 %1°").arg(m_mapRotationDeg, 0, 'f', 1));
}

void Map::handleGenerateMap()
{
    if (!m_gridWidthSpin || !m_gridHeightSpin) {
        return;
    }

    const int width = std::clamp(m_gridWidthSpin->value(), 1, kMaxGridSize);
    const int height = std::clamp(m_gridHeightSpin->value(), 1, kMaxGridSize);

    m_gridWidthSpin->setValue(width);
    m_gridHeightSpin->setValue(height);

    m_gridWidth = width;
    m_gridHeight = height;

    const double rotationDeg = m_gridRotationSpin ? m_gridRotationSpin->value() : 0.0;
    setMapRotation(rotationDeg);

    applyPresentationUpdates(true);

    setRouteStatusText(tr("Grid size updated to %1x%2 (rotation %3°)")
                           .arg(width)
                           .arg(height)
                           .arg(m_mapRotationDeg, 0, 'f', 1));
}

void Map::handleAddPointFromInput()
{
    if (!m_pointXSpin || !m_pointYSpin || !m_pointThetaSpin) {
        return;
    }

    const double x = m_pointXSpin->value();
    const double y = m_pointYSpin->value();
    const double theta = m_pointThetaSpin->value();

    if (!addPointInternal(x, y, theta)) {
        QMessageBox::warning(m_mapPage, tr("添加点"), tr("无法添加该点，编号可能已存在"));
        return;
    }

    refreshPointUi();
    refreshSelectors();
    setRouteStatusText(tr("已添加点 (X=%1, Y=%2)").arg(formatNumber(x, 2), formatNumber(y, 2)));
}

void Map::handlePointSelectionChanged()
{
    if (!m_pointTable || !m_pointXSpin || !m_pointYSpin || !m_pointThetaSpin) {
        return;
    }

    const QList<QTableWidgetItem *> items = m_pointTable->selectedItems();
    if (items.isEmpty()) {
        m_pendingPointSelection.reset();
        return;
    }

    const int row = items.first()->row();
    if (row < 0) {
        m_pendingPointSelection.reset();
        return;
    }

    const QTableWidgetItem *idItem = m_pointTable->item(row, 0);
    if (!idItem) {
        m_pendingPointSelection.reset();
        return;
    }

    const int pointId = idItem->text().toInt();
    m_pendingPointSelection = pointId;

    if (const MapPoint *point = pointById(pointId)) {
        m_pointXSpin->setValue(point->mapPosition.x());
        m_pointYSpin->setValue(point->mapPosition.y());
        m_pointThetaSpin->setValue(point->theta);
    }
}

void Map::handleUpdatePoint()
{
    if (!m_pendingPointSelection.has_value()) {
        QMessageBox::information(m_mapPage, tr("更新点"), tr("请先选择需要更新的点"));
        return;
    }

    const int pointId = m_pendingPointSelection.value();
    MapPoint *point = pointById(pointId);
    if (!point) {
        return;
    }

    point->mapPosition = QPointF(m_pointXSpin ? m_pointXSpin->value() : point->mapPosition.x(),
                                  m_pointYSpin ? m_pointYSpin->value() : point->mapPosition.y());
    point->theta = m_pointThetaSpin ? normalizeAngle(m_pointThetaSpin->value()) : point->theta;

    updatePointGraphics(*point);
    refreshPathsForPoint(pointId);
    refreshPointUi();
    refreshPathUi();
    setRouteStatusText(tr("已更新点 %1").arg(pointId));
}

void Map::handleAddPointFromClickRequested()
{
    if (!m_editModeEnabled) {
        QMessageBox::information(m_mapPage, tr("添加点"), tr("请先启用编辑模式"));
        if (m_addPointFromClickButton) {
            QSignalBlocker blocker(m_addPointFromClickButton);
            m_addPointFromClickButton->setChecked(false);
        }
        return;
    }

    m_waitingForClickPlacement = !m_waitingForClickPlacement;
    if (m_addPointFromClickButton) {
        QSignalBlocker blocker(m_addPointFromClickButton);
        m_addPointFromClickButton->setChecked(m_waitingForClickPlacement);
    }

    setRouteStatusText(m_waitingForClickPlacement ? tr("在地图上点击以创建点") : tr("已取消点击创建点"));
}

void Map::handleRemoveSelectedPoints()
{
    if (!m_pointTable) {
        return;
    }

    const QList<QTableWidgetItem *> items = m_pointTable->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::information(m_mapPage, tr("删除点"), tr("请先选择需要删除的点"));
        return;
    }

    QSet<int> idsToRemove;
    for (QTableWidgetItem *item : items) {
        if (item->column() == 0) {
            idsToRemove.insert(item->text().toInt());
        }
    }

    if (idsToRemove.isEmpty()) {
        return;
    }

    for (int pointId : idsToRemove) {
        removePointInternal(pointId);
    }

    resetNextPointId();

    for (int i = m_routeQueue.size() - 1; i >= 0; --i) {
        const RouteStep &step = m_routeQueue.at(i);
        if (!m_points.contains(step.fromId) || !m_points.contains(step.toId)) {
            m_routeQueue.removeAt(i);
        }
    }

    refreshPointUi();
    refreshPathUi();
    refreshSelectors();
    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleAddPath()
{
    if (!m_startPointCombo || !m_endPointCombo || !m_pathTypeCombo) {
        return;
    }

    const int startId = m_startPointCombo->currentData().toInt();
    const int endId = m_endPointCombo->currentData().toInt();
    if (startId <= 0 || endId <= 0) {
        QMessageBox::information(m_mapPage, tr("添加路径"), tr("请选择有效的起点和终点"));
        return;
    }
    if (startId == endId) {
        QMessageBox::information(m_mapPage, tr("添加路径"), tr("起点和终点不能相同"));
        return;
    }

    const PathType type = static_cast<PathType>(m_pathTypeCombo->currentIndex());
    const double sagitta = m_arcSagittaSpin ? m_arcSagittaSpin->value() : 0.0;

    addPathOrWarn(startId, endId, type, sagitta);
}

void Map::handleRemoveSelectedPaths()
{
    if (!m_pathTable) {
        return;
    }

    const QList<QTableWidgetItem *> items = m_pathTable->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::information(m_mapPage, tr("删除路径"), tr("请先选择需要删除的路径"));
        return;
    }

    QSet<int> pathIds;
    for (QTableWidgetItem *item : items) {
        if (item->column() == 0) {
            pathIds.insert(item->text().toInt());
        }
    }

    if (pathIds.isEmpty()) {
        return;
    }

    for (int pathId : pathIds) {
        removePathInternal(pathId);
    }

    resetNextPathId();

    refreshPathUi();
    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleBatchGeneratePoints()
{
    if (!m_batchDirectionCombo || !m_batchCountSpin || !m_batchSpacingSpin || !m_batchThetaSpin) {
        return;
    }

    const int count = std::max(0, m_batchCountSpin->value());
    if (count <= 0) {
        QMessageBox::information(m_mapPage, tr("批量生成"), tr("请指定要生成的点数量"));
        return;
    }

    QPointF base(0.0, 0.0);
    if (m_batchBasePointCombo) {
        const int baseId = m_batchBasePointCombo->currentData().toInt();
        if (baseId > 0) {
            if (const MapPoint *basePoint = pointById(baseId)) {
                base = basePoint->mapPosition;
            }
        }
    }

    QPointF direction(1.0, 0.0);
    if (m_batchDirectionCombo) {
        switch (m_batchDirectionCombo->currentIndex()) {
        case 0: direction = QPointF(1.0, 0.0); break;
        case 1: direction = QPointF(-1.0, 0.0); break;
        case 2: direction = QPointF(0.0, 1.0); break;
        case 3: direction = QPointF(0.0, -1.0); break;
        default: break;
        }
    }

    const double spacing = std::max(0.0, m_batchSpacingSpin->value());
    const double theta = m_batchThetaSpin->value();

    int created = 0;
    for (int i = 1; i <= count; ++i) {
        const QPointF position = base + direction * (spacing * i);
        if (addPointInternal(position.x(), position.y(), theta)) {
            ++created;
        }
    }

    if (created > 0) {
        refreshPointUi();
        refreshSelectors();
        setRouteStatusText(tr("已批量生成 %1 个点").arg(created));
    } else {
        setRouteStatusText(tr("未生成新点"), true);
    }
}
void Map::handleRouteAdd()
{
    if (!m_routeStartCombo || !m_routeEndCombo) {
        return;
    }

    const int fromId = m_routeStartCombo->currentData().toInt();
    const int toId = m_routeEndCombo->currentData().toInt();
    if (fromId <= 0 || toId <= 0) {
        QMessageBox::information(m_mapPage, tr("添加路线"), tr("请选择有效的起点和终点"));
        return;
    }
    if (fromId == toId) {
        QMessageBox::information(m_mapPage, tr("添加路线"), tr("起点和终点不能相同"));
        return;
    }

    QList<int> pathIds = findRoutePathIds(fromId, toId);
    if (pathIds.isEmpty()) {
        QMessageBox::warning(m_mapPage, tr("添加路线"),
                             tr("点%1到点%2之间没有可用路径").arg(fromId).arg(toId));
        return;
    }

    if (!m_routeQueue.isEmpty()) {
        const RouteStep &lastStep = m_routeQueue.constLast();
        if (lastStep.toId != fromId) {
            QMessageBox::warning(m_mapPage, tr("添加路线"),
                                 tr("新的路线起点必须是上一条路线的终点 (点%1)"
                                    ).arg(lastStep.toId));
            return;
        }
    }

    RouteStep step;
    step.fromId = fromId;
    step.toId = toId;
    step.pathIds = pathIds;
    step.pointSequence = buildPointSequenceFromPaths(pathIds, fromId);
    step.progressEdgeIndex = 0;
    m_routeQueue.append(step);

    refreshRouteQueueUi();
    updateRouteControlState();
    setRouteStatusText(tr("已添加路线: 点%1 → 点%2").arg(fromId).arg(toId));

    if (m_routeStartCombo) {
        const int index = m_routeStartCombo->findData(toId);
        if (index >= 0) {
            m_routeStartCombo->setCurrentIndex(index);
        }
    }
}

void Map::handleRouteRemove()
{
    if (!m_routeQueueList) {
        return;
    }

    const int row = m_routeQueueList->currentRow();
    if (row < 0 || row >= m_routeQueue.size()) {
        return;
    }

    m_routeQueue.removeAt(row);

    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleRouteClear()
{
    if (m_routeQueue.isEmpty()) {
        return;
    }

    m_routeQueue.clear();
    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleRouteStart()
{
    refreshPlanningRevision();
    if (!m_coordinator || m_routeQueue.isEmpty()) return;
    QList<TaskRouteSection> sections;
    for (auto step : m_routeQueue) {
        if (!rebuildRouteStep(step)) { setRouteStatusText(tr("路线中存在无法连通的步骤"), true); return; }
        TaskRouteSection section;
        section.id = QStringLiteral("route-%1-%2").arg(step.fromId).arg(step.toId);
        section.points = composePolyline(step.pathIds);
        if (const auto* end = pointById(step.toId)) section.goalMapYaw = end->theta;
        sections.append(section);
    }
    auto options = taskOptions();
    options.repeatUntilStopped = m_routeLoopCheck && m_routeLoopCheck->isChecked();
    const auto compiled = TaskCompiler::route(sections, m_binding, options, tr("常规路线"));
    if (!compiled.ok()) { setRouteStatusText(compiled.error, true); return; }
    auto plan = compiled.plan;
    if (options.repeatUntilStopped && m_routeLoopIntervalSpin && m_routeLoopIntervalSpin->value() > 0) {
        auto steps = plan["steps"].toArray();
        steps.append(QJsonObject{{"stepId", "cycle-wait"}, {"type", "wait"}, {"completion",
            QJsonObject{{"type", "timer"}, {"durationMs", m_routeLoopIntervalSpin->value() * 1000}}}});
        plan["steps"] = steps;
    }
    if (m_coordinator->upload(plan)) setRouteStatusText(tr("任务已提交校验，Ready 后请点击启动已就绪任务"));
}

void Map::handleRoutePause()
{
    if (m_coordinator) m_coordinator->pauseTask();
}

void Map::handleRouteResume()
{
    if (m_coordinator) { refreshPlanningRevision(); m_coordinator->resumeTask(); }
}

void Map::handleRouteStop()
{
    if (m_coordinator) m_coordinator->abortTask();
}


void Map::handleLocateCurrentPosition()
{
    if (!m_view || !m_hasVehiclePose) {
        return;
    }

    const QPointF vehiclePos(m_vehiclePoseX, m_vehiclePoseY);
    m_view->centerOn(mapToScene(vehiclePos));

    const double theta = m_vehiclePoseTheta;
    if (addPointInternal(vehiclePos.x(), vehiclePos.y(), theta)) {
        refreshPointUi();
        refreshSelectors();
        setRouteStatusText(tr("已在当前位置生成点：X=%1, Y=%2")
                               .arg(formatNumber(vehiclePos.x(), 2), formatNumber(vehiclePos.y(), 2)));
    }
}

void Map::handleEditModeToggled(bool checked)
{
    m_editModeEnabled = checked;
    if (!checked) {
        m_waitingForClickPlacement = false;
        if (m_addPointFromClickButton) {
            QSignalBlocker blocker(m_addPointFromClickButton);
            m_addPointFromClickButton->setChecked(false);
        }
    }
    if (m_addPointFromClickButton) {
        m_addPointFromClickButton->setEnabled(checked);
    }
    setRouteStatusText(checked ? tr("编辑模式已启用") : tr("编辑模式已关闭"));
}

void Map::handleZoomIn()
{
    if (m_view) {
        m_view->zoomByFactor(1.2);
    }
}

void Map::handleZoomOut()
{
    if (m_view) {
        m_view->zoomByFactor(1.0 / 1.2);
    }
}
void Map::handleLoadMap()
{
    const QString startDir = !m_lastLoadDirectory.isEmpty() ? m_lastLoadDirectory
                              : (!m_currentMapFilePath.isEmpty() ? QFileInfo(m_currentMapFilePath).absolutePath()
                                 : (!s_lastMapFilePath.isEmpty() ? QFileInfo(s_lastMapFilePath).absolutePath()
                                                                 : QDir::currentPath()));

    const QString filePath = QFileDialog::getOpenFileName(m_mapPage, tr("加载地图"), startDir,
                                                          tr("地图文件 (*.json);;所有文件 (*.*)"));
    if (filePath.isEmpty()) {
        LoggingManager::audit(QStringLiteral("map.load"),
                              QStringLiteral("cancelled"));
        return;
    }

    if (loadMapFromFile(filePath)) {
        m_lastLoadDirectory = QFileInfo(filePath).absolutePath();
        LoggingManager::audit(QStringLiteral("map.load"),
                              QStringLiteral("success"),
                              {{QStringLiteral("path"), filePath},
                               {QStringLiteral("points"), QString::number(m_points.size())},
                               {QStringLiteral("paths"), QString::number(m_paths.size())}});
    } else {
        LoggingManager::audit(QStringLiteral("map.load"),
                              QStringLiteral("failed"),
                              {{QStringLiteral("path"), filePath}});
    }
}

void Map::handleNewMap()
{
    if (m_currentMapFilePath.isEmpty()) {
        if (!hasUnsavedMapChanges()) {
            QMessageBox::information(m_mapPage, tr("新建地图"), tr("当前已是空地图"));
            return;
        }

        QMessageBox dialog(m_mapPage);
        dialog.setIcon(QMessageBox::Question);
        dialog.setWindowTitle(tr("新建地图"));
        dialog.setText(tr("是否保存当前新建地图"));
        QPushButton *saveButton = dialog.addButton(tr("保存"), QMessageBox::AcceptRole);
        QPushButton *discardButton = dialog.addButton(tr("不保存"), QMessageBox::DestructiveRole);
        dialog.exec();

        if (dialog.clickedButton() == saveButton) {
            if (!saveCurrentMapInteractive(true)) {
                LoggingManager::audit(QStringLiteral("map.new"),
                                      QStringLiteral("cancelled"),
                                      {{QStringLiteral("reason"), QStringLiteral("save_failed_or_cancelled")}});
                return;
            }
        } else if (dialog.clickedButton() != discardButton) {
            LoggingManager::audit(QStringLiteral("map.new"),
                                  QStringLiteral("cancelled"));
            return;
        }

        resetToBlankMap();
        LoggingManager::audit(QStringLiteral("map.new"),
                              QStringLiteral("success"),
                              {{QStringLiteral("previous"), QStringLiteral("unsaved_blank")}});
        return;
    }

    QMessageBox dialog(m_mapPage);
    dialog.setIcon(QMessageBox::Question);
    dialog.setWindowTitle(tr("新建地图"));
    dialog.setText(tr("是否保存当前对地图的修改"));
    QPushButton *saveButton = dialog.addButton(tr("保存"), QMessageBox::AcceptRole);
    QPushButton *discardButton = dialog.addButton(tr("不保存"), QMessageBox::DestructiveRole);
    dialog.exec();

    if (dialog.clickedButton() == saveButton) {
        if (!saveCurrentMapInteractive(false)) {
            LoggingManager::audit(QStringLiteral("map.new"),
                                  QStringLiteral("cancelled"),
                                  {{QStringLiteral("reason"), QStringLiteral("save_failed_or_cancelled")}});
            return;
        }
    } else if (dialog.clickedButton() != discardButton) {
        LoggingManager::audit(QStringLiteral("map.new"),
                              QStringLiteral("cancelled"));
        return;
    }

    const QString previousPath = m_currentMapFilePath;
    resetToBlankMap();
    LoggingManager::audit(QStringLiteral("map.new"),
                          QStringLiteral("success"),
                          {{QStringLiteral("previous"), previousPath}});
}

void Map::handleSaveMap()
{
    saveCurrentMapInteractive(true);
}

void Map::handleQuickSave()
{
    saveCurrentMapInteractive(m_currentMapFilePath.isEmpty());
}

void Map::initializeUi()
{
    if (!ui) {
        return;
    }

    m_mapPage = ui->mapPage;
    if (!m_mapPage) {
        return;
    }

    if (!m_togglePathsShortcut) {
        m_togglePathsShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_A), m_mapPage);
        m_togglePathsShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(m_togglePathsShortcut, &QShortcut::activated, this, &Map::handleTogglePathsVisibility);
    }

    if (auto existingLayout = m_mapPage->layout()) {
        QLayoutItem *item = nullptr;
        while ((item = existingLayout->takeAt(0)) != nullptr) {
            if (QWidget *widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
        delete existingLayout;
    }

    auto rootLayout = new QHBoxLayout(m_mapPage);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    auto mainSplitter = new QSplitter(Qt::Horizontal, m_mapPage);
    mainSplitter->setObjectName(QStringLiteral("mapMainSplitter"));
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setOpaqueResize(true);
    mainSplitter->setHandleWidth(8);
    rootLayout->addWidget(mainSplitter);

    auto leftPanel = new QFrame(m_mapPage);
    leftPanel->setObjectName(QStringLiteral("mapLeftPanel"));
    leftPanel->setFrameShape(QFrame::StyledPanel);
    leftPanel->setMinimumWidth(250);
    auto leftPanelLayout = new QVBoxLayout(leftPanel);
    leftPanelLayout->setContentsMargins(6, 6, 6, 6);
    leftPanelLayout->setSpacing(6);

    auto leftScrollArea = new QScrollArea(leftPanel);
    leftScrollArea->setObjectName(QStringLiteral("mapLeftScrollArea"));
    leftScrollArea->setWidgetResizable(true);
    leftScrollArea->setFrameShape(QFrame::NoFrame);
    leftScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScrollArea->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto leftScrollWidget = new QWidget(leftScrollArea);
    leftScrollWidget->setObjectName(QStringLiteral("mapLeftScrollWidget"));
    auto leftScrollLayout = new QVBoxLayout(leftScrollWidget);
    leftScrollLayout->setContentsMargins(0, 0, 0, 0);
    leftScrollLayout->setSpacing(8);

    const auto configureSpinField = [](QWidget *field, int minWidth = 90) {
        if (!field) {
            return;
        }
        field->setMinimumWidth(minWidth);
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    const auto configureComboField = [](QComboBox *combo, int minWidth = 100) {
        if (!combo) {
            return;
        }
        combo->setMinimumWidth(minWidth);
        combo->setMaximumWidth(QWIDGETSIZE_MAX);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    };

    auto gridBox = new QGroupBox(tr("地图尺寸"), leftScrollWidget);
    gridBox->setObjectName(QStringLiteral("mapGridBox"));
    gridBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto gridForm = new QFormLayout(gridBox);
    gridForm->setContentsMargins(8, 8, 8, 8);
    gridForm->setSpacing(6);
    gridForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_gridWidthSpin = new QSpinBox(gridBox);
    m_gridWidthSpin->setObjectName(QStringLiteral("mapGridWidthSpin"));
    m_gridWidthSpin->setRange(1, kMaxGridSize);
    m_gridWidthSpin->setValue(kDefaultGridWidth);
    configureSpinField(m_gridWidthSpin);
    gridForm->addRow(tr("宽度(格)"), m_gridWidthSpin);

    m_gridHeightSpin = new QSpinBox(gridBox);
    m_gridHeightSpin->setObjectName(QStringLiteral("mapGridHeightSpin"));
    m_gridHeightSpin->setRange(1, kMaxGridSize);
    m_gridHeightSpin->setValue(kDefaultGridHeight);
    configureSpinField(m_gridHeightSpin);
    gridForm->addRow(tr("高度(格)"), m_gridHeightSpin);

    m_gridRotationSpin = new QDoubleSpinBox(gridBox);
    m_gridRotationSpin->setObjectName(QStringLiteral("mapGridRotationSpin"));
    m_gridRotationSpin->setRange(-360.0, 360.0);
    m_gridRotationSpin->setDecimals(1);
    m_gridRotationSpin->setSingleStep(1.0);
    m_gridRotationSpin->setSuffix(QStringLiteral("°"));
    m_gridRotationSpin->setValue(0.0);
    configureSpinField(m_gridRotationSpin);
    gridForm->addRow(tr("旋转(°)"), m_gridRotationSpin);

    m_buildGridButton = new QPushButton(tr("Update Size"), gridBox);
    m_buildGridButton->setObjectName(QStringLiteral("mapBuildGridButton"));
    m_buildGridButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    gridForm->addRow(QString(), m_buildGridButton);

    leftScrollLayout->addWidget(gridBox);

    auto pointBox = new QGroupBox(tr("点管理"), leftScrollWidget);
    pointBox->setObjectName(QStringLiteral("mapPointBox"));
    pointBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto pointLayout = new QVBoxLayout(pointBox);
    pointLayout->setContentsMargins(8, 8, 8, 8);
    pointLayout->setSpacing(6);

    auto pointForm = new QFormLayout();
    pointForm->setContentsMargins(0, 0, 0, 0);
    pointForm->setSpacing(4);
    pointForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_pointXSpin = new QDoubleSpinBox(pointBox);
    m_pointXSpin->setObjectName(QStringLiteral("mapPointXSpin"));
    m_pointXSpin->setRange(-10000.0, 10000.0);
    m_pointXSpin->setDecimals(3);
    m_pointXSpin->setSingleStep(0.1);
    configureSpinField(m_pointXSpin);
    pointForm->addRow(tr("X (m)"), m_pointXSpin);

    m_pointYSpin = new QDoubleSpinBox(pointBox);
    m_pointYSpin->setObjectName(QStringLiteral("mapPointYSpin"));
    m_pointYSpin->setRange(-10000.0, 10000.0);
    m_pointYSpin->setDecimals(3);
    m_pointYSpin->setSingleStep(0.1);
    configureSpinField(m_pointYSpin);
    pointForm->addRow(tr("Y (m)"), m_pointYSpin);

    m_pointThetaSpin = new QDoubleSpinBox(pointBox);
    m_pointThetaSpin->setObjectName(QStringLiteral("mapPointThetaSpin"));
    m_pointThetaSpin->setRange(-M_PI, M_PI);
    m_pointThetaSpin->setDecimals(4);
    m_pointThetaSpin->setSingleStep(0.1);
    configureSpinField(m_pointThetaSpin);
    pointForm->addRow(tr("角度 (rad)"), m_pointThetaSpin);

    pointLayout->addLayout(pointForm);

    auto pointButtonLayout = new QGridLayout();
    pointButtonLayout->setContentsMargins(0, 0, 0, 0);
    pointButtonLayout->setHorizontalSpacing(4);
    pointButtonLayout->setVerticalSpacing(4);

    m_addPointFromInputButton = new QPushButton(tr("添加"), pointBox);
    m_addPointFromInputButton->setObjectName(QStringLiteral("mapAddPointButton"));
    m_addPointFromInputButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pointButtonLayout->addWidget(m_addPointFromInputButton, 0, 0);

    m_addPointFromClickButton = new QPushButton(tr("点击添加"), pointBox);
    m_addPointFromClickButton->setObjectName(QStringLiteral("mapAddPointFromClickButton"));
    m_addPointFromClickButton->setCheckable(true);
    m_addPointFromClickButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pointButtonLayout->addWidget(m_addPointFromClickButton, 0, 1);

    m_updatePointButton = new QPushButton(tr("更新"), pointBox);
    m_updatePointButton->setObjectName(QStringLiteral("mapUpdatePointButton"));
    m_updatePointButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pointButtonLayout->addWidget(m_updatePointButton, 1, 0);

    m_removePointButton = new QPushButton(tr("删除"), pointBox);
    m_removePointButton->setObjectName(QStringLiteral("mapRemovePointButton"));
    m_removePointButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pointButtonLayout->addWidget(m_removePointButton, 1, 1);
    pointButtonLayout->setColumnStretch(0, 1);
    pointButtonLayout->setColumnStretch(1, 1);

    pointLayout->addLayout(pointButtonLayout);

    m_pointTable = new QTableWidget(pointBox);
    m_pointTable->setObjectName(QStringLiteral("mapPointTable"));
    m_pointTable->setColumnCount(4);
    m_pointTable->setHorizontalHeaderLabels({tr("编号"), tr("X"), tr("Y"), tr("角度")});
    m_pointTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_pointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pointTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    for (int col = 0; col < m_pointTable->columnCount(); ++col) {
        if (auto *item = m_pointTable->horizontalHeaderItem(col)) {
            item->setTextAlignment(Qt::AlignCenter);
        }
    }
    m_pointTable->horizontalHeader()->setStretchLastSection(true);
    m_pointTable->verticalHeader()->setVisible(false);
    m_pointTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pointTable->verticalHeader()->setDefaultSectionSize(28);
    m_pointTable->verticalHeader()->setMinimumSectionSize(24);
    m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pointTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pointTable->setAlternatingRowColors(true);
    m_pointTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pointLayout->addWidget(m_pointTable);

    leftScrollLayout->addWidget(pointBox, 2);

    auto batchBox = new QGroupBox(tr("批量生成点"), leftScrollWidget);
    batchBox->setObjectName(QStringLiteral("mapBatchBox"));
    batchBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto batchLayout = new QFormLayout(batchBox);
    batchLayout->setContentsMargins(8, 8, 8, 8);
    batchLayout->setSpacing(6);
    batchLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_batchBasePointCombo = new QComboBox(batchBox);
    configureComboField(m_batchBasePointCombo);
    batchLayout->addRow(tr("基准点"), m_batchBasePointCombo);

    m_batchDirectionCombo = new QComboBox(batchBox);
    configureComboField(m_batchDirectionCombo);
    m_batchDirectionCombo->addItem(tr("+X 方向 (向上)"));
    m_batchDirectionCombo->addItem(tr("-X 方向 (向下)"));
    m_batchDirectionCombo->addItem(tr("+Y 方向 (向右)"));
    m_batchDirectionCombo->addItem(tr("-Y 方向 (向左)"));
    batchLayout->addRow(tr("方向"), m_batchDirectionCombo);

    m_batchCountSpin = new QSpinBox(batchBox);
    m_batchCountSpin->setRange(1, 500);
    m_batchCountSpin->setValue(5);
    configureSpinField(m_batchCountSpin);
    batchLayout->addRow(tr("数量"), m_batchCountSpin);

    m_batchSpacingSpin = new QDoubleSpinBox(batchBox);
    m_batchSpacingSpin->setRange(0.0, 1000.0);
    m_batchSpacingSpin->setSingleStep(0.1);
    m_batchSpacingSpin->setDecimals(3);
    m_batchSpacingSpin->setValue(1.0);
    configureSpinField(m_batchSpacingSpin);
    batchLayout->addRow(tr("间隔 (m)"), m_batchSpacingSpin);

    m_batchThetaSpin = new QDoubleSpinBox(batchBox);
    m_batchThetaSpin->setRange(-M_PI, M_PI);
    m_batchThetaSpin->setDecimals(4);
    m_batchThetaSpin->setSingleStep(0.1);
    configureSpinField(m_batchThetaSpin);
    batchLayout->addRow(tr("方向角 (rad)"), m_batchThetaSpin);

    m_batchGenerateButton = new QPushButton(tr("生成"), batchBox);
    m_batchGenerateButton->setObjectName(QStringLiteral("mapBatchGenerateButton"));
    m_batchGenerateButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    batchLayout->addRow(QString(), m_batchGenerateButton);

    leftScrollLayout->addWidget(batchBox);

    auto pathBox = new QGroupBox(tr("路径管理"), leftScrollWidget);
    pathBox->setObjectName(QStringLiteral("mapPathBox"));
    pathBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto pathLayout = new QVBoxLayout(pathBox);
    pathLayout->setContentsMargins(8, 8, 8, 8);
    pathLayout->setSpacing(6);

    auto pathForm = new QFormLayout();
    pathForm->setContentsMargins(0, 0, 0, 0);
    pathForm->setSpacing(4);
    pathForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_startPointCombo = new QComboBox(pathBox);
    m_startPointCombo->setObjectName(QStringLiteral("mapPathStartCombo"));
    configureComboField(m_startPointCombo);
    pathForm->addRow(tr("起点"), m_startPointCombo);

    m_endPointCombo = new QComboBox(pathBox);
    m_endPointCombo->setObjectName(QStringLiteral("mapPathEndCombo"));
    configureComboField(m_endPointCombo);
    pathForm->addRow(tr("终点"), m_endPointCombo);

    m_pathTypeCombo = new QComboBox(pathBox);
    m_pathTypeCombo->setObjectName(QStringLiteral("mapPathTypeCombo"));
    m_pathTypeCombo->addItem(tr("直线"));
    m_pathTypeCombo->addItem(tr("圆弧"));
    configureComboField(m_pathTypeCombo, 90);
    pathForm->addRow(tr("类型"), m_pathTypeCombo);

    m_arcSagittaSpin = new QDoubleSpinBox(pathBox);
    m_arcSagittaSpin->setObjectName(QStringLiteral("mapArcSagittaSpin"));
    m_arcSagittaSpin->setRange(-1000.0, 1000.0);
    m_arcSagittaSpin->setDecimals(3);
    m_arcSagittaSpin->setSingleStep(0.1);
    m_arcSagittaSpin->setValue(1.0);
    configureSpinField(m_arcSagittaSpin);
    pathForm->addRow(tr("弓高 (m)"), m_arcSagittaSpin);

    pathLayout->addLayout(pathForm);

    auto pathButtonLayout = new QHBoxLayout();
    pathButtonLayout->setSpacing(4);

    m_addPathButton = new QPushButton(tr("添加"), pathBox);
    m_addPathButton->setObjectName(QStringLiteral("mapAddPathButton"));
    m_addPathButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pathButtonLayout->addWidget(m_addPathButton);

    m_removePathButton = new QPushButton(tr("删除"), pathBox);
    m_removePathButton->setObjectName(QStringLiteral("mapRemovePathButton"));
    m_removePathButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pathButtonLayout->addWidget(m_removePathButton);

    pathLayout->addLayout(pathButtonLayout);

    m_pathTable = new QTableWidget(pathBox);
    m_pathTable->setObjectName(QStringLiteral("mapPathTable"));
    m_pathTable->setColumnCount(6);
    m_pathTable->setHorizontalHeaderLabels({tr("编号"), tr("起点"), tr("终点"), tr("类型"), tr("弓高"), tr("长度")});
    m_pathTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pathTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pathTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_pathTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_pathTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_pathTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_pathTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    for (int col = 0; col < m_pathTable->columnCount(); ++col) {
        if (auto *item = m_pathTable->horizontalHeaderItem(col)) {
            item->setTextAlignment(Qt::AlignCenter);
        }
    }
    m_pathTable->verticalHeader()->setVisible(false);
    m_pathTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pathTable->verticalHeader()->setDefaultSectionSize(26);
    m_pathTable->verticalHeader()->setMinimumSectionSize(22);
    m_pathTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pathTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pathTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pathTable->setAlternatingRowColors(true);
    m_pathTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pathLayout->addWidget(m_pathTable);

    leftScrollLayout->addWidget(pathBox, 3);
    leftScrollLayout->addStretch();
    leftScrollArea->setWidget(leftScrollWidget);
    leftPanelLayout->addWidget(leftScrollArea, 1);

    mainSplitter->addWidget(leftPanel);

    auto centerPanel = new QFrame(m_mapPage);
    centerPanel->setObjectName(QStringLiteral("mapCenterPanel"));
    centerPanel->setFrameShape(QFrame::StyledPanel);
    auto centerLayout = new QVBoxLayout(centerPanel);
    centerLayout->setContentsMargins(8, 8, 8, 8);
    centerLayout->setSpacing(8);

    auto topBar = new QHBoxLayout();
    topBar->setSpacing(6);

    m_mapNameLabel = new QLabel(tr("未保存地图"), centerPanel);
    m_mapNameLabel->setObjectName(QStringLiteral("mapNameLabel"));
    m_mapNameLabel->setMinimumWidth(90);
    topBar->addWidget(m_mapNameLabel);

    m_mousePositionLabel = new QLabel(tr("X: 0.000  Y: 0.000"), centerPanel);
    m_mousePositionLabel->setObjectName(QStringLiteral("mapMousePositionLabel"));
    m_mousePositionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_mousePositionLabel->setMinimumWidth(90);
    topBar->addWidget(m_mousePositionLabel, 1);

    m_newMapButton = new QPushButton(tr("新建"), centerPanel);
    m_newMapButton->setObjectName(QStringLiteral("mapNewButton"));
    topBar->addWidget(m_newMapButton);

    m_loadMapButton = new QPushButton(tr("加载…"), centerPanel);
    m_loadMapButton->setObjectName(QStringLiteral("mapLoadButton"));
    topBar->addWidget(m_loadMapButton);

    m_saveMapButton = new QPushButton(tr("另存为…"), centerPanel);
    m_saveMapButton->setObjectName(QStringLiteral("mapSaveAsButton"));
    topBar->addWidget(m_saveMapButton);

    m_quickSaveButton = new QPushButton(tr("保存"), centerPanel);
    m_quickSaveButton->setObjectName(QStringLiteral("mapQuickSaveButton"));
    topBar->addWidget(m_quickSaveButton);

    topBar->addStretch();

    m_editModeButton = new QPushButton(tr("编辑模式"), centerPanel);
    m_editModeButton->setObjectName(QStringLiteral("mapEditModeButton"));
    m_editModeButton->setCheckable(true);
    m_editModeButton->setChecked(true);
    topBar->addWidget(m_editModeButton);

    m_locateButton = new QPushButton(tr("定位"), centerPanel);
    m_locateButton->setObjectName(QStringLiteral("mapLocateButton"));
    topBar->addWidget(m_locateButton);

    centerLayout->addLayout(topBar);

    m_view = new MapGraphicsView(centerPanel);
    m_view->setObjectName(QStringLiteral("mapGraphicsView"));
    m_view->setMinimumSize(320, 240);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_view->viewport()->setCursor(Qt::CrossCursor);
    m_view->viewport()->installEventFilter(this);
    centerLayout->addWidget(m_view, 1);

    m_rotationSlider = new QSlider(Qt::Horizontal, centerPanel);
    m_rotationSlider->setObjectName(QStringLiteral("mapRotationSlider"));
    m_rotationSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rotationSlider->setRange(-1800, 1800);
    m_rotationSlider->setSingleStep(5);
    m_rotationSlider->setPageStep(30);
    m_rotationSlider->setValue(0);
    m_rotationSlider->setToolTip(tr("地图旋转控制"));
    centerLayout->addWidget(m_rotationSlider);

    mainSplitter->addWidget(centerPanel);

    auto rightPanel = new QFrame(m_mapPage);
    rightPanel->setObjectName(QStringLiteral("mapRightPanel"));
    rightPanel->setFrameShape(QFrame::StyledPanel);
    rightPanel->setMinimumWidth(300);
    auto rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(8);

    auto* ipcBox = new QGroupBox(tr("工控机任务控制"), rightPanel);
    auto* ipcLayout = new QGridLayout(ipcBox);
    int ipcButtonIndex = 0;
    auto addIpcButton = [this, ipcBox, ipcLayout, &ipcButtonIndex](const QString& text, auto action) {
        auto* button = new QPushButton(text, ipcBox);
        ipcLayout->addWidget(button, ipcButtonIndex / 2, ipcButtonIndex % 2); ++ipcButtonIndex;
        connect(button, &QPushButton::clicked, this, action);
    };
    addIpcButton(tr("获取任务操作权"), [this] { if (m_coordinator) m_coordinator->acquireSession(); });
    addIpcButton(tr("确认地图坐标与原点"), [this] { confirmFrameBinding(); });
    addIpcButton(tr("启动已就绪任务"), [this] { if (m_coordinator) { refreshPlanningRevision(); m_coordinator->startTask(); } });
    addIpcButton(tr("急停"), [this] { if (m_coordinator) m_coordinator->emergencyStop(); });
    addIpcButton(tr("复位任务故障"), [this] { if (m_coordinator) m_coordinator->resetFault(); });
    addIpcButton(tr("人工核对检查点拍摄结果"), [this] {
        if (!m_tracking || m_tracking->snapshot().waitingEventId.isEmpty()) return;
        const auto eventId = m_tracking->snapshot().waitingEventId;
        const auto answer = QMessageBox::question(m_mapPage, tr("检查点拍摄结果"),
            tr("请核对检查点 %1 的照片。选择“是”确认拍摄成功；“否”报告失败；取消保持等待。")
                .arg(eventId), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Cancel) emit externalActionResolved(eventId, answer == QMessageBox::Yes);
    });
    m_trackingStatusLabel = new QLabel(tr("任务状态未知"), ipcBox); m_trackingStatusLabel->setWordWrap(true);
    ipcLayout->addWidget(m_trackingStatusLabel, (ipcButtonIndex + 1) / 2, 0, 1, 2);
    rightLayout->addWidget(ipcBox);
    auto taskTabWidget = new QTabWidget(rightPanel);
    taskTabWidget->setObjectName(QStringLiteral("mapTaskTabWidget"));
    taskTabWidget->setDocumentMode(true);
    taskTabWidget->setElideMode(Qt::ElideNone);

    auto routePage = new QWidget(taskTabWidget);
    routePage->setObjectName(QStringLiteral("mapRouteTaskPage"));
    auto routeLayout = new QVBoxLayout(routePage);
    routeLayout->setContentsMargins(8, 8, 8, 8);
    routeLayout->setSpacing(8);

    auto routeForm = new QFormLayout();
    routeForm->setContentsMargins(0, 0, 0, 0);
    routeForm->setSpacing(6);

    m_routeStartCombo = new QComboBox(routePage);
    m_routeStartCombo->setObjectName(QStringLiteral("mapRouteStartCombo"));
    routeForm->addRow(tr("起点"), m_routeStartCombo);

    m_routeEndCombo = new QComboBox(routePage);
    m_routeEndCombo->setObjectName(QStringLiteral("mapRouteEndCombo"));
    routeForm->addRow(tr("终点"), m_routeEndCombo);

    routeLayout->addLayout(routeForm);

    m_routeAddButton = new QPushButton(tr("添加路线"), routePage);
    m_routeAddButton->setObjectName(QStringLiteral("mapRouteAddButton"));
    routeLayout->addWidget(m_routeAddButton);

    m_routeQueueList = new QListWidget(routePage);
    m_routeQueueList->setObjectName(QStringLiteral("mapRouteQueueList"));
    m_routeQueueList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_routeQueueList->setAlternatingRowColors(true);
    m_routeQueueList->setMinimumHeight(220);
    routeLayout->addWidget(m_routeQueueList, 1);

    auto queueButtons = new QHBoxLayout();
    queueButtons->setSpacing(4);
    m_routeRemoveButton = new QPushButton(tr("移除"), routePage);
    m_routeRemoveButton->setObjectName(QStringLiteral("mapRouteRemoveButton"));
    queueButtons->addWidget(m_routeRemoveButton);
    m_routeClearButton = new QPushButton(tr("清空"), routePage);
    m_routeClearButton->setObjectName(QStringLiteral("mapRouteClearButton"));
    queueButtons->addWidget(m_routeClearButton);
    queueButtons->addStretch();
    routeLayout->addLayout(queueButtons);

    auto controlButtons = new QHBoxLayout();
    controlButtons->setSpacing(4);
    m_routeStartButton = new QPushButton(tr("开始"), routePage);
    m_routeStartButton->setObjectName(QStringLiteral("mapRouteStartButton"));
    controlButtons->addWidget(m_routeStartButton);
    m_routePauseButton = new QPushButton(tr("暂停"), routePage);
    m_routePauseButton->setObjectName(QStringLiteral("mapRoutePauseButton"));
    controlButtons->addWidget(m_routePauseButton);
    m_routeResumeButton = new QPushButton(tr("恢复"), routePage);
    m_routeResumeButton->setObjectName(QStringLiteral("mapRouteResumeButton"));
    controlButtons->addWidget(m_routeResumeButton);
    m_routeStopButton = new QPushButton(tr("停止"), routePage);
    m_routeStopButton->setObjectName(QStringLiteral("mapRouteStopButton"));
    controlButtons->addWidget(m_routeStopButton);
    routeLayout->addLayout(controlButtons);

    auto loopLayout = new QHBoxLayout();
    loopLayout->setSpacing(4);
    m_routeLoopCheck = new QCheckBox(tr("循环执行"), routePage);
    m_routeLoopCheck->setObjectName(QStringLiteral("mapRouteLoopCheck"));
    loopLayout->addWidget(m_routeLoopCheck);
    m_routeLoopIntervalSpin = new QSpinBox(routePage);
    m_routeLoopIntervalSpin->setObjectName(QStringLiteral("mapRouteLoopIntervalSpin"));
    m_routeLoopIntervalSpin->setRange(0, 3600);
    m_routeLoopIntervalSpin->setSuffix(tr(" 秒"));
    m_routeLoopIntervalSpin->setValue(kDefaultRouteLoopIntervalSeconds);
    m_routeLoopIntervalSpin->setEnabled(false);
    loopLayout->addWidget(m_routeLoopIntervalSpin);
    loopLayout->addStretch();
    routeLayout->addLayout(loopLayout);

    m_routeStatusLabel = new QLabel(tr("待命"), routePage);
    m_routeStatusLabel->setObjectName(QStringLiteral("mapRouteStatusLabel"));
    m_routeStatusLabel->setWordWrap(true);
    m_routeStatusLabel->setMinimumHeight(40);
    QPalette statusPalette = m_routeStatusLabel->palette();
    statusPalette.setColor(QPalette::WindowText, QColor(55, 55, 55));
    m_routeStatusLabel->setPalette(statusPalette);
    routeLayout->addWidget(m_routeStatusLabel);
    routeLayout->addStretch();

    auto rowWorkPage = new QWidget(taskTabWidget);
    rowWorkPage->setObjectName(QStringLiteral("mapRowWorkPage"));
    auto rowWorkLayout = new QVBoxLayout(rowWorkPage);
    rowWorkLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkLayout->setSpacing(8);

    m_rowWorkModeTabWidget = new QTabWidget(rowWorkPage);
    m_rowWorkModeTabWidget->setObjectName(QStringLiteral("mapRowWorkModeTabWidget"));
    m_rowWorkModeTabWidget->setDocumentMode(true);
    m_rowWorkModeTabWidget->setElideMode(Qt::ElideNone);

    auto rowWorkPrimitiveScrollArea = new QScrollArea(m_rowWorkModeTabWidget);
    rowWorkPrimitiveScrollArea->setObjectName(QStringLiteral("mapRowWorkPrimitiveScrollArea"));
    rowWorkPrimitiveScrollArea->setWidgetResizable(true);
    rowWorkPrimitiveScrollArea->setFrameShape(QFrame::NoFrame);

    m_rowWorkPrimitivePage = new QWidget(rowWorkPrimitiveScrollArea);
    m_rowWorkPrimitivePage->setObjectName(QStringLiteral("mapRowWorkPrimitivePage"));
    auto primitiveLayout = new QVBoxLayout(m_rowWorkPrimitivePage);
    primitiveLayout->setContentsMargins(8, 8, 8, 8);
    primitiveLayout->setSpacing(8);

    auto rowWorkTeachBox = new QGroupBox(tr("作业线示教"), m_rowWorkPrimitivePage);
    rowWorkTeachBox->setObjectName(QStringLiteral("mapRowWorkTeachBox"));
    auto rowWorkTeachLayout = new QFormLayout(rowWorkTeachBox);
    rowWorkTeachLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkTeachLayout->setSpacing(6);

    m_rowWorkStartLabel = new QLabel(tr("未记录"), rowWorkTeachBox);
    m_rowWorkStartLabel->setObjectName(QStringLiteral("mapRowWorkStartLabel"));
    rowWorkTeachLayout->addRow(tr("起点 A"), m_rowWorkStartLabel);

    m_rowWorkEndLabel = new QLabel(tr("未记录"), rowWorkTeachBox);
    m_rowWorkEndLabel->setObjectName(QStringLiteral("mapRowWorkEndLabel"));
    rowWorkTeachLayout->addRow(tr("终点 B"), m_rowWorkEndLabel);

    auto rowWorkTeachButtons = new QHBoxLayout();
    rowWorkTeachButtons->setSpacing(4);
    m_rowWorkCaptureStartButton = new QPushButton(tr("记录起点"), rowWorkTeachBox);
    m_rowWorkCaptureStartButton->setObjectName(QStringLiteral("mapRowWorkCaptureStartButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkCaptureStartButton);
    m_rowWorkCaptureEndButton = new QPushButton(tr("记录终点"), rowWorkTeachBox);
    m_rowWorkCaptureEndButton->setObjectName(QStringLiteral("mapRowWorkCaptureEndButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkCaptureEndButton);
    m_rowWorkClearLineButton = new QPushButton(tr("清除作业线"), rowWorkTeachBox);
    m_rowWorkClearLineButton->setObjectName(QStringLiteral("mapRowWorkClearLineButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkClearLineButton);
    rowWorkTeachLayout->addRow(QString(), rowWorkTeachButtons);

    primitiveLayout->addWidget(rowWorkTeachBox);

    auto rowWorkCheckpointBox = new QGroupBox(tr("中间点管理"), m_rowWorkPrimitivePage);
    rowWorkCheckpointBox->setObjectName(QStringLiteral("mapRowWorkCheckpointBox"));
    auto rowWorkCheckpointLayout = new QVBoxLayout(rowWorkCheckpointBox);
    rowWorkCheckpointLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkCheckpointLayout->setSpacing(6);

    auto rowWorkCheckpointButtons = new QHBoxLayout();
    rowWorkCheckpointButtons->setSpacing(4);
    m_rowWorkAddCheckpointFromVehicleButton = new QPushButton(tr("当前位置添加"), rowWorkCheckpointBox);
    m_rowWorkAddCheckpointFromVehicleButton->setObjectName(QStringLiteral("mapRowWorkAddCheckpointFromVehicleButton"));
    rowWorkCheckpointButtons->addWidget(m_rowWorkAddCheckpointFromVehicleButton);
    m_rowWorkAddCheckpointFromMapButton = new QPushButton(tr("地图点击添加"), rowWorkCheckpointBox);
    m_rowWorkAddCheckpointFromMapButton->setObjectName(QStringLiteral("mapRowWorkAddCheckpointFromMapButton"));
    m_rowWorkAddCheckpointFromMapButton->setCheckable(true);
    rowWorkCheckpointButtons->addWidget(m_rowWorkAddCheckpointFromMapButton);
    rowWorkCheckpointLayout->addLayout(rowWorkCheckpointButtons);

    auto rowWorkCheckpointButtons2 = new QHBoxLayout();
    rowWorkCheckpointButtons2->setSpacing(4);
    m_rowWorkDeleteCheckpointButton = new QPushButton(tr("删除中间点"), rowWorkCheckpointBox);
    m_rowWorkDeleteCheckpointButton->setObjectName(QStringLiteral("mapRowWorkDeleteCheckpointButton"));
    rowWorkCheckpointButtons2->addWidget(m_rowWorkDeleteCheckpointButton);
    m_rowWorkClearCheckpointsButton = new QPushButton(tr("清空中间点"), rowWorkCheckpointBox);
    m_rowWorkClearCheckpointsButton->setObjectName(QStringLiteral("mapRowWorkClearCheckpointsButton"));
    rowWorkCheckpointButtons2->addWidget(m_rowWorkClearCheckpointsButton);
    rowWorkCheckpointLayout->addLayout(rowWorkCheckpointButtons2);

    m_rowWorkCheckpointTable = new QTableWidget(rowWorkCheckpointBox);
    m_rowWorkCheckpointTable->setObjectName(QStringLiteral("mapRowWorkCheckpointTable"));
    m_rowWorkCheckpointTable->setColumnCount(7);
    m_rowWorkCheckpointTable->setHorizontalHeaderLabels(
        {tr("名称"), tr("距起点(m)"), tr("停留(ms)"), tr("正向"), tr("反向"), tr("拍摄"), tr("拍摄超时(ms)")});
    m_rowWorkCheckpointTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_rowWorkCheckpointTable->verticalHeader()->setVisible(false);
    m_rowWorkCheckpointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rowWorkCheckpointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rowWorkCheckpointTable->setAlternatingRowColors(true);
    m_rowWorkCheckpointTable->setMinimumHeight(180);
    rowWorkCheckpointLayout->addWidget(m_rowWorkCheckpointTable);

    primitiveLayout->addWidget(rowWorkCheckpointBox);

    auto rowWorkParamsBox = new QGroupBox(tr("作业参数"), m_rowWorkPrimitivePage);
    rowWorkParamsBox->setObjectName(QStringLiteral("mapRowWorkParamsBox"));
    auto rowWorkParamsLayout = new QFormLayout(rowWorkParamsBox);
    rowWorkParamsLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkParamsLayout->setSpacing(6);

    m_rowWorkBaseSpeedSpin = new QDoubleSpinBox(rowWorkParamsBox);
    m_rowWorkBaseSpeedSpin->setObjectName(QStringLiteral("mapRowWorkBaseSpeedSpin"));
    m_rowWorkBaseSpeedSpin->setRange(0.01, 5.0);
    m_rowWorkBaseSpeedSpin->setDecimals(3);
    m_rowWorkBaseSpeedSpin->setSingleStep(0.01);
    rowWorkParamsLayout->addRow(tr("基础速度(m/s)"), m_rowWorkBaseSpeedSpin);

    m_rowWorkMaxSpeedSpin = new QDoubleSpinBox(rowWorkParamsBox);
    m_rowWorkMaxSpeedSpin->setObjectName(QStringLiteral("mapRowWorkMaxSpeedSpin"));
    m_rowWorkMaxSpeedSpin->setRange(0.01, 5.0);
    m_rowWorkMaxSpeedSpin->setDecimals(3);
    m_rowWorkMaxSpeedSpin->setSingleStep(0.01);
    rowWorkParamsLayout->addRow(tr("最大速度(m/s)"), m_rowWorkMaxSpeedSpin);

    m_rowWorkEndpointArrivalSpin = new QDoubleSpinBox(rowWorkParamsBox);
    m_rowWorkEndpointArrivalSpin->setObjectName(QStringLiteral("mapRowWorkEndpointArrivalSpin"));
    m_rowWorkEndpointArrivalSpin->setRange(0.01, 1.0);
    m_rowWorkEndpointArrivalSpin->setDecimals(2);
    m_rowWorkEndpointArrivalSpin->setSingleStep(0.01);
    rowWorkParamsLayout->addRow(tr("端点到达阈值(m)"), m_rowWorkEndpointArrivalSpin);

    m_rowWorkCheckpointToleranceSpin = new QDoubleSpinBox(rowWorkParamsBox);
    m_rowWorkCheckpointToleranceSpin->setObjectName(QStringLiteral("mapRowWorkCheckpointToleranceSpin"));
    m_rowWorkCheckpointToleranceSpin->setRange(0.01, 5.0);
    m_rowWorkCheckpointToleranceSpin->setDecimals(2);
    m_rowWorkCheckpointToleranceSpin->setSingleStep(0.01);
    rowWorkParamsLayout->addRow(tr("中间点阈值(m)"), m_rowWorkCheckpointToleranceSpin);

    m_rowWorkTurnAngularSpeedSpin = new QDoubleSpinBox(rowWorkParamsBox);
    m_rowWorkTurnAngularSpeedSpin->setObjectName(QStringLiteral("mapRowWorkTurnAngularSpeedSpin"));
    m_rowWorkTurnAngularSpeedSpin->setRange(0.05, 5.0);
    m_rowWorkTurnAngularSpeedSpin->setDecimals(2);
    m_rowWorkTurnAngularSpeedSpin->setSingleStep(0.01);
    rowWorkParamsLayout->addRow(tr("掉头角速度(rad/s)"), m_rowWorkTurnAngularSpeedSpin);

    m_rowWorkLoopCheck = new QCheckBox(tr("循环往返"), rowWorkParamsBox);
    m_rowWorkLoopCheck->setObjectName(QStringLiteral("mapRowWorkLoopCheck"));
    rowWorkParamsLayout->addRow(QString(), m_rowWorkLoopCheck);

    primitiveLayout->addWidget(rowWorkParamsBox);

    auto rowWorkControlBox = new QGroupBox(tr("作业控制"), m_rowWorkPrimitivePage);
    rowWorkControlBox->setObjectName(QStringLiteral("mapRowWorkControlBox"));
    auto rowWorkControlLayout = new QVBoxLayout(rowWorkControlBox);
    rowWorkControlLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkControlLayout->setSpacing(6);

    auto rowWorkControlButtons1 = new QHBoxLayout();
    rowWorkControlButtons1->setSpacing(4);
    m_rowWorkReadPlanButton = new QPushButton(tr("回读计划"), rowWorkControlBox);
    m_rowWorkReadPlanButton->setObjectName(QStringLiteral("mapRowWorkReadPlanButton"));
    rowWorkControlButtons1->addWidget(m_rowWorkReadPlanButton);
    m_rowWorkUploadPlanButton = new QPushButton(tr("下发计划"), rowWorkControlBox);
    m_rowWorkUploadPlanButton->setObjectName(QStringLiteral("mapRowWorkUploadPlanButton"));
    rowWorkControlButtons1->addWidget(m_rowWorkUploadPlanButton);
    m_rowWorkStartButton = new QPushButton(tr("开始作业"), rowWorkControlBox);
    m_rowWorkStartButton->setObjectName(QStringLiteral("mapRowWorkStartButton"));
    rowWorkControlButtons1->addWidget(m_rowWorkStartButton);
    rowWorkControlLayout->addLayout(rowWorkControlButtons1);

    auto rowWorkControlButtons2 = new QHBoxLayout();
    rowWorkControlButtons2->setSpacing(4);
    m_rowWorkPauseButton = new QPushButton(tr("暂停"), rowWorkControlBox);
    m_rowWorkPauseButton->setObjectName(QStringLiteral("mapRowWorkPauseButton"));
    rowWorkControlButtons2->addWidget(m_rowWorkPauseButton);
    m_rowWorkResumeButton = new QPushButton(tr("恢复"), rowWorkControlBox);
    m_rowWorkResumeButton->setObjectName(QStringLiteral("mapRowWorkResumeButton"));
    rowWorkControlButtons2->addWidget(m_rowWorkResumeButton);
    m_rowWorkStopButton = new QPushButton(tr("停止"), rowWorkControlBox);
    m_rowWorkStopButton->setObjectName(QStringLiteral("mapRowWorkStopButton"));
    rowWorkControlButtons2->addWidget(m_rowWorkStopButton);
    rowWorkControlLayout->addLayout(rowWorkControlButtons2);

    primitiveLayout->addWidget(rowWorkControlBox);

    auto rowWorkStatusBox = new QGroupBox(tr("作业状态"), m_rowWorkPrimitivePage);
    rowWorkStatusBox->setObjectName(QStringLiteral("mapRowWorkStatusBox"));
    auto rowWorkStatusLayout = new QVBoxLayout(rowWorkStatusBox);
    rowWorkStatusLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkStatusLayout->setSpacing(6);

    m_rowWorkPlanLabel = new QLabel(tr("计划：未配置"), rowWorkStatusBox);
    m_rowWorkPlanLabel->setObjectName(QStringLiteral("mapRowWorkPlanLabel"));
    m_rowWorkPlanLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkPlanLabel);

    m_rowWorkRuntimeLabel = new QLabel(tr("运行：等待工控机状态"), rowWorkStatusBox);
    m_rowWorkRuntimeLabel->setObjectName(QStringLiteral("mapRowWorkRuntimeLabel"));
    m_rowWorkRuntimeLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkRuntimeLabel);

    m_rowWorkProgressLabel = new QLabel(tr("进度：-"), rowWorkStatusBox);
    m_rowWorkProgressLabel->setObjectName(QStringLiteral("mapRowWorkProgressLabel"));
    m_rowWorkProgressLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkProgressLabel);

    m_rowWorkTargetLabel = new QLabel(tr("目标：-"), rowWorkStatusBox);
    m_rowWorkTargetLabel->setObjectName(QStringLiteral("mapRowWorkTargetLabel"));
    m_rowWorkTargetLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkTargetLabel);

    m_rowWorkControlLabel = new QLabel(tr("自动执行：-"), rowWorkStatusBox);
    m_rowWorkControlLabel->setObjectName(QStringLiteral("mapRowWorkControlLabel"));
    m_rowWorkControlLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkControlLabel);

    m_rowWorkPoseLabel = new QLabel(tr("位姿：等待工控机状态"), rowWorkStatusBox);
    m_rowWorkPoseLabel->setObjectName(QStringLiteral("mapRowWorkPoseLabel"));
    m_rowWorkPoseLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkPoseLabel);

    m_rowWorkFaultLabel = new QLabel(tr("故障：无"), rowWorkStatusBox);
    m_rowWorkFaultLabel->setObjectName(QStringLiteral("mapRowWorkFaultLabel"));
    m_rowWorkFaultLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkFaultLabel);

    m_rowWorkEventLabel = new QLabel(tr("最近事件：-"), rowWorkStatusBox);
    m_rowWorkEventLabel->setObjectName(QStringLiteral("mapRowWorkEventLabel"));
    m_rowWorkEventLabel->setWordWrap(true);
    rowWorkStatusLayout->addWidget(m_rowWorkEventLabel);

    m_rowWorkStatusLabel = new QLabel(tr("待命"), rowWorkStatusBox);
    m_rowWorkStatusLabel->setObjectName(QStringLiteral("mapRowWorkStatusLabel"));
    m_rowWorkStatusLabel->setWordWrap(true);
    m_rowWorkStatusLabel->setMinimumHeight(40);
    QPalette rowWorkStatusPalette = m_rowWorkStatusLabel->palette();
    rowWorkStatusPalette.setColor(QPalette::WindowText, QColor(55, 55, 55));
    m_rowWorkStatusLabel->setPalette(rowWorkStatusPalette);
    rowWorkStatusLayout->addWidget(m_rowWorkStatusLabel);

    primitiveLayout->addWidget(rowWorkStatusBox);
    primitiveLayout->addStretch();

    rowWorkPrimitiveScrollArea->setWidget(m_rowWorkPrimitivePage);
    m_rowWorkModeTabWidget->addTab(rowWorkPrimitiveScrollArea, tr("单垄原语"));

    auto rowWorkMissionScrollArea = new QScrollArea(m_rowWorkModeTabWidget);
    rowWorkMissionScrollArea->setObjectName(QStringLiteral("mapRowWorkMissionScrollArea"));
    rowWorkMissionScrollArea->setWidgetResizable(true);
    rowWorkMissionScrollArea->setFrameShape(QFrame::NoFrame);

    m_rowWorkMissionPage = new QWidget(rowWorkMissionScrollArea);
    m_rowWorkMissionPage->setObjectName(QStringLiteral("mapRowWorkMissionPage"));
    auto missionLayout = new QVBoxLayout(m_rowWorkMissionPage);
    missionLayout->setContentsMargins(8, 8, 8, 8);
    missionLayout->setSpacing(8);

    auto missionMetaBox = new QGroupBox(tr("多垄任务概览"), m_rowWorkMissionPage);
    missionMetaBox->setObjectName(QStringLiteral("mapRowMissionMetaBox"));
    auto missionMetaLayout = new QFormLayout(missionMetaBox);
    missionMetaLayout->setContentsMargins(8, 8, 8, 8);
    missionMetaLayout->setSpacing(6);

    m_rowMissionIdLabel = new QLabel(tr("任务 ID：-"), missionMetaBox);
    m_rowMissionIdLabel->setObjectName(QStringLiteral("mapRowMissionIdLabel"));
    m_rowMissionIdLabel->setWordWrap(true);
    missionMetaLayout->addRow(tr("任务标识"), m_rowMissionIdLabel);

    m_rowMissionNameEdit = new QLineEdit(missionMetaBox);
    m_rowMissionNameEdit->setObjectName(QStringLiteral("mapRowMissionNameEdit"));
    missionMetaLayout->addRow(tr("任务名称"), m_rowMissionNameEdit);

    m_rowMissionLoopCheck = new QCheckBox(tr("循环执行整套任务"), missionMetaBox);
    m_rowMissionLoopCheck->setObjectName(QStringLiteral("mapRowMissionLoopCheck"));
    missionMetaLayout->addRow(QString(), m_rowMissionLoopCheck);

    m_rowMissionSummaryLabel = new QLabel(tr("步骤：-"), missionMetaBox);
    m_rowMissionSummaryLabel->setObjectName(QStringLiteral("mapRowMissionSummaryLabel"));
    m_rowMissionSummaryLabel->setWordWrap(true);
    missionMetaLayout->addRow(tr("概览"), m_rowMissionSummaryLabel);

    missionLayout->addWidget(missionMetaBox);

    auto missionActionBox = new QGroupBox(tr("任务步骤"), m_rowWorkMissionPage);
    missionActionBox->setObjectName(QStringLiteral("mapRowMissionActionBox"));
    auto missionActionLayout = new QVBoxLayout(missionActionBox);
    missionActionLayout->setContentsMargins(8, 8, 8, 8);
    missionActionLayout->setSpacing(6);

    auto missionAddButtons1 = new QHBoxLayout();
    missionAddButtons1->setSpacing(4);
    m_rowMissionAddRowLegButton = new QPushButton(tr("添加垄内段"), missionActionBox);
    m_rowMissionAddRowLegButton->setObjectName(QStringLiteral("mapRowMissionAddRowLegButton"));
    missionAddButtons1->addWidget(m_rowMissionAddRowLegButton);
    m_rowMissionAddTransferButton = new QPushButton(tr("添加转场段"), missionActionBox);
    m_rowMissionAddTransferButton->setObjectName(QStringLiteral("mapRowMissionAddTransferButton"));
    missionAddButtons1->addWidget(m_rowMissionAddTransferButton);
    m_rowMissionAddTurnButton = new QPushButton(tr("添加掉头"), missionActionBox);
    m_rowMissionAddTurnButton->setObjectName(QStringLiteral("mapRowMissionAddTurnButton"));
    missionAddButtons1->addWidget(m_rowMissionAddTurnButton);
    missionActionLayout->addLayout(missionAddButtons1);

    auto missionAddButtons2 = new QHBoxLayout();
    missionAddButtons2->setSpacing(4);
    m_rowMissionAddWaitButton = new QPushButton(tr("添加停留"), missionActionBox);
    m_rowMissionAddWaitButton->setObjectName(QStringLiteral("mapRowMissionAddWaitButton"));
    missionAddButtons2->addWidget(m_rowMissionAddWaitButton);
    m_rowMissionImportRowWorkButton = new QPushButton(tr("从当前单垄导入"), missionActionBox);
    m_rowMissionImportRowWorkButton->setObjectName(QStringLiteral("mapRowMissionImportRowWorkButton"));
    missionAddButtons2->addWidget(m_rowMissionImportRowWorkButton);
    missionAddButtons2->addStretch();
    missionActionLayout->addLayout(missionAddButtons2);

    m_rowMissionStepTable = new QTableWidget(missionActionBox);
    m_rowMissionStepTable->setObjectName(QStringLiteral("mapRowMissionStepTable"));
    m_rowMissionStepTable->setColumnCount(4);
    m_rowMissionStepTable->setHorizontalHeaderLabels(
        {tr("启用"), tr("名称"), tr("类型"), tr("摘要")});
    m_rowMissionStepTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_rowMissionStepTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_rowMissionStepTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_rowMissionStepTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_rowMissionStepTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_rowMissionStepTable->verticalHeader()->setVisible(false);
    m_rowMissionStepTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rowMissionStepTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rowMissionStepTable->setAlternatingRowColors(true);
    m_rowMissionStepTable->setMinimumHeight(220);
    missionActionLayout->addWidget(m_rowMissionStepTable);

    auto missionStepButtons = new QHBoxLayout();
    missionStepButtons->setSpacing(4);
    m_rowMissionRemoveStepButton = new QPushButton(tr("删除步骤"), missionActionBox);
    m_rowMissionRemoveStepButton->setObjectName(QStringLiteral("mapRowMissionRemoveStepButton"));
    missionStepButtons->addWidget(m_rowMissionRemoveStepButton);
    m_rowMissionMoveUpButton = new QPushButton(tr("上移"), missionActionBox);
    m_rowMissionMoveUpButton->setObjectName(QStringLiteral("mapRowMissionMoveUpButton"));
    missionStepButtons->addWidget(m_rowMissionMoveUpButton);
    m_rowMissionMoveDownButton = new QPushButton(tr("下移"), missionActionBox);
    m_rowMissionMoveDownButton->setObjectName(QStringLiteral("mapRowMissionMoveDownButton"));
    missionStepButtons->addWidget(m_rowMissionMoveDownButton);
    m_rowMissionClearButton = new QPushButton(tr("清空步骤"), missionActionBox);
    m_rowMissionClearButton->setObjectName(QStringLiteral("mapRowMissionClearButton"));
    missionStepButtons->addWidget(m_rowMissionClearButton);
    missionActionLayout->addLayout(missionStepButtons);

    missionLayout->addWidget(missionActionBox);

    auto missionEditorBox = new QGroupBox(tr("步骤编辑器"), m_rowWorkMissionPage);
    missionEditorBox->setObjectName(QStringLiteral("mapRowMissionEditorBox"));
    auto missionEditorLayout = new QFormLayout(missionEditorBox);
    missionEditorLayout->setContentsMargins(8, 8, 8, 8);
    missionEditorLayout->setSpacing(6);

    m_rowMissionStepTypeCombo = new QComboBox(missionEditorBox);
    m_rowMissionStepTypeCombo->setObjectName(QStringLiteral("mapRowMissionStepTypeCombo"));
    m_rowMissionStepTypeCombo->addItem(tr("垄内段"), static_cast<int>(RowMissionStepType::RowLeg));
    m_rowMissionStepTypeCombo->addItem(tr("转场段"), static_cast<int>(RowMissionStepType::Transfer));
    m_rowMissionStepTypeCombo->addItem(tr("掉头"), static_cast<int>(RowMissionStepType::Turn));
    m_rowMissionStepTypeCombo->addItem(tr("停留"), static_cast<int>(RowMissionStepType::Wait));
    missionEditorLayout->addRow(tr("步骤类型"), m_rowMissionStepTypeCombo);

    m_rowMissionStepNameEdit = new QLineEdit(missionEditorBox);
    m_rowMissionStepNameEdit->setObjectName(QStringLiteral("mapRowMissionStepNameEdit"));
    missionEditorLayout->addRow(tr("步骤名称"), m_rowMissionStepNameEdit);

    m_rowMissionStepNoteEdit = new QLineEdit(missionEditorBox);
    m_rowMissionStepNoteEdit->setObjectName(QStringLiteral("mapRowMissionStepNoteEdit"));
    missionEditorLayout->addRow(tr("备注"), m_rowMissionStepNoteEdit);

    m_rowMissionPrimitiveSummaryLabel = new QLabel(tr("原语：-"), missionEditorBox);
    m_rowMissionPrimitiveSummaryLabel->setObjectName(QStringLiteral("mapRowMissionPrimitiveSummaryLabel"));
    m_rowMissionPrimitiveSummaryLabel->setWordWrap(true);
    missionEditorLayout->addRow(tr("原语概览"), m_rowMissionPrimitiveSummaryLabel);

    m_rowMissionTargetYawSpin = new QDoubleSpinBox(missionEditorBox);
    m_rowMissionTargetYawSpin->setObjectName(QStringLiteral("mapRowMissionTargetYawSpin"));
    m_rowMissionTargetYawSpin->setRange(-360.0, 360.0);
    m_rowMissionTargetYawSpin->setDecimals(1);
    m_rowMissionTargetYawSpin->setSingleStep(1.0);
    m_rowMissionTargetYawSpin->setSuffix(tr("°"));
    missionEditorLayout->addRow(tr("目标航向"), m_rowMissionTargetYawSpin);

    m_rowMissionDwellSpin = new QSpinBox(missionEditorBox);
    m_rowMissionDwellSpin->setObjectName(QStringLiteral("mapRowMissionDwellSpin"));
    m_rowMissionDwellSpin->setRange(0, 3600000);
    m_rowMissionDwellSpin->setSingleStep(100);
    m_rowMissionDwellSpin->setSuffix(tr(" ms"));
    missionEditorLayout->addRow(tr("停留时间"), m_rowMissionDwellSpin);

    m_rowMissionApplyStepButton = new QPushButton(tr("应用步骤修改"), missionEditorBox);
    m_rowMissionApplyStepButton->setObjectName(QStringLiteral("mapRowMissionApplyStepButton"));
    missionEditorLayout->addRow(QString(), m_rowMissionApplyStepButton);

    missionLayout->addWidget(missionEditorBox);

    m_rowMissionStatusLabel = new QLabel(tr("待命"), m_rowWorkMissionPage);
    m_rowMissionStatusLabel->setObjectName(QStringLiteral("mapRowMissionStatusLabel"));
    m_rowMissionStatusLabel->setWordWrap(true);
    m_rowMissionStatusLabel->setMinimumHeight(40);
    QPalette missionStatusPalette = m_rowMissionStatusLabel->palette();
    missionStatusPalette.setColor(QPalette::WindowText, QColor(55, 55, 55));
    m_rowMissionStatusLabel->setPalette(missionStatusPalette);
    missionLayout->addWidget(m_rowMissionStatusLabel);
    missionLayout->addStretch();

    rowWorkMissionScrollArea->setWidget(m_rowWorkMissionPage);
    auto* uploadMission = new QPushButton(tr("上传完整多垄任务"), m_rowWorkMissionPage);
    connect(uploadMission, &QPushButton::clicked, this, [this] { uploadMissionTask(); });
    missionLayout->addWidget(uploadMission);
    m_rowWorkModeTabWidget->addTab(rowWorkMissionScrollArea, tr("多垄任务"));

    rowWorkLayout->addWidget(m_rowWorkModeTabWidget, 1);

    taskTabWidget->addTab(routePage, tr("常规任务"));
    taskTabWidget->addTab(rowWorkPage, tr("直线作业"));
    rightLayout->addWidget(taskTabWidget, 1);

    mainSplitter->addWidget(rightPanel);

    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);
    mainSplitter->setCollapsible(0, true);
    mainSplitter->setCollapsible(2, true);
    mainSplitter->setSizes({280, 1100, 340});
    connect(m_buildGridButton, &QPushButton::clicked, this, &Map::handleGenerateMap);
    connect(m_addPointFromInputButton, &QPushButton::clicked, this, &Map::handleAddPointFromInput);
    connect(m_addPointFromClickButton, &QPushButton::clicked, this, &Map::handleAddPointFromClickRequested);
    connect(m_updatePointButton, &QPushButton::clicked, this, &Map::handleUpdatePoint);
    connect(m_removePointButton, &QPushButton::clicked, this, &Map::handleRemoveSelectedPoints);
    connect(m_batchGenerateButton, &QPushButton::clicked, this, &Map::handleBatchGeneratePoints);
    connect(m_pointTable, &QTableWidget::itemSelectionChanged, this, &Map::handlePointSelectionChanged);
    connect(m_pathTypeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_arcSagittaSpin) {
            m_arcSagittaSpin->setEnabled(static_cast<PathType>(index) == PathType::Arc);
        }
    });
    connect(m_addPathButton, &QPushButton::clicked, this, &Map::handleAddPath);
    connect(m_removePathButton, &QPushButton::clicked, this, &Map::handleRemoveSelectedPaths);
    connect(m_routeAddButton, &QPushButton::clicked, this, &Map::handleRouteAdd);
    connect(m_routeRemoveButton, &QPushButton::clicked, this, &Map::handleRouteRemove);
    connect(m_routeClearButton, &QPushButton::clicked, this, &Map::handleRouteClear);
    connect(m_routeStartButton, &QPushButton::clicked, this, &Map::handleRouteStart);
    connect(m_routePauseButton, &QPushButton::clicked, this, &Map::handleRoutePause);
    connect(m_routeResumeButton, &QPushButton::clicked, this, &Map::handleRouteResume);
    connect(m_routeStopButton, &QPushButton::clicked, this, &Map::handleRouteStop);
    connect(m_routeLoopCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_routeLoopIntervalSpin) {
            m_routeLoopIntervalSpin->setEnabled(checked);
        }
    });
    connect(m_locateButton, &QPushButton::clicked, this, &Map::handleLocateCurrentPosition);
    connect(m_editModeButton, &QPushButton::toggled, this, &Map::handleEditModeToggled);
    connect(m_newMapButton, &QPushButton::clicked, this, &Map::handleNewMap);
    connect(m_view, &MapGraphicsView::scenePointClicked, this, &Map::handleSceneClick);
    connect(m_view, &MapGraphicsView::mouseMovedOnScene, this, &Map::handleSceneMouseMoved);
    connect(m_loadMapButton, &QPushButton::clicked, this, &Map::handleLoadMap);
    connect(m_saveMapButton, &QPushButton::clicked, this, &Map::handleSaveMap);
    connect(m_quickSaveButton, &QPushButton::clicked, this, &Map::handleQuickSave);
    if (m_rotationSlider) {
        connect(m_rotationSlider, &QSlider::valueChanged, this, &Map::handleRotationSliderValueChanged);
    }
    if (m_gridRotationSpin) {
        connect(m_gridRotationSpin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &Map::handleRotationSpinChanged);
    }
    connect(m_rowWorkCaptureStartButton, &QPushButton::clicked, this, &Map::handleRowWorkCaptureStartPoint);
    connect(m_rowWorkCaptureEndButton, &QPushButton::clicked, this, &Map::handleRowWorkCaptureEndPoint);
    connect(m_rowWorkClearLineButton, &QPushButton::clicked, this, &Map::handleRowWorkClearPlan);
    connect(m_rowWorkAddCheckpointFromVehicleButton, &QPushButton::clicked, this, &Map::handleRowWorkAddCheckpointFromVehicle);
    connect(m_rowWorkAddCheckpointFromMapButton, &QPushButton::clicked, this, &Map::handleRowWorkAddCheckpointFromMap);
    connect(m_rowWorkDeleteCheckpointButton, &QPushButton::clicked, this, &Map::handleRowWorkDeleteCheckpoint);
    connect(m_rowWorkClearCheckpointsButton, &QPushButton::clicked, this, &Map::handleRowWorkClearCheckpoints);
    connect(m_rowWorkReadPlanButton, &QPushButton::clicked, this, &Map::handleRowWorkReadPlan);
    connect(m_rowWorkUploadPlanButton, &QPushButton::clicked, this, &Map::handleRowWorkUploadPlan);
    connect(m_rowWorkStartButton, &QPushButton::clicked, this, &Map::handleRowWorkStart);
    connect(m_rowWorkPauseButton, &QPushButton::clicked, this, &Map::handleRowWorkPause);
    connect(m_rowWorkResumeButton, &QPushButton::clicked, this, &Map::handleRowWorkResume);
    connect(m_rowWorkStopButton, &QPushButton::clicked, this, &Map::handleRowWorkStop);
    connect(m_rowWorkBaseSpeedSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canEditRowWorkPlan()) { refreshRowWorkUi(); return; }
        m_rowWorkPlan.params.baseLinearSpeed = value; updateRowWorkPlanVersion();
    });
    connect(m_rowWorkMaxSpeedSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canEditRowWorkPlan()) { refreshRowWorkUi(); return; }
        m_rowWorkPlan.params.maxLinearSpeed = value; updateRowWorkPlanVersion();
    });
    connect(m_rowWorkEndpointArrivalSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canEditRowWorkPlan()) { refreshRowWorkUi(); return; }
        m_rowWorkPlan.params.endpointArrivalDistance = value; updateRowWorkPlanVersion();
    });
    connect(m_rowWorkCheckpointToleranceSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canEditRowWorkPlan()) { refreshRowWorkUi(); return; }
        m_rowWorkPlan.params.checkpointArrivalTolerance = value; updateRowWorkPlanVersion();
    });
    connect(m_rowWorkTurnAngularSpeedSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!canEditRowWorkPlan()) { refreshRowWorkUi(); return; }
        m_rowWorkPlan.params.turnAngularSpeed = value; updateRowWorkPlanVersion();
    });
    connect(m_rowWorkLoopCheck, &QCheckBox::toggled, this, &Map::handleRowWorkLoopChanged);
    connect(m_rowWorkCheckpointTable, &QTableWidget::cellChanged, this, &Map::handleRowWorkCheckpointCellChanged);
    connect(m_rowMissionNameEdit, &QLineEdit::editingFinished, this, &Map::handleRowMissionNameEdited);
    connect(m_rowMissionLoopCheck, &QCheckBox::toggled, this, &Map::handleRowMissionLoopChanged);
    connect(m_rowMissionStepTable, &QTableWidget::itemSelectionChanged, this, &Map::handleRowMissionStepSelectionChanged);
    connect(m_rowMissionStepTable, &QTableWidget::cellChanged, this, &Map::handleRowMissionStepCellChanged);
    connect(m_rowMissionStepTypeCombo, &QComboBox::currentIndexChanged, this, &Map::handleRowMissionTypeChanged);
    connect(m_rowMissionStepNameEdit, &QLineEdit::editingFinished, this, &Map::handleRowMissionStepNameEdited);
    connect(m_rowMissionStepNoteEdit, &QLineEdit::editingFinished, this, &Map::handleRowMissionStepNoteEdited);
    connect(m_rowMissionTargetYawSpin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, &Map::handleRowMissionTargetYawChanged);
    connect(m_rowMissionDwellSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Map::handleRowMissionDwellChanged);
    connect(m_rowMissionAddRowLegButton, &QPushButton::clicked, this, &Map::handleRowMissionAddRowLegStep);
    connect(m_rowMissionAddTransferButton, &QPushButton::clicked, this, &Map::handleRowMissionAddTransferStep);
    connect(m_rowMissionAddTurnButton, &QPushButton::clicked, this, &Map::handleRowMissionAddTurnStep);
    connect(m_rowMissionAddWaitButton, &QPushButton::clicked, this, &Map::handleRowMissionAddWaitStep);
    connect(m_rowMissionRemoveStepButton, &QPushButton::clicked, this, &Map::handleRowMissionRemoveStep);
    connect(m_rowMissionMoveUpButton, &QPushButton::clicked, this, &Map::handleRowMissionMoveStepUp);
    connect(m_rowMissionMoveDownButton, &QPushButton::clicked, this, &Map::handleRowMissionMoveStepDown);
    connect(m_rowMissionClearButton, &QPushButton::clicked, this, &Map::handleRowMissionClearSteps);
    connect(m_rowMissionImportRowWorkButton, &QPushButton::clicked, this, &Map::handleRowMissionImportCurrentRowWork);
    connect(m_rowMissionApplyStepButton, &QPushButton::clicked, this, &Map::handleRowMissionApplyStepEdits);

    handleEditModeToggled(m_editModeButton && m_editModeButton->isChecked());


    refreshRowWorkUi();
}
void Map::ensureScene()
{
    if (m_scene) {
        return;
    }

    m_scene = new QGraphicsScene(this);
    m_scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    m_scene->setBackgroundBrush(QColor(248, 248, 248));

    if (m_view) {
        m_view->setScene(m_scene);
    }
}

void Map::rebuildGrid()
{
    if (!m_scene) {
        ensureScene();
    }

    clearGrid();

    const double halfWidthMeters = (m_gridWidth * m_cellSizeMeters) / 2.0;
    const double halfHeightMeters = (m_gridHeight * m_cellSizeMeters) / 2.0;

    const QPointF corners[] = {
        mapToScene(QPointF(-halfWidthMeters, -halfHeightMeters)),
        mapToScene(QPointF(-halfWidthMeters, halfHeightMeters)),
        mapToScene(QPointF(halfWidthMeters, -halfHeightMeters)),
        mapToScene(QPointF(halfWidthMeters, halfHeightMeters))
    };

    double minX = corners[0].x();
    double maxX = corners[0].x();
    double minY = corners[0].y();
    double maxY = corners[0].y();
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, corners[i].x());
        maxX = std::max(maxX, corners[i].x());
        minY = std::min(minY, corners[i].y());
        maxY = std::max(maxY, corners[i].y());
    }

    QRectF sceneRect(QPointF(minX, minY), QPointF(maxX, maxY));
    sceneRect = sceneRect.normalized();
    const double margin = m_cellSizePixels * 2.0;
    sceneRect.adjust(-margin, -margin, margin, margin);
    m_scene->setSceneRect(sceneRect);

    m_gridGroup = new QGraphicsItemGroup();
    m_scene->addItem(m_gridGroup);
    m_gridGroup->setZValue(-10.0);

    QPen gridPen(QColor(224, 224, 224));
    gridPen.setWidthF(0.0);
    QPen axisPen(QColor(176, 176, 176));
    axisPen.setWidthF(0.0);

    const int verticalLines = m_gridWidth + 1;
    for (int i = 0; i < verticalLines; ++i) {
        const double xMeters = -halfWidthMeters + i * m_cellSizeMeters;
        const QPointF startScene = mapToScene(QPointF(xMeters, -halfHeightMeters));
        const QPointF endScene = mapToScene(QPointF(xMeters, halfHeightMeters));
        auto line = m_scene->addLine(startScene.x(), startScene.y(), endScene.x(), endScene.y(), gridPen);
        if (std::abs(xMeters) < kGeometryEpsilon) {
            line->setPen(axisPen);
        }
        m_gridGroup->addToGroup(line);
    }

    const int horizontalLines = m_gridHeight + 1;
    for (int j = 0; j < horizontalLines; ++j) {
        const double yMeters = -halfHeightMeters + j * m_cellSizeMeters;
        const QPointF startScene = mapToScene(QPointF(-halfWidthMeters, yMeters));
        const QPointF endScene = mapToScene(QPointF(halfWidthMeters, yMeters));
        auto line = m_scene->addLine(startScene.x(), startScene.y(), endScene.x(), endScene.y(), gridPen);
        if (std::abs(yMeters) < kGeometryEpsilon) {
            line->setPen(axisPen);
        }
        m_gridGroup->addToGroup(line);
    }

    auto axisXStart = mapToScene(QPointF(-halfWidthMeters, 0.0));
    auto axisXEnd = mapToScene(QPointF(halfWidthMeters, 0.0));
    auto axisYStart = mapToScene(QPointF(0.0, -halfHeightMeters));
    auto axisYEnd = mapToScene(QPointF(0.0, halfHeightMeters));
    auto axisX = m_scene->addLine(axisXStart.x(), axisXStart.y(), axisXEnd.x(), axisXEnd.y(), axisPen);
    auto axisY = m_scene->addLine(axisYStart.x(), axisYStart.y(), axisYEnd.x(), axisYEnd.y(), axisPen);
    m_gridGroup->addToGroup(axisX);
    m_gridGroup->addToGroup(axisY);

    const double originRadius = 4.0;
    m_originItem = m_scene->addEllipse(-originRadius, -originRadius, originRadius * 2.0, originRadius * 2.0,
                                       QPen(QColor(200, 0, 0)), QBrush(QColor(200, 0, 0)));
    m_originItem->setPos(mapToScene(QPointF(0.0, 0.0)));
    m_originItem->setZValue(-2.0);
}

void Map::clearGrid()
{
    if (m_gridGroup) {
        delete m_gridGroup;
        m_gridGroup = nullptr;
    }
    if (m_originItem) {
        delete m_originItem;
        m_originItem = nullptr;
    }
}

void Map::ensureVehicleItem()
{
    if (!m_scene || m_vehicleItem) {
        return;
    }

    m_vehicleItem = new QGraphicsItemGroup();
    m_vehicleItem->setZValue(20.0);
    m_vehicleItem->setVisible(false);
    m_scene->addItem(m_vehicleItem);

    const double scale = m_cellSizePixels / m_cellSizeMeters;
    const double bodyLength = kVehicleLengthMeters * scale;
    const double bodyWidth = kVehicleWidthMeters * scale;
    const double halfLength = bodyLength / 2.0;
    const double halfWidth = bodyWidth / 2.0;

    m_vehicleBody = new QGraphicsRectItem(-halfLength, -halfWidth, bodyLength, bodyWidth, m_vehicleItem);
    m_vehicleBody->setPen(QPen(QColor(33, 150, 243), 0.8));
    m_vehicleBody->setBrush(QColor(33, 150, 243, 80));

    m_vehiclePoseMarker = new QGraphicsItemGroup(m_vehicleItem);
    m_vehiclePoseMarker->setZValue(10.0);
    m_vehiclePoseMarker->setPos(0.0, 0.0);

    m_vehiclePoseCircle = new QGraphicsEllipseItem(-kPointMarkerRadiusPx, -kPointMarkerRadiusPx,
                                                   kPointMarkerRadiusPx * 2.0, kPointMarkerRadiusPx * 2.0,
                                                   m_vehiclePoseMarker);
    m_vehiclePoseCircle->setBrush(Qt::NoBrush);
    QPen posePen(Qt::black);
    posePen.setWidthF(1.5);
    m_vehiclePoseCircle->setPen(posePen);

    m_vehiclePoseArrow = new QGraphicsPathItem(makePointArrowPath(), m_vehiclePoseMarker);
    m_vehiclePoseArrow->setBrush(QColor(0, 0, 0));
    m_vehiclePoseArrow->setPen(Qt::NoPen);
}

int Map::allocatePointId()
{
    return m_nextPointId++;
}

int Map::allocatePathId()
{
    return m_nextPathId++;
}

void Map::resetNextPointId()
{
    int maxId = 0;
    for (auto it = m_points.cbegin(); it != m_points.cend(); ++it) {
        maxId = std::max(maxId, it.key());
    }
    m_nextPointId = maxId + 1;
}

void Map::resetNextPathId()
{
    int maxId = 0;
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        maxId = std::max(maxId, it.key());
    }
    m_nextPathId = maxId + 1;
}

bool Map::addPointInternal(double x, double y, double theta, std::optional<int> forcedId)
{
    if (!m_scene) {
        ensureScene();
    }

    const int pointId = forcedId.value_or(allocatePointId());
    if (m_points.contains(pointId)) {
        return false;
    }

    m_nextPointId = std::max(m_nextPointId, pointId + 1);

    MapPoint point;
    point.id = pointId;
    point.mapPosition = QPointF(x, y);
    point.theta = normalizeAngle(theta);

    point.markerGroup = new QGraphicsItemGroup();
    point.markerGroup->setZValue(5.0);
    point.markerGroup->setFlag(QGraphicsItem::ItemIsSelectable, true);
    m_scene->addItem(point.markerGroup);

    point.circle = new QGraphicsEllipseItem(-kPointMarkerRadiusPx, -kPointMarkerRadiusPx,
                                            kPointMarkerRadiusPx * 2.0, kPointMarkerRadiusPx * 2.0,
                                            point.markerGroup);
    point.circle->setBrush(QColor(220, 20, 60));
    point.circle->setPen(QPen(QColor(139, 0, 0)));

    point.arrow = new QGraphicsPathItem(point.markerGroup);
    point.arrow->setPen(Qt::NoPen);
    point.arrow->setBrush(QColor(0, 0, 0));

    point.label = new QGraphicsTextItem(QString::number(point.id), point.markerGroup);
    point.label->setDefaultTextColor(QColor(45, 45, 45));

    updatePointGraphics(point);

    m_points.insert(point.id, point);

    return true;
}

void Map::removePointInternal(int pointId)
{
    auto it = m_points.find(pointId);
    if (it == m_points.end()) {
        return;
    }

    MapPoint point = it.value();
    if (point.markerGroup) {
        delete point.markerGroup;
    }
    m_points.erase(it);

    m_outgoingPathIds.remove(pointId);

    QList<int> affectedPaths;
    for (auto pit = m_paths.cbegin(); pit != m_paths.cend(); ++pit) {
        if (pit.value().startId == pointId || pit.value().endId == pointId) {
            affectedPaths.append(pit.key());
        }
    }
    for (int pathId : affectedPaths) {
        removePathInternal(pathId);
    }

    if (m_vehicleCurrentPointId && m_vehicleCurrentPointId.value() == pointId) {
        m_vehicleCurrentPointId.reset();
    }
}

int Map::addPathOrWarn(int startId, int endId, PathType type, double sagitta, std::optional<int> forcedId)
{
    const int existingId = findPathId(startId, endId);
    if (existingId != -1 && !forcedId.has_value()) {
        if (MapPath *existingPath = pathById(existingId)) {
            PathType targetType = type;
            double targetSagitta = sagitta;
            if (targetType == PathType::Arc && std::abs(sagitta) < kArcSagittaEpsilon) {
                targetType = PathType::Line;
                targetSagitta = 0.0;
            }
            if (targetType == PathType::Line) {
                targetSagitta = 0.0;
            }

            existingPath->type = targetType;
            existingPath->sagitta = targetSagitta;
            refreshPathGeometry(*existingPath);

            const int reverseId = findPathId(endId, startId);
            if (reverseId != -1) {
                if (MapPath *reversePath = pathById(reverseId)) {
                    refreshPathGeometry(*reversePath);
                }
            }

            refreshPathUi();
            refreshSelectors();
            setRouteStatusText(tr("更新路径: 从 %1 到 %2").arg(startId).arg(endId));
            return existingId;
        }
    }

    bool success = false;
    if (type == PathType::Arc) {
        success = addArcPath(startId, endId, sagitta, forcedId);
    } else {
        success = addLinePath(startId, endId, forcedId);
    }
    if (!success) {
        QMessageBox::warning(m_mapPage, tr("添加路径"), tr("无法创建该路径，可能已存在"));
        return -1;
    }
    int createdId = -1;
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        if (it.value().startId == startId && it.value().endId == endId) {
            createdId = it.key();
            break;
        }
    }
    refreshPathUi();
    refreshSelectors();
    setRouteStatusText(tr("已添加路径: 点%1 → 点%2").arg(startId).arg(endId));
    return createdId;
}

bool Map::addLinePath(int startId, int endId, std::optional<int> forcedId)
{
    const MapPoint *startPoint = pointById(startId);
    const MapPoint *endPoint = pointById(endId);
    if (!startPoint || !endPoint || startId == endId) {
        return false;
    }

    MapPath path;
    path.id = forcedId.value_or(-1);
    path.startId = startId;
    path.endId = endId;
    path.type = PathType::Line;
    path.polyline = {startPoint->mapPosition, endPoint->mapPosition};
    path.sagitta = 0.0;

    return addPathInternal(std::move(path));
}

bool Map::addArcPath(int startId, int endId, double sagitta, std::optional<int> forcedId)
{
    const MapPoint *startPoint = pointById(startId);
    const MapPoint *endPoint = pointById(endId);
    if (!startPoint || !endPoint || startId == endId) {
        return false;
    }

    if (std::abs(sagitta) < kArcSagittaEpsilon) {
        return addLinePath(startId, endId, forcedId);
    }

    MapPath path;
    path.id = forcedId.value_or(-1);
    path.startId = startId;
    path.endId = endId;
    path.type = PathType::Arc;
    path.sagitta = sagitta;

    return addPathInternal(std::move(path));
}
bool Map::addPathInternal(MapPath &&path)
{
    if (!pointById(path.startId) || !pointById(path.endId)) {
        return false;
    }

    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        if (it.value().startId == path.startId && it.value().endId == path.endId) {
            return false;
        }
    }

    if (!m_scene) {
        ensureScene();
    }

    if (path.id < 0) {
        path.id = allocatePathId();
    }
    m_nextPathId = std::max(m_nextPathId, path.id + 1);

    if (!path.pathItem) {
        path.pathItem = new QGraphicsPathItem();
        path.pathItem->setZValue(-4.0);
        m_scene->addItem(path.pathItem);
    }
    path.pathItem->setVisible(m_pathsVisible);

    MapPath &stored = m_paths[path.id];
    if (stored.pathItem && stored.pathItem != path.pathItem) {
        delete stored.pathItem;
    }
    stored = path;

    refreshPathGeometry(stored);

    const int reverseId = findPathId(path.endId, path.startId);
    if (reverseId != -1) {
        if (MapPath *reversePath = pathById(reverseId)) {
            refreshPathGeometry(*reversePath);
        }
    }

    QList<int> &outgoing = m_outgoingPathIds[path.startId];
    if (!outgoing.contains(path.id)) {
        outgoing.append(path.id);
    }

    autoSelectRoutePoints(path.startId, path.endId);

    return true;
}

void Map::removePathInternal(int pathId)
{
    auto it = m_paths.find(pathId);
    if (it == m_paths.end()) {
        return;
    }

    MapPath path = it.value();
    const int reverseId = findPathId(path.endId, path.startId);
    if (path.pathItem) {
        delete path.pathItem;
    }
    m_paths.erase(it);

    QList<int> &outgoing = m_outgoingPathIds[path.startId];
    outgoing.removeAll(pathId);
    if (outgoing.isEmpty()) {
        m_outgoingPathIds.remove(path.startId);
    }

    for (int i = m_routeQueue.size() - 1; i >= 0; --i) {
        RouteStep &step = m_routeQueue[i];
        if (step.pathIds.contains(pathId)) {
            if (!rebuildRouteStep(step)) {
                m_routeQueue.removeAt(i);
            }
        }
    }

    if (reverseId != -1) {
        if (MapPath *reversePath = pathById(reverseId)) {
            refreshPathGeometry(*reversePath);
        }
    }
}

void Map::refreshPathGeometry(MapPath &path)
{
    const MapPoint *startPoint = pointById(path.startId);
    const MapPoint *endPoint = pointById(path.endId);
    if (!startPoint || !endPoint || !path.pathItem) {
        return;
    }

    QList<QPointF> polyline;

    if (path.type == PathType::Line || std::abs(path.sagitta) < kArcSagittaEpsilon) {
        polyline = {startPoint->mapPosition, endPoint->mapPosition};
        path.polyline = polyline;
        path.arcCenter = QPointF();
        path.radius = 0.0;
        path.sweepAngleRad = 0.0;
    } else {
        const QPointF aStd = mapToStandardPoint(startPoint->mapPosition);
        const QPointF bStd = mapToStandardPoint(endPoint->mapPosition);
        const double dx = bStd.x() - aStd.x();
        const double dy = bStd.y() - aStd.y();
        const double chord = std::hypot(dx, dy);
        if (chord < kGeometryEpsilon) {
            polyline = {startPoint->mapPosition, endPoint->mapPosition};
            path.polyline = polyline;
            path.arcCenter = QPointF();
            path.radius = 0.0;
            path.sweepAngleRad = 0.0;
        } else {
            const double sagitta = path.sagitta;
            const double absSagitta = std::abs(sagitta);
            const double radius = (chord * chord) / (8.0 * absSagitta) + absSagitta / 2.0;
            const double sign = (sagitta >= 0.0) ? 1.0 : -1.0;
            const QPointF tangent(dx / chord, dy / chord);
            const QPointF normal(-tangent.y(), tangent.x());
            const QPointF mid((aStd.x() + bStd.x()) / 2.0, (aStd.y() + bStd.y()) / 2.0);
            const double offset = radius - absSagitta;
            const QPointF centerStd(mid.x() + normal.x() * offset * sign,
                                    mid.y() + normal.y() * offset * sign);

            double startAngle = std::atan2(aStd.y() - centerStd.y(), aStd.x() - centerStd.x());
            double endAngle = std::atan2(bStd.y() - centerStd.y(), bStd.x() - centerStd.x());
            double sweep = endAngle - startAngle;
            if (sign > 0.0) {
                if (sweep <= 0.0) {
                    sweep += 2.0 * M_PI;
                }
            } else {
                if (sweep >= 0.0) {
                    sweep -= 2.0 * M_PI;
                }
            }

            const int segments = std::max(kMinArcSegments,
                                          static_cast<int>(std::ceil(std::abs(sweep) / (M_PI / 24.0))));
            polyline.clear();
            for (int i = 0; i <= segments; ++i) {
                const double t = static_cast<double>(i) / segments;
                const double angle = startAngle + sweep * t;
                const QPointF stdPoint(centerStd.x() + std::cos(angle) * radius,
                                       centerStd.y() + std::sin(angle) * radius);
                polyline.append(standardToMapPoint(stdPoint));
            }

            path.polyline = polyline;
            path.arcCenter = standardToMapPoint(centerStd);
            path.radius = radius;
            path.sweepAngleRad = sweep;
        }
    }

    if (polyline.size() < 2) {
        polyline = {startPoint->mapPosition, endPoint->mapPosition};
        path.polyline = polyline;
    }

    QPainterPath scenePath;
    bool first = true;
    for (const QPointF &point : std::as_const(polyline)) {
        const QPointF scenePoint = mapToScene(point);
        if (first) {
            scenePath.moveTo(scenePoint);
            first = false;
        } else {
            scenePath.lineTo(scenePoint);
        }
    }

    const bool hasReverse = (findPathId(path.endId, path.startId) != -1);
    const QColor singleColor(220, 90, 0);
    const QColor bidirectionalColor(0, 180, 120);
    QPen pen(hasReverse ? bidirectionalColor : singleColor);
    pen.setWidthF(path.type == PathType::Line ? 2.0 : 2.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    path.pathItem->setPath(scenePath);
    path.pathItem->setPen(pen);
    path.pathItem->setVisible(m_pathsVisible);

}

void Map::updatePointGraphics(MapPoint &point)
{
    if (!point.markerGroup) {
        return;
    }

    const QPointF scenePos = mapToScene(point.mapPosition);
    point.markerGroup->setPos(scenePos);

    if (point.arrow) {
        point.arrow->setPath(makePointArrowPath());
        point.arrow->setRotation(qRadiansToDegrees(mapHeadingToSceneAngle(point.theta)));
    }

    if (point.label) {
        point.label->setPlainText(QString::number(point.id));
        const QRectF rect = point.label->boundingRect();
        point.label->setPos(-rect.width() / 2.0, -rect.height() - 10.0);
    }
}

void Map::refreshAllPointGraphics()
{
    for (auto it = m_points.begin(); it != m_points.end(); ++it) {
        updatePointGraphics(it.value());
    }
}

void Map::refreshAllPathGeometry()
{
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
        refreshPathGeometry(it.value());
    }
}

void Map::applyPresentationUpdates(bool recenterView)
{
    rebuildGrid();
    refreshAllPointGraphics();
    refreshAllPathGeometry();
    refreshVehicleGraphics();
    if (m_view) {
        const double currentZoom = m_view->zoomFactor();
        const QGraphicsView::ViewportAnchor previousAnchor = m_view->transformationAnchor();
        m_view->setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        m_view->resetTransform();
        m_view->scale(currentZoom, currentZoom);
        m_view->rotate(m_mapRotationDeg);
        m_view->setTransformationAnchor(previousAnchor);
        if (recenterView) {
            m_view->centerOn(QPointF(0.0, 0.0));
        }
    }
}

void Map::refreshVehicleGraphics()
{
    if (!m_vehicleItem) {
        ensureVehicleItem();
    }
    if (!m_vehicleItem) {
        return;
    }

    m_vehicleItem->setVisible(m_hasVehiclePose);
    if (!m_hasVehiclePose) {
        return;
    }

    m_vehicleItem->setPos(mapToScene(QPointF(m_vehiclePoseX, m_vehiclePoseY)));
    m_vehicleItem->setRotation(qRadiansToDegrees(mapHeadingToSceneAngle(m_vehiclePoseTheta)));
}

void Map::refreshPathsForPoint(int pointId)
{
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
        if (it.value().startId == pointId || it.value().endId == pointId) {
            refreshPathGeometry(it.value());
        }
    }
}
void Map::autoSelectRoutePoints(int startId, int endId)
{
    if (m_startPointCombo) {
        const int index = m_startPointCombo->findData(startId);
        if (index >= 0) {
            m_startPointCombo->setCurrentIndex(index);
        }
    }
    if (m_endPointCombo) {
        const int index = m_endPointCombo->findData(endId);
        if (index >= 0) {
            m_endPointCombo->setCurrentIndex(index);
        }
    }
    if (m_routeStartCombo) {
        const int index = m_routeStartCombo->findData(startId);
        if (index >= 0) {
            m_routeStartCombo->setCurrentIndex(index);
        }
    }
    if (m_routeEndCombo) {
        const int index = m_routeEndCombo->findData(endId);
        if (index >= 0) {
            m_routeEndCombo->setCurrentIndex(index);
        }
    }
}

QList<int> Map::findRoutePathIds(int startId, int endId) const
{
    QList<int> result;
    if (startId == endId) {
        return result;
    }
    if (!m_points.contains(startId) || !m_points.contains(endId)) {
        return result;
    }

    QList<MapRoutePlanner::Edge> edges;
    edges.reserve(m_paths.size());
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        const MapPath &path = it.value();
        if (path.startId < 0 || path.endId < 0) {
            continue;
        }
        MapRoutePlanner::Edge edge;
        edge.pathId = path.id;
        edge.fromId = path.startId;
        edge.toId = path.endId;
        edge.lengthMeters = polylineLength(path.polyline);
        edge.isArc = (path.type == PathType::Arc);
        edges.append(edge);
    }

    return MapRoutePlanner::findPathIds(startId, endId, edges, ConfigManager::instance().routePlanning());
}

QList<QPointF> Map::composePolyline(const QList<int> &pathIds) const
{
    QList<QPointF> polyline;
    for (int pathId : pathIds) {
        const MapPath *path = pathById(pathId);
        if (!path) {
            continue;
        }
        const QList<QPointF> &segment = path->polyline;
        if (segment.isEmpty()) {
            continue;
        }
        if (polyline.isEmpty()) {
            polyline.append(segment);
        } else {
            if (pointsAlmostEqual(polyline.last(), segment.first())) {
                for (int i = 1; i < segment.size(); ++i) {
                    polyline.append(segment.at(i));
                }
            } else {
                polyline.append(segment);
            }
        }
    }
    return polyline;
}

double Map::angleFromMapVector(double dx, double dy) const
{
    if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy)) {
        return 0.0;
    }
    const QPointF stdVec = mapToStandardPoint(QPointF(dx, dy));
    const double stdAngle = std::atan2(stdVec.y(), stdVec.x());
    return normalizeAngle(standardToMapAngle(stdAngle));
}

int Map::findNearestPointId(const QPointF &mapPos, double thresholdMeters, double *outDistance) const
{
    int nearestId = -1;
    double bestDist = thresholdMeters;
    for (auto it = m_points.cbegin(); it != m_points.cend(); ++it) {
        const MapPoint &point = it.value();
        const double dx = point.mapPosition.x() - mapPos.x();
        const double dy = point.mapPosition.y() - mapPos.y();
        const double dist = std::hypot(dx, dy);
        if (dist < bestDist) {
            bestDist = dist;
            nearestId = point.id;
        }
    }
    if (outDistance) {
        *outDistance = bestDist;
    }
    return nearestId;
}

int Map::findPathId(int startId, int endId) const
{
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        if (it.value().startId == startId && it.value().endId == endId) {
            return it.key();
        }
    }
    return -1;
}

void Map::updatePointTableRow(int pointId)
{
    if (!m_pointTable) {
        return;
    }
    const MapPoint *point = pointById(pointId);
    if (!point) {
        return;
    }
    const QList<QTableWidgetItem *> matches =
        m_pointTable->findItems(QString::number(pointId), Qt::MatchExactly);
    if (matches.isEmpty()) {
        return;
    }
    const int row = matches.first()->row();
    QSignalBlocker blocker(m_pointTable);
    if (auto xItem = m_pointTable->item(row, 1)) {
        xItem->setText(formatNumber(point->mapPosition.x()));
        xItem->setTextAlignment(Qt::AlignCenter);
    }
    if (auto yItem = m_pointTable->item(row, 2)) {
        yItem->setText(formatNumber(point->mapPosition.y()));
        yItem->setTextAlignment(Qt::AlignCenter);
    }
    if (auto thetaItem = m_pointTable->item(row, 3)) {
        thetaItem->setText(formatNumber(point->theta));
        thetaItem->setTextAlignment(Qt::AlignCenter);
    }
}

QList<int> Map::buildPointSequenceFromPaths(const QList<int> &pathIds, int startId) const
{
    QList<int> sequence;
    sequence.append(startId);
    int current = startId;
    for (int pathId : pathIds) {
        const MapPath *path = pathById(pathId);
        if (!path || path->startId != current) {
            sequence.clear();
            return sequence;
        }
        sequence.append(path->endId);
        current = path->endId;
    }
    return sequence;
}

bool Map::rebuildRouteStep(RouteStep &step)
{
    QList<int> pathIds = findRoutePathIds(step.fromId, step.toId);
    if (pathIds.isEmpty()) {
        step.pathIds.clear();
        step.pointSequence.clear();
        step.progressEdgeIndex = 0;
        return false;
    }
    step.pathIds = pathIds;
    step.pointSequence = buildPointSequenceFromPaths(pathIds, step.fromId);
    step.progressEdgeIndex = 0;
    return true;
}




void Map::refreshPointUi()
{
    if (!m_pointTable) {
        return;
    }

    const std::optional<int> selectedId = m_pendingPointSelection;
    QSignalBlocker blocker(m_pointTable);
    QList<int> ids = m_points.keys();
    std::sort(ids.begin(), ids.end());

    m_pointTable->setRowCount(ids.size());
    int row = 0;
    for (int id : ids) {
        const MapPoint &point = m_points.value(id);
        auto idItem = new QTableWidgetItem(QString::number(id));
        idItem->setFlags(idItem->flags() ^ Qt::ItemIsEditable);
        idItem->setTextAlignment(Qt::AlignCenter);
        m_pointTable->setItem(row, 0, idItem);

        auto xItem = new QTableWidgetItem(formatNumber(point.mapPosition.x()));
        xItem->setFlags(xItem->flags() ^ Qt::ItemIsEditable);
        xItem->setTextAlignment(Qt::AlignCenter);
        m_pointTable->setItem(row, 1, xItem);

        auto yItem = new QTableWidgetItem(formatNumber(point.mapPosition.y()));
        yItem->setFlags(yItem->flags() ^ Qt::ItemIsEditable);
        yItem->setTextAlignment(Qt::AlignCenter);
        m_pointTable->setItem(row, 2, yItem);

        auto thetaItem = new QTableWidgetItem(formatNumber(point.theta));
        thetaItem->setFlags(thetaItem->flags() ^ Qt::ItemIsEditable);
        thetaItem->setTextAlignment(Qt::AlignCenter);
        m_pointTable->setItem(row, 3, thetaItem);
        ++row;
    }

    m_pointTable->resizeRowsToContents();

    if (selectedId.has_value()) {
        const QList<QTableWidgetItem *> items =
            m_pointTable->findItems(QString::number(selectedId.value()), Qt::MatchExactly);
        if (!items.isEmpty()) {
            const int selectedRow = items.first()->row();
            m_pointTable->setCurrentCell(selectedRow, 0,
                                         QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
}

void Map::refreshPathUi()
{
    if (!m_pathTable) {
        return;
    }

    QSignalBlocker blocker(m_pathTable);
    QList<int> ids = m_paths.keys();
    std::sort(ids.begin(), ids.end());

    m_pathTable->setRowCount(ids.size());
    int row = 0;
    for (int id : ids) {
        const MapPath &path = m_paths.value(id);
        auto idItem = new QTableWidgetItem(QString::number(id));
        idItem->setFlags(idItem->flags() ^ Qt::ItemIsEditable);
        idItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 0, idItem);

        auto startItem = new QTableWidgetItem(QString::number(path.startId));
        startItem->setFlags(startItem->flags() ^ Qt::ItemIsEditable);
        startItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 1, startItem);

        auto endItem = new QTableWidgetItem(QString::number(path.endId));
        endItem->setFlags(endItem->flags() ^ Qt::ItemIsEditable);
        endItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 2, endItem);

        auto typeItem = new QTableWidgetItem(path.type == PathType::Line ? tr("直线") : tr("圆弧"));
        typeItem->setFlags(typeItem->flags() ^ Qt::ItemIsEditable);
        typeItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 3, typeItem);

        auto sagittaItem = new QTableWidgetItem(formatNumber(path.sagitta));
        sagittaItem->setFlags(sagittaItem->flags() ^ Qt::ItemIsEditable);
        sagittaItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 4, sagittaItem);

        auto lengthItem = new QTableWidgetItem(formatNumber(polylineLength(path.polyline)));
        lengthItem->setFlags(lengthItem->flags() ^ Qt::ItemIsEditable);
        lengthItem->setTextAlignment(Qt::AlignCenter);
        m_pathTable->setItem(row, 5, lengthItem);
        ++row;
    }

    m_pathTable->horizontalHeader()->resizeSections(QHeaderView::Stretch);
    m_pathTable->resizeRowsToContents();
}

void Map::refreshSelectors()
{
    QList<int> ids = m_points.keys();
    std::sort(ids.begin(), ids.end());

    auto updateCombo = [&ids](QComboBox *combo, const QString &placeholder) {
        if (!combo) {
            return;
        }
        const QVariant currentData = combo->currentData();
        QSignalBlocker blocker(combo);
        combo->clear();
        if (!placeholder.isEmpty()) {
            combo->addItem(placeholder, QVariant());
        }
        for (int id : ids) {
            combo->addItem(QObject::tr("点 %1").arg(id), id);
        }
        const int currentIndex = combo->findData(currentData);
        if (currentIndex >= 0) {
            combo->setCurrentIndex(currentIndex);
        } else if (!ids.isEmpty()) {
            combo->setCurrentIndex(placeholder.isEmpty() ? 0 : 1);
        }
    };

    updateCombo(m_startPointCombo, tr("选择起点"));
    updateCombo(m_endPointCombo, tr("选择终点"));
    updateCombo(m_routeStartCombo, tr("选择起点"));
    updateCombo(m_routeEndCombo, tr("选择终点"));

    if (m_batchBasePointCombo) {
        const QVariant currentData = m_batchBasePointCombo->currentData();
        QSignalBlocker blocker(m_batchBasePointCombo);
        m_batchBasePointCombo->clear();
        m_batchBasePointCombo->addItem(tr("原点 (0,0)"), 0);
        for (int id : ids) {
            m_batchBasePointCombo->addItem(tr("点 %1").arg(id), id);
        }
        const int currentIndex = m_batchBasePointCombo->findData(currentData);
        if (currentIndex >= 0) {
            m_batchBasePointCombo->setCurrentIndex(currentIndex);
        } else {
            m_batchBasePointCombo->setCurrentIndex(0);
        }
    }
}

void Map::refreshRouteQueueUi()
{
    if (!m_routeQueueList) {
        return;
    }

    QSignalBlocker blocker(m_routeQueueList);
    m_routeQueueList->clear();
    for (int i = 0; i < m_routeQueue.size(); ++i) {
        const RouteStep &step = m_routeQueue.at(i);
        const QString text = tr("%1. 点%2 → 点%3 (%4段)")
                                 .arg(i + 1)
                                 .arg(step.fromId)
                                 .arg(step.toId)
                                 .arg(step.pathIds.size());
        m_routeQueueList->addItem(text);
    }
}
void Map::updateRouteControlState()
{
    const bool editable = canEditRowWorkPlan();
    const bool online = m_tracking && m_tracking->fresh();
    const auto state = online ? m_tracking->snapshot() : TrackingSnapshot{};
    if (m_routeStartButton) { m_routeStartButton->setText(tr("上传整条路线")); m_routeStartButton->setEnabled(editable && !m_routeQueue.isEmpty()); }
    if (m_routePauseButton) m_routePauseButton->setEnabled(online && state.isExecuting());
    if (m_routeResumeButton) m_routeResumeButton->setEnabled(online && state.state == "Paused");
    if (m_routeStopButton) m_routeStopButton->setEnabled(online && !state.executionId.isEmpty() && !state.isTerminal());
    if (m_routeRemoveButton) m_routeRemoveButton->setEnabled(editable && !m_routeQueue.isEmpty());
    if (m_routeClearButton) m_routeClearButton->setEnabled(editable && !m_routeQueue.isEmpty());
}

void Map::setRouteStatusText(const QString &text, bool warning)
{
    if (!m_routeStatusLabel) {
        return;
    }
    m_routeStatusLabel->setText(text);
    QPalette palette = m_routeStatusLabel->palette();
    palette.setColor(QPalette::WindowText, warning ? QColor(220, 80, 60) : QColor(55, 55, 55));
    m_routeStatusLabel->setPalette(palette);
}

QPointF Map::sceneToMap(const QPointF &scenePoint) const
{
    const double scale = m_cellSizeMeters / m_cellSizePixels;
    return QPointF(scenePoint.y() * scale, scenePoint.x() * scale);
}

QPointF Map::mapToScene(const QPointF &mapPoint) const
{
    const double scale = m_cellSizePixels / m_cellSizeMeters;
    return QPointF(mapPoint.y() * scale, mapPoint.x() * scale);
}

void Map::setMapRotation(double rotationDeg)
{
    m_mapRotationDeg = normalizedRotationDeg(rotationDeg);
    if (m_gridRotationSpin) {
        QSignalBlocker blocker(m_gridRotationSpin);
        m_gridRotationSpin->setValue(m_mapRotationDeg);
    }
    if (m_rotationSlider) {
        QSignalBlocker blocker(m_rotationSlider);
        m_rotationSlider->setValue(static_cast<int>(std::round(m_mapRotationDeg * 10.0)));
    }
}

double Map::normalizedRotationDeg(double rotationDeg) const
{
    double normalized = std::fmod(rotationDeg, 360.0);
    if (normalized > 180.0) {
        normalized -= 360.0;
    }
    if (normalized <= -180.0) {
        normalized += 360.0;
    }
    if (std::abs(normalized) < 1e-6) {
        normalized = 0.0;
    }
    return normalized;
}

double Map::mapHeadingToSceneAngle(double headingRad) const
{
    return normalizeAngle(mapToStandardAngle(headingRad));
}

Map::MapPath *Map::pathById(int id)
{
    auto it = m_paths.find(id);
    return it == m_paths.end() ? nullptr : &it.value();
}

const Map::MapPath *Map::pathById(int id) const
{
    auto it = m_paths.find(id);
    return it == m_paths.end() ? nullptr : &it.value();
}

Map::MapPoint *Map::pointById(int id)
{
    auto it = m_points.find(id);
    return it == m_points.end() ? nullptr : &it.value();
}

const Map::MapPoint *Map::pointById(int id) const
{
    auto it = m_points.find(id);
    return it == m_points.end() ? nullptr : &it.value();
}

void Map::updateMapNameDisplay()
{
    if (!m_mapNameLabel) {
        return;
    }
    if (m_mousePositionLabel) {
        m_mousePositionLabel->setText(tr("X: 0.000  Y: 0.000"));
    }
    if (m_currentMapFilePath.isEmpty()) {
        m_mapNameLabel->setText(tr("未保存地图"));
    } else {
        m_mapNameLabel->setText(tr("地图: %1").arg(QFileInfo(m_currentMapFilePath).fileName()));
    }
}

void Map::updateVehiclePointBinding()
{
    if (!m_hasVehiclePose) {
        m_vehicleCurrentPointId.reset();
        return;
    }

    double bestDist = kVehicleSnapThresholdMeters;
    std::optional<int> bestId;
    for (auto it = m_points.cbegin(); it != m_points.cend(); ++it) {
        const MapPoint &point = it.value();
        const double dx = point.mapPosition.x() - m_vehiclePoseX;
        const double dy = point.mapPosition.y() - m_vehiclePoseY;
        const double dist = std::hypot(dx, dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestId = point.id;
        }
    }

    m_vehicleCurrentPointId = bestId;

    if (bestId.has_value() && m_routeStartCombo) {
        const int index = m_routeStartCombo->findData(bestId.value());
        if (index >= 0) {
            m_routeStartCombo->setCurrentIndex(index);
        }
    }
}



bool Map::isRouteQueueContinuous() const
{
    if (m_routeQueue.isEmpty()) {
        return true;
    }
    for (int i = 1; i < m_routeQueue.size(); ++i) {
        if (m_routeQueue.at(i - 1).toId != m_routeQueue.at(i).fromId) {
            return false;
        }
    }
    return true;
}


void Map::resetToBlankMap()
{
    m_binding = {};
    if (m_coordinator) m_coordinator->setBinding(m_binding);
    clearMapData();

    m_gridWidth = kDefaultGridWidth;
    m_gridHeight = kDefaultGridHeight;
    m_cellSizeMeters = kCellSizeMeters;
    m_nextPointId = 1;
    m_nextPathId = 1;
    m_currentMapFilePath.clear();

    if (m_gridWidthSpin) {
        QSignalBlocker blocker(m_gridWidthSpin);
        m_gridWidthSpin->setValue(m_gridWidth);
    }
    if (m_gridHeightSpin) {
        QSignalBlocker blocker(m_gridHeightSpin);
        m_gridHeightSpin->setValue(m_gridHeight);
    }

    setMapRotation(0.0);

    m_waitingForClickPlacement = false;
    m_rowWorkClickPlacementMode = false;
    m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    m_pendingPointSelection.reset();
    if (m_addPointFromClickButton) {
        QSignalBlocker blocker(m_addPointFromClickButton);
        m_addPointFromClickButton->setChecked(false);
    }
    if (m_rowWorkAddCheckpointFromMapButton) {
        QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
        m_rowWorkAddCheckpointFromMapButton->setChecked(false);
    }

    m_rowWorkPlan = RowWorkPlan{};
    m_rowWorkPlan.frameBinding = m_binding;
    m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    m_rowWorkPlan.frameId = QStringLiteral("map");
    m_rowWorkPlan.params.loopEnabled = true;
    m_rowMissionPlan = RowMissionPlan{};
    m_rowMissionPlan.frameBinding = m_binding;
    m_rowMissionPlan.missionId = RowMissionJson::generateMissionId();
    m_rowMissionPlan.frameId = QStringLiteral("map");

    applyPresentationUpdates(true);
    updateMapNameDisplay();
    updateVehiclePointBinding();
    refreshRowWorkUi();
    syncCommittedMapState();
    setRouteStatusText(tr("已新建空白地图"));
}

bool Map::saveCurrentMapInteractive(bool forceChooseFile)
{
    QString filePath = m_currentMapFilePath;
    const bool saveAs = forceChooseFile || filePath.isEmpty();
    if (forceChooseFile || filePath.isEmpty()) {
        const QString startDir = !m_lastSaveDirectory.isEmpty() ? m_lastSaveDirectory
                                  : (!m_currentMapFilePath.isEmpty() ? QFileInfo(m_currentMapFilePath).absolutePath()
                                     : (!s_lastMapFilePath.isEmpty() ? QFileInfo(s_lastMapFilePath).absolutePath()
                                                                     : QDir::currentPath()));

        filePath = QFileDialog::getSaveFileName(m_mapPage, tr("保存地图"), startDir,
                                                tr("地图文件 (*.json);;所有文件 (*.*)"));
        if (filePath.isEmpty()) {
            LoggingManager::audit(QStringLiteral("map.save"),
                                  QStringLiteral("cancelled"),
                                  {{QStringLiteral("mode"), saveAs ? QStringLiteral("save_as") : QStringLiteral("save")}});
            return false;
        }
    }

    if (!saveMapToFile(filePath)) {
        LoggingManager::audit(QStringLiteral("map.save"),
                              QStringLiteral("failed"),
                              {{QStringLiteral("mode"), saveAs ? QStringLiteral("save_as") : QStringLiteral("save")},
                               {QStringLiteral("path"), filePath}});
        return false;
    }

    m_currentMapFilePath = filePath;
    s_lastMapFilePath = filePath;
    m_lastSaveDirectory = QFileInfo(filePath).absolutePath();
    updateMapNameDisplay();
    syncCommittedMapState();
    LoggingManager::audit(QStringLiteral("map.save"),
                          QStringLiteral("success"),
                          {{QStringLiteral("mode"), saveAs ? QStringLiteral("save_as") : QStringLiteral("save")},
                           {QStringLiteral("path"), filePath},
                           {QStringLiteral("points"), QString::number(m_points.size())},
                           {QStringLiteral("paths"), QString::number(m_paths.size())}});
    return true;
}

QByteArray Map::buildComparableMapState() const
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kMapSchemaVersion);
    root.insert("frameBinding", m_binding.toJson());
    root.insert(QStringLiteral("gridWidth"), m_gridWidth);
    root.insert(QStringLiteral("gridHeight"), m_gridHeight);
    root.insert(QStringLiteral("cellSizeMeters"), m_cellSizeMeters);
    root.insert(QStringLiteral("rotationDeg"), m_mapRotationDeg);

    QJsonArray pointsArray;
    QList<int> pointIds = m_points.keys();
    std::sort(pointIds.begin(), pointIds.end());
    for (int pointId : pointIds) {
        const MapPoint *point = pointById(pointId);
        if (!point) {
            continue;
        }
        QJsonObject pointObj;
        pointObj.insert(QStringLiteral("id"), point->id);
        pointObj.insert(QStringLiteral("x"), point->mapPosition.x());
        pointObj.insert(QStringLiteral("y"), point->mapPosition.y());
        pointObj.insert(QStringLiteral("theta"), point->theta);
        pointsArray.append(pointObj);
    }
    root.insert(QStringLiteral("points"), pointsArray);

    QJsonArray pathsArray;
    QList<int> pathIds = m_paths.keys();
    std::sort(pathIds.begin(), pathIds.end());
    for (int pathId : pathIds) {
        const MapPath *path = pathById(pathId);
        if (!path) {
            continue;
        }
        QJsonObject pathObj;
        pathObj.insert(QStringLiteral("id"), path->id);
        pathObj.insert(QStringLiteral("start"), path->startId);
        pathObj.insert(QStringLiteral("end"), path->endId);
        pathObj.insert(QStringLiteral("type"), path->type == PathType::Arc ? QStringLiteral("arc")
                                                                           : QStringLiteral("line"));
        pathObj.insert(QStringLiteral("sagitta"), path->sagitta);
        pathsArray.append(pathObj);
    }
    root.insert(QStringLiteral("paths"), pathsArray);

    if (m_rowWorkPlan.isValid() || !m_rowWorkPlan.checkpoints.isEmpty()) {
        root.insert(QStringLiteral("rowWork"), RowWorkJson::planToJson(m_rowWorkPlan));
    }

    if (m_rowMissionPlan.isValid()) {
        root.insert(QStringLiteral("rowMission"), RowMissionJson::planToJson(m_rowMissionPlan));
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void Map::syncCommittedMapState()
{
    m_committedMapState = buildComparableMapState();
}

void Map::refreshPlanningRevision()
{
    // Exclude embedded bindings: advancing the revision must not itself count as another edit.
    std::function<QJsonValue(const QJsonValue&)> stripBindings = [&](const QJsonValue& value) -> QJsonValue {
        if (value.isArray()) { QJsonArray out; for (const auto& child : value.toArray()) out.append(stripBindings(child)); return out; }
        if (!value.isObject()) return value;
        auto out = value.toObject(); out.remove("frameBinding");
        for (auto it = out.begin(); it != out.end(); ++it) it.value() = stripBindings(it.value());
        return out;
    };
    auto content = stripBindings(QJsonDocument::fromJson(buildComparableMapState()).object()).toObject();
    QJsonArray route;
    for (const auto& step : m_routeQueue) route.append(QJsonArray{step.fromId, step.toId});
    content["routeQueue"] = route;
    content["routeLoop"] = m_routeLoopCheck && m_routeLoopCheck->isChecked();
    content["routeInterval"] = m_routeLoopIntervalSpin ? m_routeLoopIntervalSpin->value() : 0;
    const auto fingerprint = QJsonDocument(content).toJson(QJsonDocument::Compact);
    if (m_planningFingerprint == fingerprint) return;
    const bool changed = !m_planningFingerprint.isEmpty();
    m_planningFingerprint = fingerprint;
    if (!changed) return;
    ++m_binding.context.mapRevision;
    if (m_coordinator) m_coordinator->setBinding(m_binding);
}

bool Map::hasUnsavedMapChanges() const
{
    return buildComparableMapState() != m_committedMapState;
}

bool Map::confirmClose()
{
    if (!hasUnsavedMapChanges()) return true;
    const auto answer = QMessageBox::question(m_mapPage, tr("保存地图"), tr("是否保存地图修改后退出？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && saveCurrentMapInteractive(false));
}

void Map::clearMapData()
{
    clearRowWorkGraphics();

    while (!m_paths.isEmpty()) {
        removePathInternal(m_paths.begin().key());
    }

    while (!m_points.isEmpty()) {
        removePointInternal(m_points.begin().key());
    }

    m_points.clear();
    m_paths.clear();
    m_outgoingPathIds.clear();
    m_vehicleCurrentPointId.reset();

    m_routeQueue.clear();

    m_rowWorkPlan = RowWorkPlan{};
    m_rowWorkPlan.frameBinding = m_binding;
    m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    m_rowWorkPlan.frameId = QStringLiteral("map");
    m_rowWorkPlan.params.loopEnabled = true;
    m_rowMissionPlan = RowMissionPlan{};
    m_rowMissionPlan.frameBinding = m_binding;
    m_rowMissionPlan.missionId = RowMissionJson::generateMissionId();
    m_rowMissionPlan.frameId = QStringLiteral("map");
    m_rowWorkCheckpointTableUpdating = false;
    m_rowWorkClickPlacementMode = false;
    m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;

    refreshPointUi();
    refreshPathUi();
    refreshSelectors();
    refreshRouteQueueUi();
    updateRouteControlState();
    refreshRowWorkUi();
}

bool Map::saveMapToFile(const QString &filePath)
{
    refreshPlanningRevision();
    if (filePath.isEmpty()) {
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(m_mapPage, tr("保存地图"), tr("无法写入文件: %1").arg(file.errorString()));
        return false;
    }

    QJsonDocument doc(serializeMap());
    const QByteArray payload = doc.toJson(QJsonDocument::Indented);
    const qint64 written = file.write(payload);
    if (written != payload.size()) {
        file.cancelWriting();
        QMessageBox::warning(m_mapPage, tr("保存地图"), tr("写入失败: %1").arg(file.errorString()));
        return false;
    }
    if (!file.commit()) {
        QMessageBox::warning(m_mapPage, tr("保存地图"), tr("提交文件失败: %1").arg(file.errorString()));
        return false;
    }
    return true;
}

bool Map::loadMapFromFile(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(m_mapPage, tr("加载地图"), tr("无法打开文件: %1").arg(file.errorString()));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        QMessageBox::warning(m_mapPage, tr("加载地图"), tr("解析失败: %1").arg(parseError.errorString()));
        return false;
    }
    if (!doc.isObject()) {
        QMessageBox::warning(m_mapPage, tr("加载地图"), tr("文件格式不正确"));
        return false;
    }

    if (!deserializeMap(doc.object())) {
        QMessageBox::warning(m_mapPage, tr("加载地图"), tr("文件内容无效"));
        return false;
    }

    m_currentMapFilePath = filePath;
    s_lastMapFilePath = filePath;
    updateMapNameDisplay();
    syncCommittedMapState();
    return true;
}

QJsonObject Map::serializeMap() const
{
    MapDocument doc;
    doc.schemaVersion = kMapSchemaVersion;
    doc.frameBinding = m_binding;
    doc.savedAtIsoUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    doc.gridWidth = m_gridWidth;
    doc.gridHeight = m_gridHeight;
    doc.cellSizeMeters = m_cellSizeMeters;
    doc.rotationDeg = m_mapRotationDeg;

    doc.points.reserve(m_points.size());
    for (auto it = m_points.cbegin(); it != m_points.cend(); ++it) {
        const MapPoint &point = it.value();
        MapDocumentPoint record;
        record.id = point.id;
        record.x = point.mapPosition.x();
        record.y = point.mapPosition.y();
        record.theta = point.theta;
        doc.points.append(record);
    }

    doc.paths.reserve(m_paths.size());
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        const MapPath &path = it.value();
        MapDocumentPath record;
        record.id = path.id;
        record.startId = path.startId;
        record.endId = path.endId;
        record.type = (path.type == PathType::Arc) ? MapDocumentPathType::Arc : MapDocumentPathType::Line;
        record.sagitta = path.sagitta;
        doc.paths.append(record);
    }

    if (m_rowWorkPlan.isValid() || !m_rowWorkPlan.checkpoints.isEmpty()) {
        doc.hasRowWorkPlan = true;
        doc.rowWorkPlan = m_rowWorkPlan;
    }

    if (m_rowMissionPlan.isValid()) {
        doc.hasRowMissionPlan = true;
        doc.rowMissionPlan = m_rowMissionPlan;
    }

    return MapDocumentCodec::toJson(doc);
}

bool Map::deserializeMap(const QJsonObject &object)
{
    MapDocument doc;
    QString decodeError;
    if (!MapDocumentCodec::fromJson(object, &doc, &decodeError, kMapSchemaVersion)) {
        qCWarning(lcMapModule) << "Map deserialize failed:" << decodeError;
        return false;
    }

    m_binding = doc.frameBinding;
    if (m_coordinator) m_coordinator->setBinding(m_binding);
    if (m_gridWidthSpin) {
        m_gridWidthSpin->setValue(doc.gridWidth);
    }
    if (m_gridHeightSpin) {
        m_gridHeightSpin->setValue(doc.gridHeight);
    }
    m_gridWidth = doc.gridWidth;
    m_gridHeight = doc.gridHeight;
    m_cellSizeMeters = doc.cellSizeMeters;

    setMapRotation(doc.rotationDeg);

    clearMapData();

    int maxPointId = 0;
    for (const MapDocumentPoint &point : doc.points) {
        if (addPointInternal(point.x, point.y, point.theta, point.id)) {
            maxPointId = std::max(maxPointId, point.id);
        }
    }

    int maxPathId = 0;
    for (const MapDocumentPath &path : doc.paths) {
        bool ok = false;
        if (path.type == MapDocumentPathType::Arc) {
            ok = addArcPath(path.startId, path.endId, path.sagitta, path.id);
        } else {
            ok = addLinePath(path.startId, path.endId, path.id);
        }
        if (ok) {
            maxPathId = std::max(maxPathId, path.id);
        }
    }

    m_nextPointId = maxPointId + 1;
    m_nextPathId = maxPathId + 1;

    if (doc.hasRowWorkPlan) {
        m_rowWorkPlan = doc.rowWorkPlan;
    } else {
        m_rowWorkPlan = RowWorkPlan{};
    m_rowWorkPlan.frameBinding = m_binding;
        m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
        m_rowWorkPlan.frameId = QStringLiteral("map");
        m_rowWorkPlan.params.loopEnabled = true;
    }
    if (doc.hasRowMissionPlan) {
        m_rowMissionPlan = doc.rowMissionPlan;
    } else {
        m_rowMissionPlan = RowMissionPlan{};
    m_rowMissionPlan.frameBinding = m_binding;
        m_rowMissionPlan.missionId = RowMissionJson::generateMissionId();
        m_rowMissionPlan.frameId = QStringLiteral("map");
    }
    m_rowWorkClickPlacementMode = false;
    m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    if (m_rowWorkAddCheckpointFromMapButton) {
        QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
        m_rowWorkAddCheckpointFromMapButton->setChecked(false);
    }

    applyPresentationUpdates(true);
    refreshPointUi();
    refreshPathUi();
    refreshSelectors();
    refreshRouteQueueUi();
    updateRouteControlState();
    refreshRowWorkUi();

    return true;
}




void Map::refreshRowWorkUi()
{
    refreshRowWorkPlanSummary();
    refreshRowWorkCheckpointTable();
    refreshRowWorkControlState();
    refreshRowWorkGraphics();
    refreshRowMissionUi();
}

void Map::refreshRowWorkPlanSummary()
{
    if (m_rowWorkStartLabel) {
        m_rowWorkStartLabel->setText(m_rowWorkPlan.hasStartPose
                                         ? tr("X=%1, Y=%2, Yaw=%3")
                                               .arg(m_rowWorkPlan.startPose.x, 0, 'f', 2)
                                               .arg(m_rowWorkPlan.startPose.y, 0, 'f', 2)
                                               .arg(m_rowWorkPlan.startPose.yaw, 0, 'f', 2)
                                         : tr("未记录"));
    }
    if (m_rowWorkEndLabel) {
        m_rowWorkEndLabel->setText(m_rowWorkPlan.hasEndPose
                                       ? tr("X=%1, Y=%2, Yaw=%3")
                                             .arg(m_rowWorkPlan.endPose.x, 0, 'f', 2)
                                             .arg(m_rowWorkPlan.endPose.y, 0, 'f', 2)
                                             .arg(m_rowWorkPlan.endPose.yaw, 0, 'f', 2)
                                       : tr("未记录"));
    }
    if (m_rowWorkBaseSpeedSpin) {
        QSignalBlocker blocker(m_rowWorkBaseSpeedSpin);
        m_rowWorkBaseSpeedSpin->setValue(m_rowWorkPlan.params.baseLinearSpeed);
    }
    if (m_rowWorkMaxSpeedSpin) {
        QSignalBlocker blocker(m_rowWorkMaxSpeedSpin);
        m_rowWorkMaxSpeedSpin->setValue(m_rowWorkPlan.params.maxLinearSpeed);
    }
    if (m_rowWorkEndpointArrivalSpin) {
        QSignalBlocker blocker(m_rowWorkEndpointArrivalSpin);
        m_rowWorkEndpointArrivalSpin->setValue(m_rowWorkPlan.params.endpointArrivalDistance);
    }
    if (m_rowWorkCheckpointToleranceSpin) {
        QSignalBlocker blocker(m_rowWorkCheckpointToleranceSpin);
        m_rowWorkCheckpointToleranceSpin->setValue(m_rowWorkPlan.params.checkpointArrivalTolerance);
    }
    if (m_rowWorkTurnAngularSpeedSpin) {
        QSignalBlocker blocker(m_rowWorkTurnAngularSpeedSpin);
        m_rowWorkTurnAngularSpeedSpin->setValue(m_rowWorkPlan.params.turnAngularSpeed);
    }
    if (m_rowWorkLoopCheck) {
        QSignalBlocker blocker(m_rowWorkLoopCheck);
        m_rowWorkLoopCheck->setChecked(m_rowWorkPlan.params.loopEnabled);
    }
    if (m_rowWorkPlanLabel) {
        const QString lineText = hasRowWorkLine()
                                     ? tr("计划：长度 %1 m，中间点 %2 个，版本 %3，循环=%4")
                                           .arg(m_rowWorkPlan.lineLength(), 0, 'f', 2)
                                           .arg(m_rowWorkPlan.checkpoints.size())
                                           .arg(m_rowWorkPlan.version)
                                           .arg(m_rowWorkPlan.params.loopEnabled ? tr("开") : tr("关"))
                                     : tr("计划：未完成 A/B 示教");
        m_rowWorkPlanLabel->setText(lineText);
    }
    const bool online = m_tracking && m_tracking->fresh();
    const auto status = online ? m_tracking->snapshot() : TrackingSnapshot{};
    if (m_rowWorkRuntimeLabel) m_rowWorkRuntimeLabel->setText(tr("状态：%1").arg(online ? status.state : tr("未知")));
    if (m_rowWorkProgressLabel) m_rowWorkProgressLabel->setText(tr("步骤 %1：%2 / %3 m，剩余转角 %4 rad，等待 %5 ms")
        .arg(status.stepId).arg(status.progressMeters, 0, 'f', 2).arg(status.lengthMeters, 0, 'f', 2)
        .arg(status.remainingAngleRad, 0, 'f', 2).arg(status.remainingWaitMs, 0, 'f', 0));
    if (m_rowWorkTargetLabel) m_rowWorkTargetLabel->setText(tr("执行 %1 · 循环 %2").arg(status.executionId).arg(status.loopIndex));
    if (m_rowWorkControlLabel) m_rowWorkControlLabel->setText(tr("工控机独立执行"));
    if (m_rowWorkPoseLabel) m_rowWorkPoseLabel->setText(m_poseClient && m_poseClient->fresh() ? tr("定位有效") : tr("定位过期或无效"));
    if (m_rowWorkFaultLabel) m_rowWorkFaultLabel->setText(tr("暂停原因：%1 · 故障：%2").arg(status.pauseReason, status.faultCode));
    if (m_rowWorkEventLabel) m_rowWorkEventLabel->setText(tr("等待事件：%1 · 结果：%2").arg(status.waitingEventId, status.result));
}

void Map::refreshRowWorkCheckpointTable()
{
    if (!m_rowWorkCheckpointTable) {
        return;
    }

    m_rowWorkCheckpointTableUpdating = true;
    QSignalBlocker blocker(m_rowWorkCheckpointTable);
    m_rowWorkCheckpointTable->setRowCount(m_rowWorkPlan.checkpoints.size());
    for (int row = 0; row < m_rowWorkPlan.checkpoints.size(); ++row) {
        const RowCheckpoint &checkpoint = m_rowWorkPlan.checkpoints.at(row);

        auto *nameItem = new QTableWidgetItem(checkpoint.name.trimmed());
        nameItem->setTextAlignment(Qt::AlignCenter);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableNameColumn, nameItem);

        auto *progressItem = new QTableWidgetItem(QString::number(checkpoint.progress, 'f', 2));
        progressItem->setTextAlignment(Qt::AlignCenter);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableProgressColumn, progressItem);

        auto *dwellItem = new QTableWidgetItem(QString::number(checkpoint.dwellMs));
        dwellItem->setTextAlignment(Qt::AlignCenter);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableDwellColumn, dwellItem);

        auto *forwardItem = new QTableWidgetItem();
        forwardItem->setFlags((forwardItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                              & ~Qt::ItemIsEditable);
        forwardItem->setCheckState(checkpoint.triggerOnForward ? Qt::Checked : Qt::Unchecked);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableForwardColumn, forwardItem);

        auto *backwardItem = new QTableWidgetItem();
        backwardItem->setFlags((backwardItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                               & ~Qt::ItemIsEditable);
        backwardItem->setCheckState(checkpoint.triggerOnBackward ? Qt::Checked : Qt::Unchecked);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableBackwardColumn, backwardItem);
        auto* captureItem = new QTableWidgetItem;
        captureItem->setFlags((captureItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        captureItem->setCheckState(checkpoint.capturePhoto ? Qt::Checked : Qt::Unchecked);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableCaptureColumn, captureItem);
        m_rowWorkCheckpointTable->setItem(row, kRowWorkCheckpointTableTimeoutColumn,
            new QTableWidgetItem(QString::number(checkpoint.actionTimeoutMs)));
    }
    m_rowWorkCheckpointTable->resizeRowsToContents();
    m_rowWorkCheckpointTableUpdating = false;
}

void Map::refreshRowWorkControlState()
{
    const bool clientReady = m_tracking && m_tracking->fresh();
    const bool busy = m_tracking && m_tracking->busy();
    const bool hasLine = hasRowWorkLine();
    const bool isRunning = clientReady && m_tracking->snapshot().isExecuting();
    const bool isPaused = clientReady && m_tracking->snapshot().state == "Paused";
    const bool editable = canEditRowWorkPlan() && !busy;
    const bool mapEditingAllowed = !isRunning && !isPaused;

    if (m_addPointFromInputButton) {
        m_addPointFromInputButton->setEnabled(mapEditingAllowed);
    }
    if (m_addPointFromClickButton) {
        m_addPointFromClickButton->setEnabled(mapEditingAllowed);
    }
    if (m_updatePointButton) {
        m_updatePointButton->setEnabled(mapEditingAllowed);
    }
    if (m_removePointButton) {
        m_removePointButton->setEnabled(mapEditingAllowed);
    }
    if (m_batchGenerateButton) {
        m_batchGenerateButton->setEnabled(mapEditingAllowed);
    }
    if (m_addPathButton) {
        m_addPathButton->setEnabled(mapEditingAllowed);
    }
    if (m_removePathButton) {
        m_removePathButton->setEnabled(mapEditingAllowed);
    }

    if (m_rowWorkCaptureStartButton) {
        m_rowWorkCaptureStartButton->setEnabled(clientReady && !busy);
    }
    if (m_rowWorkCaptureEndButton) {
        m_rowWorkCaptureEndButton->setEnabled(clientReady && !busy);
    }
    if (m_rowWorkAddCheckpointFromVehicleButton) {
        m_rowWorkAddCheckpointFromVehicleButton->setEnabled(clientReady && hasLine && !busy);
    }
    if (m_rowWorkAddCheckpointFromMapButton) {
        m_rowWorkAddCheckpointFromMapButton->setEnabled(hasLine && editable);
    }
    if (m_rowWorkDeleteCheckpointButton) {
        m_rowWorkDeleteCheckpointButton->setEnabled(editable && m_rowWorkCheckpointTable
                                                    && m_rowWorkCheckpointTable->currentRow() >= 0);
    }
    if (m_rowWorkClearCheckpointsButton) {
        m_rowWorkClearCheckpointsButton->setEnabled(editable && !m_rowWorkPlan.checkpoints.isEmpty());
    }
    if (m_rowWorkClearLineButton) {
        m_rowWorkClearLineButton->setEnabled(editable && (m_rowWorkPlan.hasStartPose || m_rowWorkPlan.hasEndPose
                                                          || !m_rowWorkPlan.checkpoints.isEmpty()));
    }
    if (m_rowWorkUploadPlanButton) {
        m_rowWorkUploadPlanButton->setEnabled(clientReady && hasLine && !busy);
    }
    if (m_rowWorkReadPlanButton) {
        m_rowWorkReadPlanButton->setEnabled(clientReady && !busy);
    }
    if (m_rowWorkStartButton) {
        m_rowWorkStartButton->setEnabled(clientReady && !busy && m_tracking->snapshot().state == "Ready");
    }
    if (m_rowWorkPauseButton) {
        m_rowWorkPauseButton->setEnabled(clientReady && isRunning && !busy);
    }
    if (m_rowWorkResumeButton) {
        m_rowWorkResumeButton->setEnabled(clientReady && isPaused && !busy);
    }
    if (m_rowWorkStopButton) {
        m_rowWorkStopButton->setEnabled(clientReady && (isRunning || isPaused) && !busy);
    }
}

void Map::refreshRowWorkGraphics()
{
    clearRowWorkGraphics();

    if (!m_scene || !hasRowWorkLine()) {
        return;
    }

    const QPointF start = m_rowWorkPlan.startPose.toPointF();
    const QPointF end = m_rowWorkPlan.endPose.toPointF();
    const QPointF sceneStart = mapToScene(start);
    const QPointF sceneEnd = mapToScene(end);

    QPainterPath linePath;
    linePath.moveTo(sceneStart);
    linePath.lineTo(sceneEnd);
    m_rowWorkLineItem = m_scene->addPath(linePath, QPen(QColor(40, 120, 210), 2.5, Qt::DashLine));
    m_rowWorkLineItem->setZValue(-3.5);

    m_rowWorkStartMarker = m_scene->addEllipse(-6.0, -6.0, 12.0, 12.0,
                                               QPen(QColor(20, 120, 60)),
                                               QBrush(QColor(70, 180, 90)));
    m_rowWorkStartMarker->setPos(sceneStart);
    m_rowWorkStartMarker->setZValue(6.0);

    m_rowWorkEndMarker = m_scene->addEllipse(-6.0, -6.0, 12.0, 12.0,
                                             QPen(QColor(170, 80, 20)),
                                             QBrush(QColor(230, 150, 70)));
    m_rowWorkEndMarker->setPos(sceneEnd);
    m_rowWorkEndMarker->setZValue(6.0);

    const QPointF arrowPoint = RowWorkGeometry::pointAtProgress(m_rowWorkPlan, m_rowWorkPlan.lineLength() * 0.5);
    const QPointF arrowScene = mapToScene(arrowPoint);
    const double headingDeg = qRadiansToDegrees(std::atan2(end.y() - start.y(), end.x() - start.x()));
    m_rowWorkDirectionArrowItem = m_scene->addPath(makeDirectionArrowPath(), QPen(QColor(40, 120, 210), 2.0));
    m_rowWorkDirectionArrowItem->setPos(arrowScene);
    m_rowWorkDirectionArrowItem->setRotation(headingDeg);
    m_rowWorkDirectionArrowItem->setZValue(6.0);

    for (int i = 0; i < m_rowWorkPlan.checkpoints.size(); ++i) {
        const RowCheckpoint &checkpoint = m_rowWorkPlan.checkpoints.at(i);
        const QPointF checkpointMapPos = RowWorkGeometry::pointAtProgress(m_rowWorkPlan, checkpoint.progress);
        const QPointF checkpointScenePos = mapToScene(checkpointMapPos);
        const bool isActiveCheckpoint = false;

        auto *group = new QGraphicsItemGroup();
        auto *circle = new QGraphicsEllipseItem(-5.0, -5.0, 10.0, 10.0);
        circle->setPen(QPen(isActiveCheckpoint ? QColor(200, 70, 20) : QColor(120, 50, 160), isActiveCheckpoint ? 2.0 : 1.0));
        circle->setBrush(QBrush(isActiveCheckpoint ? QColor(255, 190, 120) : QColor(185, 110, 220)));
        auto *label = new QGraphicsTextItem(QStringLiteral("P%1").arg(i + 1));
        label->setDefaultTextColor(isActiveCheckpoint ? QColor(180, 80, 20) : QColor(100, 40, 140));
        const QRectF labelRect = label->boundingRect();
        label->setPos(-labelRect.width() / 2.0, -labelRect.height() - 8.0);
        group->addToGroup(circle);
        group->addToGroup(label);
        group->setPos(checkpointScenePos);
        group->setZValue(6.0);
        m_scene->addItem(group);
        m_rowWorkCheckpointMarkers.append(group);
    }
}

void Map::clearRowWorkGraphics()
{
    if (m_rowWorkLineItem) {
        delete m_rowWorkLineItem;
        m_rowWorkLineItem = nullptr;
    }
    if (m_rowWorkDirectionArrowItem) {
        delete m_rowWorkDirectionArrowItem;
        m_rowWorkDirectionArrowItem = nullptr;
    }
    if (m_rowWorkStartMarker) {
        delete m_rowWorkStartMarker;
        m_rowWorkStartMarker = nullptr;
    }
    if (m_rowWorkEndMarker) {
        delete m_rowWorkEndMarker;
        m_rowWorkEndMarker = nullptr;
    }
    qDeleteAll(m_rowWorkCheckpointMarkers);
    m_rowWorkCheckpointMarkers.clear();
}

void Map::setRowWorkStatusText(const QString &text, bool warning)
{
    if (!m_rowWorkStatusLabel) {
        return;
    }
    m_rowWorkStatusLabel->setText(text);
    QPalette palette = m_rowWorkStatusLabel->palette();
    palette.setColor(QPalette::WindowText, warning ? QColor(220, 80, 60) : QColor(55, 55, 55));
    m_rowWorkStatusLabel->setPalette(palette);
}

void Map::updateRowWorkPlanVersion()
{
    m_rowWorkPlan.version = qMax(1, m_rowWorkPlan.version + 1);
    if (m_rowWorkPlan.planId.trimmed().isEmpty()) {
        m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    }
    if (m_rowWorkPlan.frameId.trimmed().isEmpty()) {
        m_rowWorkPlan.frameId = QStringLiteral("map");
    }
}

bool Map::hasRowWorkLine() const
{
    return m_rowWorkPlan.isValid();
}

void Map::clearRowWorkPlanInternal(bool keepStatusMessage)
{
    m_rowWorkPlan = RowWorkPlan{};
    m_rowWorkPlan.frameBinding = m_binding;
    m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    m_rowWorkPlan.frameId = QStringLiteral("map");
    m_rowWorkPlan.params.loopEnabled = true;
    m_rowWorkClickPlacementMode = false;
    m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    if (m_rowWorkAddCheckpointFromMapButton) {
        QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
        m_rowWorkAddCheckpointFromMapButton->setChecked(false);
    }
    refreshRowWorkUi();
    if (!keepStatusMessage) {
        setRowWorkStatusText(tr("已清除作业线与中间点"));
    }
}

void Map::addRowWorkCheckpoint(const QPointF &mapPos)
{
    if (!hasRowWorkLine()) {
        setRowWorkStatusText(tr("请先完成 A/B 示教后再添加中间点"), true);
        return;
    }

    RowCheckpoint checkpoint;
    checkpoint.name = nextCheckpointName();
    checkpoint.progress = RowWorkGeometry::projectPointToProgress(m_rowWorkPlan, mapPos);
    checkpoint.dwellMs = 2000;
    checkpoint.enabled = true;
    checkpoint.triggerOnForward = true;
    checkpoint.triggerOnBackward = true;

    m_rowWorkPlan.checkpoints.append(checkpoint);
    sortRowWorkCheckpoints();
    updateCheckpointRowNames();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
}

void Map::sortRowWorkCheckpoints()
{
    std::sort(m_rowWorkPlan.checkpoints.begin(), m_rowWorkPlan.checkpoints.end(), [](const RowCheckpoint &lhs, const RowCheckpoint &rhs) {
        return lhs.progress < rhs.progress;
    });
}

QString Map::nextCheckpointName() const
{
    return QStringLiteral("P%1").arg(m_rowWorkPlan.checkpoints.size() + 1);
}

RowCheckpoint Map::checkpointFromRow(int row) const
{
    RowCheckpoint checkpoint;
    if (!m_rowWorkCheckpointTable || row < 0 || row >= m_rowWorkCheckpointTable->rowCount()) {
        return checkpoint;
    }

    if (const auto *nameItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableNameColumn)) {
        checkpoint.name = nameItem->text().trimmed();
    }
    if (const auto *progressItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableProgressColumn)) {
        checkpoint.progress = progressItem->text().toDouble();
    }
    if (const auto *dwellItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableDwellColumn)) {
        checkpoint.dwellMs = dwellItem->text().toInt();
    }
    if (const auto *forwardItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableForwardColumn)) {
        checkpoint.triggerOnForward = forwardItem->checkState() == Qt::Checked;
    }
    if (const auto *backwardItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableBackwardColumn)) {
        checkpoint.triggerOnBackward = backwardItem->checkState() == Qt::Checked;
    }
    checkpoint.enabled = true;
    if (const auto* item = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableCaptureColumn)) checkpoint.capturePhoto = item->checkState() == Qt::Checked;
    if (const auto* item = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableTimeoutColumn)) checkpoint.actionTimeoutMs = item->text().toInt();
    return checkpoint;
}

void Map::updateCheckpointRowNames()
{
    for (int i = 0; i < m_rowWorkPlan.checkpoints.size(); ++i) {
        if (m_rowWorkPlan.checkpoints[i].name.trimmed().isEmpty()) {
            m_rowWorkPlan.checkpoints[i].name = QStringLiteral("P%1").arg(i + 1);
        }
    }
}

bool Map::canEditRowWorkPlan() const
{
    return !m_tracking || (!m_tracking->busy() && (!m_tracking->fresh() ||
        (!m_tracking->snapshot().isExecuting() && m_tracking->snapshot().state != "Paused")));
}

bool Map::uploadRowWorkPlanIfNeeded(bool forceUpload)
{
    Q_UNUSED(forceUpload);
    if (!m_coordinator || !hasRowWorkLine()) return false;
    auto options = taskOptions(); options.repeatUntilStopped = m_rowWorkPlan.params.loopEnabled;
    const auto task = TaskCompiler::row(m_rowWorkPlan, m_binding, options, true);
    if (!task.ok()) { setRowWorkStatusText(task.error, true); return false; }
    const bool sent = m_coordinator->upload(task.plan);
    if (sent) setRowWorkStatusText(tr("单垄任务已提交校验，Ready 后请明确启动"));
    return sent;
}


void Map::refreshRowMissionUi()
{
    refreshRowMissionSummary();
    refreshRowMissionStepTable();
    refreshRowMissionStepEditor();
    refreshRowMissionControlState();
}

void Map::refreshRowMissionSummary()
{
    if (m_rowMissionIdLabel) {
        const QString missionId = m_rowMissionPlan.missionId.trimmed().isEmpty()
                                      ? tr("-")
                                      : m_rowMissionPlan.missionId.trimmed();
        m_rowMissionIdLabel->setText(missionId);
    }
    if (m_rowMissionNameEdit) {
        QSignalBlocker blocker(m_rowMissionNameEdit);
        m_rowMissionNameEdit->setText(m_rowMissionPlan.name.trimmed());
    }
    if (m_rowMissionLoopCheck) {
        QSignalBlocker blocker(m_rowMissionLoopCheck);
        m_rowMissionLoopCheck->setChecked(m_rowMissionPlan.loopEnabled);
    }
    if (m_rowMissionSummaryLabel) {
        int rowLegCount = 0;
        int transferCount = 0;
        int turnCount = 0;
        int waitCount = 0;
        for (const RowMissionStep &step : m_rowMissionPlan.steps) {
            switch (step.type) {
            case RowMissionStepType::RowLeg:
                ++rowLegCount;
                break;
            case RowMissionStepType::Transfer:
                ++transferCount;
                break;
            case RowMissionStepType::Turn:
                ++turnCount;
                break;
            case RowMissionStepType::Wait:
                ++waitCount;
                break;
            }
        }

        const QString summary = tr("步骤总数 %1，启用 %2，垄内 %3，转场 %4，掉头 %5，停留 %6，循环=%7")
                                    .arg(m_rowMissionPlan.steps.size())
                                    .arg(m_rowMissionPlan.enabledStepCount())
                                    .arg(rowLegCount)
                                    .arg(transferCount)
                                    .arg(turnCount)
                                    .arg(waitCount)
                                    .arg(m_rowMissionPlan.loopEnabled ? tr("开") : tr("关"));
        m_rowMissionSummaryLabel->setText(summary);
    }
}

void Map::refreshRowMissionStepTable()
{
    if (!m_rowMissionStepTable) {
        return;
    }

    m_rowMissionStepTableUpdating = true;
    QSignalBlocker blocker(m_rowMissionStepTable);
    m_rowMissionStepTable->setRowCount(m_rowMissionPlan.steps.size());
    for (int row = 0; row < m_rowMissionPlan.steps.size(); ++row) {
        const RowMissionStep &step = m_rowMissionPlan.steps.at(row);

        auto *enabledItem = new QTableWidgetItem();
        enabledItem->setFlags((enabledItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                              & ~Qt::ItemIsEditable);
        enabledItem->setCheckState(step.enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setTextAlignment(Qt::AlignCenter);
        m_rowMissionStepTable->setItem(row, kRowMissionStepEnabledColumn, enabledItem);

        auto *nameItem = new QTableWidgetItem(step.name.trimmed());
        nameItem->setTextAlignment(Qt::AlignCenter);
        m_rowMissionStepTable->setItem(row, kRowMissionStepNameColumn, nameItem);

        auto *typeItem = new QTableWidgetItem(rowMissionStepTypeText(step.type));
        typeItem->setFlags((typeItem->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable);
        typeItem->setTextAlignment(Qt::AlignCenter);
        m_rowMissionStepTable->setItem(row, kRowMissionStepTypeColumn, typeItem);

        auto *summaryItem = new QTableWidgetItem(rowMissionStepSummaryText(step));
        summaryItem->setFlags((summaryItem->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable);
        summaryItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_rowMissionStepTable->setItem(row, kRowMissionStepSummaryColumn, summaryItem);
    }
    m_rowMissionStepTable->resizeRowsToContents();
    m_rowMissionStepTableUpdating = false;

    if (m_rowMissionPlan.steps.isEmpty()) {
        m_rowMissionSelectedStepRow = -1;
        return;
    }

    if (m_rowMissionSelectedStepRow < 0 || m_rowMissionSelectedStepRow >= m_rowMissionPlan.steps.size()) {
        m_rowMissionSelectedStepRow = 0;
    }
    selectRowMissionStep(m_rowMissionSelectedStepRow);
}

void Map::refreshRowMissionControlState()
{
    const bool editable = canEditRowMissionPlan();
    const int selectedRow = rowMissionSelectedStepRow();
    const bool hasSelection = selectedRow >= 0 && selectedRow < m_rowMissionPlan.steps.size();

    if (m_rowMissionNameEdit) {
        m_rowMissionNameEdit->setEnabled(editable);
    }
    if (m_rowMissionLoopCheck) {
        m_rowMissionLoopCheck->setEnabled(editable);
    }
    if (m_rowMissionAddRowLegButton) {
        m_rowMissionAddRowLegButton->setEnabled(editable && hasRowWorkLine());
    }
    if (m_rowMissionAddTransferButton) {
        m_rowMissionAddTransferButton->setEnabled(editable && hasRowWorkLine());
    }
    if (m_rowMissionAddTurnButton) {
        m_rowMissionAddTurnButton->setEnabled(editable);
    }
    if (m_rowMissionAddWaitButton) {
        m_rowMissionAddWaitButton->setEnabled(editable);
    }
    if (m_rowMissionImportRowWorkButton) {
        m_rowMissionImportRowWorkButton->setEnabled(editable && hasRowWorkLine());
    }
    if (m_rowMissionRemoveStepButton) {
        m_rowMissionRemoveStepButton->setEnabled(editable && hasSelection);
    }
    if (m_rowMissionMoveUpButton) {
        m_rowMissionMoveUpButton->setEnabled(editable && hasSelection && selectedRow > 0);
    }
    if (m_rowMissionMoveDownButton) {
        m_rowMissionMoveDownButton->setEnabled(editable && hasSelection && selectedRow < m_rowMissionPlan.steps.size() - 1);
    }
    if (m_rowMissionClearButton) {
        m_rowMissionClearButton->setEnabled(editable && !m_rowMissionPlan.steps.isEmpty());
    }
    if (m_rowMissionApplyStepButton) {
        m_rowMissionApplyStepButton->setEnabled(editable && hasSelection);
    }
    if (m_rowMissionStepTable) {
        m_rowMissionStepTable->setEnabled(editable || !m_rowMissionPlan.steps.isEmpty());
    }
}

void Map::refreshRowMissionStepEditor()
{
    loadRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::setRowMissionStatusText(const QString &text, bool warning)
{
    if (!m_rowMissionStatusLabel) {
        return;
    }
    m_rowMissionStatusLabel->setText(text);
    QPalette palette = m_rowMissionStatusLabel->palette();
    palette.setColor(QPalette::WindowText, warning ? QColor(220, 80, 60) : QColor(55, 55, 55));
    m_rowMissionStatusLabel->setPalette(palette);
}

void Map::updateRowMissionPlanVersion()
{
    m_rowMissionPlan.version = qMax(1, m_rowMissionPlan.version + 1);
    if (m_rowMissionPlan.missionId.trimmed().isEmpty()) {
        m_rowMissionPlan.missionId = RowMissionJson::generateMissionId();
    }
    if (m_rowMissionPlan.frameId.trimmed().isEmpty()) {
        m_rowMissionPlan.frameId = QStringLiteral("map");
    }
}

bool Map::canEditRowMissionPlan() const
{
    return canEditRowWorkPlan();
}

RowMissionStep *Map::rowMissionStepAt(int row)
{
    if (row < 0 || row >= m_rowMissionPlan.steps.size()) {
        return nullptr;
    }
    return &m_rowMissionPlan.steps[row];
}

const RowMissionStep *Map::rowMissionStepAt(int row) const
{
    if (row < 0 || row >= m_rowMissionPlan.steps.size()) {
        return nullptr;
    }
    return &m_rowMissionPlan.steps[row];
}

int Map::rowMissionSelectedStepRow() const
{
    if (m_rowMissionStepTable) {
        const int currentRow = m_rowMissionStepTable->currentRow();
        if (currentRow >= 0 && currentRow < m_rowMissionPlan.steps.size()) {
            return currentRow;
        }
    }
    return m_rowMissionSelectedStepRow;
}

QString Map::rowMissionStepTypeText(RowMissionStepType type) const
{
    switch (type) {
    case RowMissionStepType::RowLeg:
        return tr("垄内段");
    case RowMissionStepType::Transfer:
        return tr("转场段");
    case RowMissionStepType::Turn:
        return tr("掉头");
    case RowMissionStepType::Wait:
        return tr("停留");
    }
    return tr("垄内段");
}

QString Map::rowMissionStepSummaryText(const RowMissionStep &step) const
{
    switch (step.type) {
    case RowMissionStepType::RowLeg:
    case RowMissionStepType::Transfer:
        if (!step.primitivePlan.isValid()) {
            return tr("未配置原语");
        }
        return tr("长度 %1 m，中间点 %2，循环=%3")
            .arg(step.primitivePlan.lineLength(), 0, 'f', 2)
            .arg(step.primitivePlan.checkpoints.size())
            .arg(step.primitivePlan.params.loopEnabled ? tr("开") : tr("关"));
    case RowMissionStepType::Turn:
        return tr("目标航向 %1°").arg(qRadiansToDegrees(step.targetYawRad), 0, 'f', 1);
    case RowMissionStepType::Wait:
        return tr("停留 %1 ms").arg(step.dwellMs);
    }
    return QString();
}

RowMissionStep Map::buildMissionStepFromCurrentRowWork(RowMissionStepType type) const
{
    RowMissionStep step;
    step.stepId = RowMissionJson::generateStepId(m_rowMissionPlan.steps.size());
    step.type = type;
    step.enabled = true;
    step.targetYawRad = m_vehiclePoseTheta;

    switch (type) {
    case RowMissionStepType::RowLeg:
        step.name = tr("垄段%1").arg(m_rowMissionPlan.steps.size() + 1);
        step.note = tr("使用当前单垄原语");
        step.primitivePlan = m_rowWorkPlan;
        break;
    case RowMissionStepType::Transfer:
        step.name = tr("转场%1").arg(m_rowMissionPlan.steps.size() + 1);
        step.note = tr("使用当前单垄原语作为短转场模板");
        step.primitivePlan = m_rowWorkPlan;
        break;
    case RowMissionStepType::Turn:
        step.name = tr("掉头%1").arg(m_rowMissionPlan.steps.size() + 1);
        step.note = tr("原地转向步骤");
        step.targetYawRad = m_hasVehiclePose ? m_vehiclePoseTheta : 0.0;
        break;
    case RowMissionStepType::Wait:
        step.name = tr("停留%1").arg(m_rowMissionPlan.steps.size() + 1);
        step.note = tr("任务内停留步骤");
        step.dwellMs = 2000;
        break;
    }
    return step;
}

void Map::insertRowMissionStep(const RowMissionStep &step, int row)
{
    RowMissionStep normalizedStep = step;
    if (normalizedStep.stepId.trimmed().isEmpty()) {
        normalizedStep.stepId = RowMissionJson::generateStepId(row >= 0 ? row : m_rowMissionPlan.steps.size());
    }
    if (normalizedStep.name.trimmed().isEmpty()) {
        normalizedStep.name = normalizedStep.stepId;
    }

    if (row < 0 || row > m_rowMissionPlan.steps.size()) {
        m_rowMissionPlan.steps.append(normalizedStep);
        m_rowMissionSelectedStepRow = m_rowMissionPlan.steps.size() - 1;
    } else {
        m_rowMissionPlan.steps.insert(row, normalizedStep);
        m_rowMissionSelectedStepRow = row;
    }

    updateRowMissionStepNames();
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::moveRowMissionStep(int fromRow, int toRow)
{
    if (fromRow < 0 || fromRow >= m_rowMissionPlan.steps.size()) {
        return;
    }
    if (toRow < 0 || toRow >= m_rowMissionPlan.steps.size() || fromRow == toRow) {
        return;
    }

    m_rowMissionPlan.steps.move(fromRow, toRow);
    m_rowMissionSelectedStepRow = toRow;
    updateRowMissionStepNames();
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::updateRowMissionStepNames()
{
    for (int i = 0; i < m_rowMissionPlan.steps.size(); ++i) {
        RowMissionStep &step = m_rowMissionPlan.steps[i];
        if (step.stepId.trimmed().isEmpty()) {
            step.stepId = RowMissionJson::generateStepId(i);
        }
        if (step.name.trimmed().isEmpty()) {
            step.name = QStringLiteral("%1 %2").arg(rowMissionStepTypeText(step.type)).arg(i + 1);
        }
    }
}

void Map::loadRowMissionStepEditor(int row)
{
    m_rowMissionEditorUpdating = true;
    const RowMissionStep *step = rowMissionStepAt(row);
    if (!step) {
        if (m_rowMissionStepTypeCombo) {
            QSignalBlocker blocker(m_rowMissionStepTypeCombo);
            m_rowMissionStepTypeCombo->setCurrentIndex(0);
            m_rowMissionStepTypeCombo->setEnabled(false);
        }
        if (m_rowMissionStepNameEdit) {
            QSignalBlocker blocker(m_rowMissionStepNameEdit);
            m_rowMissionStepNameEdit->clear();
            m_rowMissionStepNameEdit->setEnabled(false);
        }
        if (m_rowMissionStepNoteEdit) {
            QSignalBlocker blocker(m_rowMissionStepNoteEdit);
            m_rowMissionStepNoteEdit->clear();
            m_rowMissionStepNoteEdit->setEnabled(false);
        }
        if (m_rowMissionTargetYawSpin) {
            QSignalBlocker blocker(m_rowMissionTargetYawSpin);
            m_rowMissionTargetYawSpin->setValue(0.0);
            m_rowMissionTargetYawSpin->setEnabled(false);
        }
        if (m_rowMissionDwellSpin) {
            QSignalBlocker blocker(m_rowMissionDwellSpin);
            m_rowMissionDwellSpin->setValue(0);
            m_rowMissionDwellSpin->setEnabled(false);
        }
        if (m_rowMissionPrimitiveSummaryLabel) {
            m_rowMissionPrimitiveSummaryLabel->setText(tr("原语：未选择步骤"));
        }
        m_rowMissionEditorUpdating = false;
        return;
    }

    if (m_rowMissionStepTypeCombo) {
        QSignalBlocker blocker(m_rowMissionStepTypeCombo);
        const int index = m_rowMissionStepTypeCombo->findData(static_cast<int>(step->type));
        m_rowMissionStepTypeCombo->setCurrentIndex(index >= 0 ? index : 0);
        m_rowMissionStepTypeCombo->setEnabled(canEditRowMissionPlan());
    }
    if (m_rowMissionStepNameEdit) {
        QSignalBlocker blocker(m_rowMissionStepNameEdit);
        m_rowMissionStepNameEdit->setText(step->name);
        m_rowMissionStepNameEdit->setEnabled(canEditRowMissionPlan());
    }
    if (m_rowMissionStepNoteEdit) {
        QSignalBlocker blocker(m_rowMissionStepNoteEdit);
        m_rowMissionStepNoteEdit->setText(step->note);
        m_rowMissionStepNoteEdit->setEnabled(canEditRowMissionPlan());
    }
    if (m_rowMissionTargetYawSpin) {
        QSignalBlocker blocker(m_rowMissionTargetYawSpin);
        m_rowMissionTargetYawSpin->setValue(qRadiansToDegrees(step->targetYawRad));
        m_rowMissionTargetYawSpin->setEnabled(canEditRowMissionPlan() && step->type == RowMissionStepType::Turn);
    }
    if (m_rowMissionDwellSpin) {
        QSignalBlocker blocker(m_rowMissionDwellSpin);
        m_rowMissionDwellSpin->setValue(step->dwellMs);
        m_rowMissionDwellSpin->setEnabled(canEditRowMissionPlan() && step->type == RowMissionStepType::Wait);
    }
    if (m_rowMissionPrimitiveSummaryLabel) {
        if (step->type == RowMissionStepType::RowLeg || step->type == RowMissionStepType::Transfer) {
            if (step->primitivePlan.isValid()) {
                m_rowMissionPrimitiveSummaryLabel->setText(
                    tr("原语：长度 %1 m，中间点 %2，版本 %3")
                        .arg(step->primitivePlan.lineLength(), 0, 'f', 2)
                        .arg(step->primitivePlan.checkpoints.size())
                        .arg(step->primitivePlan.version));
            } else {
                m_rowMissionPrimitiveSummaryLabel->setText(tr("原语：未配置有效的单垄计划"));
            }
        } else {
            m_rowMissionPrimitiveSummaryLabel->setText(tr("原语：当前步骤不使用单垄原语"));
        }
    }
    m_rowMissionEditorUpdating = false;
}

void Map::applyRowMissionStepEditor(int row)
{
    if (m_rowMissionEditorUpdating) {
        return;
    }
    RowMissionStep *step = rowMissionStepAt(row);
    if (!step) {
        return;
    }

    if (m_rowMissionStepTypeCombo) {
        const QVariant data = m_rowMissionStepTypeCombo->currentData();
        step->type = static_cast<RowMissionStepType>(data.toInt());
    }
    if (m_rowMissionStepNameEdit) {
        step->name = m_rowMissionStepNameEdit->text().trimmed();
    }
    if (m_rowMissionStepNoteEdit) {
        step->note = m_rowMissionStepNoteEdit->text().trimmed();
    }
    if (m_rowMissionTargetYawSpin) {
        step->targetYawRad = qDegreesToRadians(m_rowMissionTargetYawSpin->value());
    }
    if (m_rowMissionDwellSpin) {
        step->dwellMs = m_rowMissionDwellSpin->value();
    }
    if ((step->type == RowMissionStepType::RowLeg || step->type == RowMissionStepType::Transfer)
        && !step->primitivePlan.isValid() && hasRowWorkLine()) {
        step->primitivePlan = m_rowWorkPlan;
    }

    if (step->name.trimmed().isEmpty()) {
        step->name = QStringLiteral("%1 %2").arg(rowMissionStepTypeText(step->type)).arg(row + 1);
    }

    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::syncRowMissionEditorWidgets()
{
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::selectRowMissionStep(int row)
{
    if (!m_rowMissionStepTable || row < 0 || row >= m_rowMissionStepTable->rowCount()) {
        return;
    }
    m_rowMissionSelectedStepRow = row;
    QSignalBlocker blocker(m_rowMissionStepTable);
    m_rowMissionStepTable->setCurrentCell(row, kRowMissionStepNameColumn);
}

void Map::handleRowWorkCaptureStartPoint()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (m_poseClient->capture(m_binding)) {
        m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::StartPose;
        setRowWorkStatusText(tr("正在采集停稳后的独立定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkCaptureEndPoint()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (m_poseClient->capture(m_binding)) {
        m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::EndPose;
        setRowWorkStatusText(tr("正在采集停稳后的独立定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkAddCheckpointFromVehicle()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (m_poseClient->capture(m_binding)) {
        m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::CheckpointFromVehicle;
        setRowWorkStatusText(tr("正在采集停稳后的独立定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkAddCheckpointFromMap()
{
    if (!hasRowWorkLine()) {
        setRowWorkStatusText(tr("请先完成 A/B 示教后再添加中间点"), true);
        if (m_rowWorkAddCheckpointFromMapButton) {
            QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
            m_rowWorkAddCheckpointFromMapButton->setChecked(false);
        }
        return;
    }

    m_rowWorkClickPlacementMode = m_rowWorkAddCheckpointFromMapButton && m_rowWorkAddCheckpointFromMapButton->isChecked();
    setRowWorkStatusText(m_rowWorkClickPlacementMode ? tr("请在地图上点击添加中间点") : tr("已取消地图点击添加中间点"));
}

void Map::handleRowWorkDeleteCheckpoint()
{
    if (!m_rowWorkCheckpointTable) {
        return;
    }
    const int row = m_rowWorkCheckpointTable->currentRow();
    if (row < 0 || row >= m_rowWorkPlan.checkpoints.size()) {
        return;
    }
    m_rowWorkPlan.checkpoints.removeAt(row);
    updateCheckpointRowNames();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
    setRowWorkStatusText(tr("已删除中间点"));
}

void Map::handleRowWorkClearCheckpoints()
{
    if (m_rowWorkPlan.checkpoints.isEmpty()) {
        return;
    }
    m_rowWorkPlan.checkpoints.clear();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
    setRowWorkStatusText(tr("已清空中间点"));
}

void Map::handleRowWorkClearPlan()
{
    clearRowWorkPlanInternal();
}

void Map::handleRowWorkReadPlan()
{
    if (m_tracking && m_tracking->fresh()) {
        const auto status = m_tracking->snapshot();
        if (!status.taskId.isEmpty()) m_tracking->readTask(status.taskId, status.taskRevision);
    }
}

void Map::handleRowWorkUploadPlan()
{
    refreshPlanningRevision();
    uploadRowWorkPlanIfNeeded(true);
}

void Map::handleRowWorkStart()
{
    if (m_coordinator) { refreshPlanningRevision(); m_coordinator->startTask(); }
}

void Map::handleRowWorkPause()
{
    if (m_coordinator) m_coordinator->pauseTask();
}

void Map::handleRowWorkResume()
{
    if (m_coordinator) { refreshPlanningRevision(); m_coordinator->resumeTask(); }
}

void Map::handleRowWorkStop()
{
    if (m_coordinator) m_coordinator->abortTask();
}

void Map::handleRowWorkLoopChanged(bool checked)
{
    if (m_rowWorkPlan.params.loopEnabled == checked) {
        return;
    }
    m_rowWorkPlan.params.loopEnabled = checked;
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
}

void Map::handleRowWorkCheckpointCellChanged(int row, int column)
{
    Q_UNUSED(column);
    if (m_rowWorkCheckpointTableUpdating || row < 0 || row >= m_rowWorkPlan.checkpoints.size()) {
        return;
    }

    RowCheckpoint checkpoint = checkpointFromRow(row);
    checkpoint.progress = RowWorkGeometry::clampProgress(m_rowWorkPlan, checkpoint.progress);
    checkpoint.dwellMs = qMax(0, checkpoint.dwellMs);
    if (checkpoint.name.trimmed().isEmpty()) {
        checkpoint.name = QStringLiteral("P%1").arg(row + 1);
    }
    m_rowWorkPlan.checkpoints[row] = checkpoint;
    sortRowWorkCheckpoints();
    updateCheckpointRowNames();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
}

void Map::handleRowMissionNameEdited()
{
    if (m_rowMissionEditorUpdating) {
        return;
    }
    const QString name = m_rowMissionNameEdit ? m_rowMissionNameEdit->text().trimmed() : QString();
    if (m_rowMissionPlan.name == name) {
        return;
    }
    m_rowMissionPlan.name = name;
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::handleRowMissionLoopChanged(bool checked)
{
    if (m_rowMissionPlan.loopEnabled == checked) {
        return;
    }
    m_rowMissionPlan.loopEnabled = checked;
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::handleRowMissionStepSelectionChanged()
{
    if (!m_rowMissionStepTable) {
        return;
    }
    m_rowMissionSelectedStepRow = m_rowMissionStepTable->currentRow();
    loadRowMissionStepEditor(m_rowMissionSelectedStepRow);
    refreshRowMissionControlState();
}

void Map::handleRowMissionStepCellChanged(int row, int column)
{
    if (m_rowMissionStepTableUpdating || row < 0 || row >= m_rowMissionPlan.steps.size()) {
        return;
    }

    RowMissionStep &step = m_rowMissionPlan.steps[row];
    if (column == kRowMissionStepEnabledColumn) {
        if (const auto *item = m_rowMissionStepTable->item(row, column)) {
            step.enabled = item->checkState() == Qt::Checked;
        }
    } else if (column == kRowMissionStepNameColumn) {
        if (const auto *item = m_rowMissionStepTable->item(row, column)) {
            step.name = item->text().trimmed();
        }
    }

    updateRowMissionStepNames();
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
}

void Map::handleRowMissionTypeChanged(int index)
{
    Q_UNUSED(index);
    if (m_rowMissionEditorUpdating) {
        return;
    }
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::handleRowMissionStepNameEdited()
{
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::handleRowMissionStepNoteEdited()
{
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::handleRowMissionTargetYawChanged(double value)
{
    Q_UNUSED(value);
    if (m_rowMissionEditorUpdating) {
        return;
    }
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::handleRowMissionDwellChanged(int value)
{
    Q_UNUSED(value);
    if (m_rowMissionEditorUpdating) {
        return;
    }
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

void Map::handleRowMissionAddRowLegStep()
{
    if (!canEditRowMissionPlan()) {
        return;
    }
    insertRowMissionStep(buildMissionStepFromCurrentRowWork(RowMissionStepType::RowLeg));
    setRowMissionStatusText(tr("已添加垄内段步骤"));
}

void Map::handleRowMissionAddTransferStep()
{
    if (!canEditRowMissionPlan()) {
        return;
    }
    insertRowMissionStep(buildMissionStepFromCurrentRowWork(RowMissionStepType::Transfer));
    setRowMissionStatusText(tr("已添加转场段步骤"));
}

void Map::handleRowMissionAddTurnStep()
{
    if (!canEditRowMissionPlan()) {
        return;
    }
    insertRowMissionStep(buildMissionStepFromCurrentRowWork(RowMissionStepType::Turn));
    setRowMissionStatusText(tr("已添加掉头步骤"));
}

void Map::handleRowMissionAddWaitStep()
{
    if (!canEditRowMissionPlan()) {
        return;
    }
    insertRowMissionStep(buildMissionStepFromCurrentRowWork(RowMissionStepType::Wait));
    setRowMissionStatusText(tr("已添加停留步骤"));
}

void Map::handleRowMissionRemoveStep()
{
    const int row = rowMissionSelectedStepRow();
    if (!canEditRowMissionPlan() || row < 0 || row >= m_rowMissionPlan.steps.size()) {
        return;
    }
    m_rowMissionPlan.steps.removeAt(row);
    updateRowMissionStepNames();
    m_rowMissionSelectedStepRow = m_rowMissionPlan.steps.isEmpty() ? -1 : qMin(row, m_rowMissionPlan.steps.size() - 1);
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
    setRowMissionStatusText(tr("已删除步骤"));
}

void Map::handleRowMissionMoveStepUp()
{
    const int row = rowMissionSelectedStepRow();
    if (!canEditRowMissionPlan() || row <= 0) {
        return;
    }
    moveRowMissionStep(row, row - 1);
}

void Map::handleRowMissionMoveStepDown()
{
    const int row = rowMissionSelectedStepRow();
    if (!canEditRowMissionPlan() || row < 0 || row >= m_rowMissionPlan.steps.size() - 1) {
        return;
    }
    moveRowMissionStep(row, row + 1);
}

void Map::handleRowMissionClearSteps()
{
    if (!canEditRowMissionPlan() || m_rowMissionPlan.steps.isEmpty()) {
        return;
    }
    m_rowMissionPlan.steps.clear();
    m_rowMissionSelectedStepRow = -1;
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
    setRowMissionStatusText(tr("已清空步骤"));
}

void Map::handleRowMissionImportCurrentRowWork()
{
    if (!canEditRowMissionPlan()) {
        return;
    }
    if (!hasRowWorkLine()) {
        setRowMissionStatusText(tr("请先完成当前单垄原语示教"), true);
        return;
    }

    m_rowMissionPlan.steps.clear();
    RowMissionStep rowLeg = buildMissionStepFromCurrentRowWork(RowMissionStepType::RowLeg);
    rowLeg.name = tr("单垄往返");
    rowLeg.note = tr("从当前单垄原语导入");
    m_rowMissionPlan.steps.append(rowLeg);

    RowMissionStep turn = buildMissionStepFromCurrentRowWork(RowMissionStepType::Turn);
    turn.name = tr("掉头");
    turn.note = tr("导入后继续补充转场/停留步骤");
    m_rowMissionPlan.steps.append(turn);

    m_rowMissionSelectedStepRow = 0;
    updateRowMissionStepNames();
    updateRowMissionPlanVersion();
    refreshRowMissionUi();
    setRowMissionStatusText(tr("已从当前单垄导入任务模板"));
}

void Map::handleRowMissionApplyStepEdits()
{
    applyRowMissionStepEditor(rowMissionSelectedStepRow());
}

















































TaskCompileOptions Map::taskOptions() const
{
    const auto& d = ConfigManager::instance().taskDefaults();
    TaskCompileOptions options;
    options.speedLimit = d.speedLimit; options.goalToleranceMeters = d.goalToleranceMeters;
    options.angularSpeedLimit = d.angularSpeedLimit; options.angleToleranceRad = d.angleToleranceRad;
    options.safetyProfileId = d.safetyProfileId; options.rotationZoneId = d.rotationZoneId;
    return options;
}
void Map::setControlCoordinator(ControlSessionCoordinator* c)
{
    if (m_coordinator || !c) return;
    m_coordinator = c; m_tracking = c->tracking(); m_poseClient = c->pose(); c->setBinding(m_binding);
    connect(c, &ControlSessionCoordinator::message, this, [this](const QString& text) {
        setRowWorkStatusText(text); setRouteStatusText(text);
    });
    connect(m_tracking, &TrackingClient::statusChanged, this, &Map::applyTrackingStatus);
    connect(m_tracking, &TrackingClient::availabilityChanged, this, [this](bool online, const QString& reason) {
        if (!online && m_trackingStatusLabel) m_trackingStatusLabel->setText(reason);
        updateRouteControlState(); refreshRowWorkPlanSummary(); refreshRowWorkControlState();
    });
    connect(m_tracking, &TrackingClient::taskReceived, this, [this](const QJsonObject& task) {
        QMessageBox::information(m_mapPage, tr("工控机任务"), tr("任务 %1 版本 %2 · %3\n步骤数 %4\n计划校验值 %5")
            .arg(task["taskId"].toString()).arg(task["revision"].toInt()).arg(task["state"].toString())
            .arg(task["preparedStepCount"].toInt()).arg(task["planHash"].toString()));
    });
    connect(m_poseClient, &PoseClient::poseChanged, this, [this](const ControlPoseSnapshot& pose) {
        if (!m_binding.isUsable() || pose.originRevision != m_binding.context.originRevision ||
            pose.calibrationId != m_binding.context.calibrationId) { m_hasVehiclePose = false; return; }
        MapFrameAdapter adapter(m_binding); const auto point = adapter.fromEnu(pose.position);
        updateVehiclePose(point.x(), point.y(), adapter.yawFromEnu(pose.yaw));
    });
    connect(m_poseClient, &PoseClient::availabilityChanged, this, [this](bool ready, const QString&) {
        if (!ready) m_hasVehiclePose = false;
        refreshRowWorkPlanSummary();
    });
    connect(m_poseClient, &PoseClient::captureFinished, this, &Map::applyCapture);
    connect(m_poseClient, &PoseClient::errorOccurred, this, [this](const QString& reason) {
        m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None; setRowWorkStatusText(reason, true);
    });
}
void Map::applyTrackingStatus(const TrackingSnapshot& status)
{
    if (m_trackingStatusLabel) m_trackingStatusLabel->setText(tr("%1 · 工控机独立执行\n步骤 %2 · 循环 %3\n暂停 %4 · 故障 %5 · 结果 %6")
        .arg(status.state, status.stepId).arg(status.loopIndex)
        .arg(status.pauseReason, status.faultCode, status.result));
    updateRouteControlState(); refreshRowWorkPlanSummary(); refreshRowWorkControlState();
}
void Map::applyCapture(const QJsonObject& capture)
{
    const auto target = m_rowWorkPendingCaptureTarget; m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    if (capture["state"].toString() != "succeeded" || !m_binding.isUsable() ||
        capture["originRevision"].toString() != m_binding.context.originRevision ||
        capture["calibrationId"].toString() != m_binding.context.calibrationId) {
        setRowWorkStatusText(tr("采样失败或坐标版本改变：%1").arg(capture["reason"].toString()), true); return;
    }
    if (!m_rowWorkPlan.frameBinding.isUsable() ||
        !m_rowWorkPlan.frameBinding.context.sameCoordinates(m_binding.context)) {
        m_rowWorkPlan = RowWorkPlan{}; m_rowWorkPlan.frameBinding = m_binding;
        m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    }
    const auto o = capture["pose"].toObject();
    if (!o["x"].isDouble() || !o["y"].isDouble() || !o["yaw"].isDouble()) return;
    MapFrameAdapter adapter(m_binding); const auto point = adapter.fromEnu({o["x"].toDouble(), o["y"].toDouble()});
    RowWorkPose pose{point.x(), point.y(), adapter.yawFromEnu(o["yaw"].toDouble())};
    switch (target) {
    case RowWorkPendingCaptureTarget::StartPose: m_rowWorkPlan.startPose = pose; m_rowWorkPlan.hasStartPose = true; break;
    case RowWorkPendingCaptureTarget::EndPose: m_rowWorkPlan.endPose = pose; m_rowWorkPlan.hasEndPose = true; break;
    case RowWorkPendingCaptureTarget::CheckpointFromVehicle: addRowWorkCheckpoint(point); break;
    case RowWorkPendingCaptureTarget::None: return;
    }
    updateRowWorkPlanVersion(); refreshRowWorkUi();
    setRowWorkStatusText(tr("示教完成：%1 个独立位置样本，散布 %2 m")
        .arg(capture["sampleCount"].toInt()).arg(capture["positionScatterMeters"].toDouble(), 0, 'f', 3));
}
void Map::uploadMissionTask()
{
    refreshPlanningRevision();
    if (!m_coordinator || !canEditRowMissionPlan()) return;
    auto options = taskOptions(); options.repeatUntilStopped = m_rowMissionPlan.loopEnabled;
    const auto task = TaskCompiler::mission(m_rowMissionPlan, m_binding, options);
    if (!task.ok()) { setRowWorkStatusText(task.error, true); return; }
    if (m_coordinator->upload(task.plan)) setRowWorkStatusText(tr("多垄完整任务已提交校验，Ready 后请明确启动"));
}
void Map::confirmFrameBinding()
{
    if (!m_tracking || !m_tracking->fresh() || !canEditRowWorkPlan()) return;
    const auto config = m_tracking->configuration();
    const auto active = config["origin"].toObject()["active"].toObject();
    const QString origin = active["originRevision"].toString();
    const QString calibration = config["calibration"].toObject()["calibrationId"].toString();
    const QString profile = config["profile_revision"].toString();
    if (origin.isEmpty() || calibration.isEmpty() || profile.isEmpty()) { setRouteStatusText(tr("工控机原点和标定尚未就绪"), true); return; }
    QDialog dialog(m_mapPage); dialog.setWindowTitle(tr("确认地图与工控机坐标"));
    auto* layout = new QFormLayout(&dialog);
    auto* text = new QLabel(tr("地图南/东轴先转为东/北，再应用旋转和平移。请填写经测量确认的变换。\n原点 %1 · 标定 %2 · 控制配置 %3")
        .arg(origin, calibration, profile), &dialog); text->setWordWrap(true); layout->addRow(text);
    QDoubleSpinBox east, north, yaw;
    east.setRange(-1000000, 1000000); north.setRange(-1000000, 1000000); yaw.setRange(-180, 180);
    east.setDecimals(4); north.setDecimals(4); yaw.setDecimals(4);
    east.setValue(m_binding.enuTranslation.x()); north.setValue(m_binding.enuTranslation.y()); yaw.setValue(qRadiansToDegrees(m_binding.enuYawOffsetRad));
    layout->addRow(tr("东向平移 m"), &east); layout->addRow(tr("北向平移 m"), &north); layout->addRow(tr("旋转 deg"), &yaw);
    QCheckBox verified(tr("已核实地图、示教点及工控机坐标对应关系")); layout->addRow(&verified);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addRow(&buttons);
    buttons.button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(&verified, &QCheckBox::toggled, buttons.button(QDialogButtonBox::Ok), &QPushButton::setEnabled);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !verified.isChecked() || !m_tracking->fresh() ||
        m_tracking->configuration() != config || !canEditRowWorkPlan()) return;
    if (!m_currentMapFilePath.isEmpty() && QFile::exists(m_currentMapFilePath)) {
        const QString backup = m_currentMapFilePath + ".before-frame-binding.bak";
        if (!QFile::exists(backup) && !QFile::copy(m_currentMapFilePath, backup)) { setRouteStatusText(tr("地图备份失败，未修改坐标绑定"), true); return; }
    }
    if (m_binding.context.mapId.isEmpty()) m_binding.context.mapId = TrackingJson::newId();
    ++m_binding.context.mapRevision;
    m_binding.context.originRevision = origin; m_binding.context.calibrationId = calibration;
    m_binding.context.controllerProfileRevision = profile; m_binding.context.frameTransformRevision = TrackingJson::newId();
    m_binding.enuTranslation = {east.value(), north.value()}; m_binding.enuYawOffsetRad = qDegreesToRadians(yaw.value());
    m_binding.confirmed = true;
    m_rowWorkPlan.frameBinding = m_binding;
    m_rowMissionPlan.frameBinding = m_binding;
    for (auto& step : m_rowMissionPlan.steps) step.primitivePlan.frameBinding = m_binding;
    m_coordinator->setBinding(m_binding);
    setRouteStatusText(tr("地图坐标已确认，请保存地图后上传新任务"));
}
