#include "map.h"

#include "mapgraphicsview.h"
#include "ui_mainwindow.h"
#include "configmanager.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
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
#include <QQueue>
#include <QSplitter>
#include <QMenu>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector2D>
#include <QSlider>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QtMath>

namespace {
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
constexpr double kPointSelectionThresholdMeters = 0.5;
constexpr int kDefaultRouteLoopIntervalSeconds = 3;
constexpr double kArcSagittaEpsilon = 1e-3;
constexpr double kGeometryEpsilon = 1e-6;
constexpr int kMinArcSegments = 24;

inline double normalizeAngle(double angle)
{
    angle = std::fmod(angle + M_PI, 2.0 * M_PI);
    if (angle < 0.0) {
        angle += 2.0 * M_PI;
    }
    return angle - M_PI;
}

inline double mapToStandardAngle(double angle)
{
    return normalizeAngle(angle - M_PI_2);
}

inline double standardToMapAngle(double angle)
{
    return normalizeAngle(angle + M_PI_2);
}

inline QPointF mapToStandardPoint(const QPointF &mapPoint)
{
    return QPointF(mapPoint.y(), -mapPoint.x());
}

inline QPointF standardToMapPoint(const QPointF &stdPoint)
{
    return QPointF(-stdPoint.y(), stdPoint.x());
}


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
inline bool pointsAlmostEqual(const QPointF &a, const QPointF &b)
{
    return std::abs(a.x() - b.x()) < kGeometryEpsilon && std::abs(a.y() - b.y()) < kGeometryEpsilon;
}

inline QString formatNumber(double value, int precision = 3)
{
    return QString::number(value, 'f', precision);
}

inline double polylineLength(const QList<QPointF> &polyline)
{
    if (polyline.size() < 2) {
        return 0.0;
    }
    double length = 0.0;
    for (int i = 1; i < polyline.size(); ++i) {
        const QPointF &a = polyline.at(i - 1);
        const QPointF &b = polyline.at(i);
        length += std::hypot(b.x() - a.x(), b.y() - a.y());
    }
    return length;
}

} // namespace

QString Map::s_lastMapFilePath;

Map::Map(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
{
    m_cellSizeMeters = kCellSizeMeters;
    m_cellSizePixels = 50.0;

    initializeUi();
    ensureScene();
    ensureVehicleItem();
    handleModuleActivated();
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
        applyPresentationUpdates(true);

        m_currentMapFilePath.clear();
        updateMapNameDisplay();
        updateVehiclePointBinding();

        m_initialized = true;
        setRouteStatusText(tr("Grid reset to %1x%2 (rotation %3°, base lat %4, lon %5)")
                               .arg(m_gridWidth)
                               .arg(m_gridHeight)
                               .arg(m_mapRotationDeg, 0, 'f', 1)
                               .arg(m_baseLatitudeDeg, 0, 'f', 6)
                               .arg(m_baseLongitudeDeg, 0, 'f', 6));
    }

    applyVehiclePoseFromUi();
}

void Map::updateVehiclePose(double x, double y, double theta)
{
    m_vehiclePoseX = x;
    m_vehiclePoseY = y;
    m_vehiclePoseTheta = normalizeAngle(theta);
    m_hasVehiclePose = true;
    refreshVehicleGraphics();
    updateVehiclePointBinding();
}

void Map::applyVehiclePoseFromUi()
{
    if (!ui || !ui->lineEdit_Position) {
        return;
    }

    const QString rawText = ui->lineEdit_Position->text().trimmed();
    if (rawText.isEmpty()) {
        return;
    }

    const QStringList parts = rawText.split(',', Qt::SkipEmptyParts);
    if (parts.size() < 3) {
        return;
    }

    auto parseComponent = [](const QString &component) -> std::optional<double> {
        const int equalsIndex = component.indexOf('=');
        if (equalsIndex < 0) {
            return std::nullopt;
        }
        QString valueStr = component.mid(equalsIndex + 1).trimmed();
        valueStr.remove(QChar(0x00B0));
        bool ok = false;
        const double value = valueStr.toDouble(&ok);
        if (!ok) {
            return std::nullopt;
        }
        return value;
    };

    const auto xOpt = parseComponent(parts.at(0));
    const auto yOpt = parseComponent(parts.at(1));
    const auto thetaOpt = parseComponent(parts.at(2));

    if (!xOpt || !yOpt || !thetaOpt) {
        return;
    }

    updateVehiclePose(xOpt.value(), yOpt.value(), thetaOpt.value());
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

void Map::handleRouteSegmentCompleted(bool success)
{
    if (!m_waitingForSegmentCompletion) {
        return;
    }

    m_waitingForSegmentCompletion = false;

    if (!success) {
        setRouteStatusText(tr("路线执行失败，已停止"), true);
        m_activeRouteIndex = -1;
        m_pauseRequested = false;
        updateRouteControlState();
        emit routeExecutionCancelled();
        return;
    }

    if (m_activeRouteIndex >= 0 && m_activeRouteIndex < m_routeQueue.size()) {
        const RouteStep &step = m_routeQueue.at(m_activeRouteIndex);
        m_vehicleCurrentPointId = step.toId;
        if (const MapPoint *point = pointById(step.toId)) {
            m_vehiclePoseX = point->mapPosition.x();
            m_vehiclePoseY = point->mapPosition.y();
            m_vehiclePoseTheta = point->theta;
            refreshVehicleGraphics();
        }
    }

    ++m_activeRouteIndex;

    if (m_pauseRequested) {
        updateRouteControlState();
        return;
    }

    if (m_activeRouteIndex >= m_routeQueue.size()) {
        m_activeRouteIndex = -1;
        updateRouteControlState();
        emit routeQueueCompletedOnce();
        if (m_routeLoopCheck && m_routeLoopCheck->isChecked()) {
            scheduleNextCycle();
        }
        return;
    }

    dispatchNextEdge();
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
    if (m_activeRouteIndex >= m_routeQueue.size()) {
        m_activeRouteIndex = m_routeQueue.size() - 1;
    }

    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleRouteClear()
{
    if (m_routeQueue.isEmpty()) {
        return;
    }

    m_routeQueue.clear();
    resetRouteProgress();
    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleRouteStart()
{
    if (m_routeQueue.isEmpty()) {
        QMessageBox::information(m_mapPage, tr("发送路线"), tr("请先添加需要发送的路线"));
        return;
    }

    if (!isRouteQueueContinuous()) {
        QMessageBox::warning(m_mapPage, tr("发送路线"), tr("路线不连续，请检查起止点"));
        return;
    }

    if (!m_vehicleCurrentPointId.has_value()) {
        QMessageBox::warning(m_mapPage, tr("发送路线"), tr("无法确定小车当前所在点，请先定位"));
        return;
    }

    const int requiredStart = m_routeQueue.first().fromId;
    if (m_vehicleCurrentPointId.value() != requiredStart) {
        QMessageBox::warning(m_mapPage, tr("发送路线"),
                             tr("首段路线起点为点%1，请先将小车定位到该点").arg(requiredStart));
        return;
    }

    if (m_waitingForSegmentCompletion) {
        return;
    }

    if (m_routeTimer) {
        m_routeTimer->stop();
    }

    m_pauseRequested = false;
    resetRouteProgress();
    m_activeRouteIndex = 0;
    dispatchNextEdge();
}

void Map::handleRoutePause()
{
    if (m_routeQueue.isEmpty()) {
        return;
    }

    m_pauseRequested = true;
    if (!m_waitingForSegmentCompletion) {
    } else {
    }
    updateRouteControlState();
    emit routeExecutionPauseRequested();
}

void Map::handleRouteResume()
{
    if (m_routeQueue.isEmpty()) {
        QMessageBox::information(m_mapPage, tr("恢复路线"), tr("路线队列为空"));
        return;
    }

    m_pauseRequested = false;
    updateRouteControlState();
    emit routeExecutionResumeRequested();

    if (!m_waitingForSegmentCompletion) {
        dispatchNextEdge();
    }
}

void Map::handleRouteStop()
{
    if (m_routeTimer) {
        m_routeTimer->stop();
    }

    m_pauseRequested = false;
    m_waitingForSegmentCompletion = false;
    m_activeRouteIndex = -1;
    m_routeQueue.clear();
    refreshRouteQueueUi();
    updateRouteControlState();
    setRouteStatusText(tr("已停止并清空路线"), true);
    emit routeExecutionCancelled();
}

void Map::handleRouteTimerTick()
{
    if (m_routeQueue.isEmpty()) {
        updateRouteControlState();
        return;
    }

    resetRouteProgress();
    m_pauseRequested = false;
    dispatchNextEdge();
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
        return;
    }

    if (loadMapFromFile(filePath)) {
        m_lastLoadDirectory = QFileInfo(filePath).absolutePath();
    }
}

void Map::handleSaveMap()
{
    const QString startDir = !m_lastSaveDirectory.isEmpty() ? m_lastSaveDirectory
                              : (!m_currentMapFilePath.isEmpty() ? QFileInfo(m_currentMapFilePath).absolutePath()
                                 : (!s_lastMapFilePath.isEmpty() ? QFileInfo(s_lastMapFilePath).absolutePath()
                                                                 : QDir::currentPath()));

    const QString filePath = QFileDialog::getSaveFileName(m_mapPage, tr("保存地图"), startDir,
                                                          tr("地图文件 (*.json);;所有文件 (*.*)"));
    if (filePath.isEmpty()) {
        return;
    }

    if (saveMapToFile(filePath)) {
        m_currentMapFilePath = filePath;
        s_lastMapFilePath = filePath;
        m_lastSaveDirectory = QFileInfo(filePath).absolutePath();
        updateMapNameDisplay();
    }
}

void Map::handleQuickSave()
{
    QString filePath = m_currentMapFilePath;
    if (filePath.isEmpty()) {
        handleSaveMap();
        return;
    }

    if (saveMapToFile(filePath)) {
        s_lastMapFilePath = filePath;
        m_lastSaveDirectory = QFileInfo(filePath).absolutePath();
        updateMapNameDisplay();
    }
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
    mainSplitter->setChildrenCollapsible(false);
    rootLayout->addWidget(mainSplitter);

    auto leftPanel = new QFrame(m_mapPage);
    leftPanel->setFrameShape(QFrame::StyledPanel);
    leftPanel->setMinimumWidth(260);
    auto leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(8);

    mainSplitter->addWidget(leftPanel);

    auto gridBox = new QGroupBox(tr("地图尺寸"), leftPanel);
    gridBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto gridForm = new QFormLayout(gridBox);
    gridForm->setContentsMargins(8, 8, 8, 8);
    gridForm->setSpacing(6);

    m_gridWidthSpin = new QSpinBox(gridBox);
    m_gridWidthSpin->setRange(1, kMaxGridSize);
    m_gridWidthSpin->setValue(kDefaultGridWidth);
    gridForm->addRow(tr("宽度(格)"), m_gridWidthSpin);

    m_gridHeightSpin = new QSpinBox(gridBox);
    m_gridHeightSpin->setRange(1, kMaxGridSize);
    m_gridHeightSpin->setValue(kDefaultGridHeight);
    gridForm->addRow(tr("高度(格)"), m_gridHeightSpin);

    m_gridRotationSpin = new QDoubleSpinBox(gridBox);
    m_gridRotationSpin->setRange(-360.0, 360.0);
    m_gridRotationSpin->setDecimals(1);
    m_gridRotationSpin->setSingleStep(1.0);
    m_gridRotationSpin->setSuffix(QStringLiteral("°"));
    m_gridRotationSpin->setValue(0.0);
    gridForm->addRow(tr("旋转(°)"), m_gridRotationSpin);

    m_buildGridButton = new QPushButton(tr("Update Size"), gridBox);
    gridForm->addRow(QString(), m_buildGridButton);

    leftLayout->addWidget(gridBox);

    auto pointBox = new QGroupBox(tr("点管理"), leftPanel);
    pointBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto pointLayout = new QVBoxLayout(pointBox);
    pointLayout->setContentsMargins(8, 8, 8, 8);
    pointLayout->setSpacing(6);

    auto pointForm = new QFormLayout();
    pointForm->setContentsMargins(0, 0, 0, 0);
    pointForm->setSpacing(4);

    m_pointXSpin = new QDoubleSpinBox(pointBox);
    m_pointXSpin->setRange(-10000.0, 10000.0);
    m_pointXSpin->setDecimals(3);
    m_pointXSpin->setSingleStep(0.1);
    pointForm->addRow(tr("X (m)"), m_pointXSpin);

    m_pointYSpin = new QDoubleSpinBox(pointBox);
    m_pointYSpin->setRange(-10000.0, 10000.0);
    m_pointYSpin->setDecimals(3);
    m_pointYSpin->setSingleStep(0.1);
    pointForm->addRow(tr("Y (m)"), m_pointYSpin);

    m_pointThetaSpin = new QDoubleSpinBox(pointBox);
    m_pointThetaSpin->setRange(-M_PI, M_PI);
    m_pointThetaSpin->setDecimals(4);
    m_pointThetaSpin->setSingleStep(0.1);
    pointForm->addRow(tr("角度 (rad)"), m_pointThetaSpin);

    pointLayout->addLayout(pointForm);

    auto pointButtonLayout = new QHBoxLayout();
    pointButtonLayout->setSpacing(4);

    m_addPointFromInputButton = new QPushButton(tr("添加"), pointBox);
    pointButtonLayout->addWidget(m_addPointFromInputButton);

    m_addPointFromClickButton = new QPushButton(tr("点击添加"), pointBox);
    m_addPointFromClickButton->setCheckable(true);
    pointButtonLayout->addWidget(m_addPointFromClickButton);

    m_updatePointButton = new QPushButton(tr("更新"), pointBox);
    pointButtonLayout->addWidget(m_updatePointButton);

    m_removePointButton = new QPushButton(tr("删除"), pointBox);
    pointButtonLayout->addWidget(m_removePointButton);

    pointLayout->addLayout(pointButtonLayout);

    m_pointTable = new QTableWidget(pointBox);
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

    leftLayout->addWidget(pointBox, 2);

    auto batchBox = new QGroupBox(tr("批量生成点"), leftPanel);
    batchBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto batchLayout = new QFormLayout(batchBox);
    batchLayout->setContentsMargins(8, 8, 8, 8);
    batchLayout->setSpacing(6);

    m_batchBasePointCombo = new QComboBox(batchBox);
    batchLayout->addRow(tr("基准点"), m_batchBasePointCombo);

    m_batchDirectionCombo = new QComboBox(batchBox);
    m_batchDirectionCombo->addItem(tr("+X 方向 (向上)"));
    m_batchDirectionCombo->addItem(tr("-X 方向 (向下)"));
    m_batchDirectionCombo->addItem(tr("+Y 方向 (向右)"));
    m_batchDirectionCombo->addItem(tr("-Y 方向 (向左)"));
    batchLayout->addRow(tr("方向"), m_batchDirectionCombo);

    m_batchCountSpin = new QSpinBox(batchBox);
    m_batchCountSpin->setRange(1, 500);
    m_batchCountSpin->setValue(5);
    batchLayout->addRow(tr("数量"), m_batchCountSpin);

    m_batchSpacingSpin = new QDoubleSpinBox(batchBox);
    m_batchSpacingSpin->setRange(0.0, 1000.0);
    m_batchSpacingSpin->setSingleStep(0.1);
    m_batchSpacingSpin->setDecimals(3);
    m_batchSpacingSpin->setValue(1.0);
    batchLayout->addRow(tr("间隔 (m)"), m_batchSpacingSpin);

    m_batchThetaSpin = new QDoubleSpinBox(batchBox);
    m_batchThetaSpin->setRange(-M_PI, M_PI);
    m_batchThetaSpin->setDecimals(4);
    m_batchThetaSpin->setSingleStep(0.1);
    batchLayout->addRow(tr("方向角 (rad)"), m_batchThetaSpin);

    m_batchGenerateButton = new QPushButton(tr("生成"), batchBox);
    batchLayout->addRow(QString(), m_batchGenerateButton);

    leftLayout->addWidget(batchBox);

    auto pathBox = new QGroupBox(tr("路径管理"), leftPanel);
    pathBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto pathLayout = new QVBoxLayout(pathBox);
    pathLayout->setContentsMargins(8, 8, 8, 8);
    pathLayout->setSpacing(6);

    auto pathForm = new QFormLayout();
    pathForm->setContentsMargins(0, 0, 0, 0);
    pathForm->setSpacing(4);

    m_startPointCombo = new QComboBox(pathBox);
    pathForm->addRow(tr("起点"), m_startPointCombo);

    m_endPointCombo = new QComboBox(pathBox);
    pathForm->addRow(tr("终点"), m_endPointCombo);

    m_pathTypeCombo = new QComboBox(pathBox);
    m_pathTypeCombo->addItem(tr("直线"));
    m_pathTypeCombo->addItem(tr("圆弧"));
    pathForm->addRow(tr("类型"), m_pathTypeCombo);

    m_arcSagittaSpin = new QDoubleSpinBox(pathBox);
    m_arcSagittaSpin->setRange(-1000.0, 1000.0);
    m_arcSagittaSpin->setDecimals(3);
    m_arcSagittaSpin->setSingleStep(0.1);
    m_arcSagittaSpin->setValue(1.0);
    pathForm->addRow(tr("弓高 (m)"), m_arcSagittaSpin);

    pathLayout->addLayout(pathForm);

    auto pathButtonLayout = new QHBoxLayout();
    pathButtonLayout->setSpacing(4);

    m_addPathButton = new QPushButton(tr("添加"), pathBox);
    pathButtonLayout->addWidget(m_addPathButton);

    m_removePathButton = new QPushButton(tr("删除"), pathBox);
    pathButtonLayout->addWidget(m_removePathButton);

    pathLayout->addLayout(pathButtonLayout);

    m_pathTable = new QTableWidget(pathBox);
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

    leftLayout->addWidget(pathBox, 3);

    mainSplitter->addWidget(leftPanel);

    auto centerPanel = new QFrame(m_mapPage);
    centerPanel->setFrameShape(QFrame::StyledPanel);
    auto centerLayout = new QVBoxLayout(centerPanel);
    centerLayout->setContentsMargins(8, 8, 8, 8);
    centerLayout->setSpacing(8);

    auto topBar = new QHBoxLayout();
    topBar->setSpacing(6);

    m_mapNameLabel = new QLabel(tr("未保存地图"), centerPanel);
    m_mapNameLabel->setMinimumWidth(160);
    topBar->addWidget(m_mapNameLabel);

    m_mousePositionLabel = new QLabel(tr("X: 0.000  Y: 0.000"), centerPanel);
    m_mousePositionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_mousePositionLabel->setMinimumWidth(120);
    topBar->addWidget(m_mousePositionLabel, 1);

    m_loadMapButton = new QPushButton(tr("加载…"), centerPanel);
    topBar->addWidget(m_loadMapButton);

    m_saveMapButton = new QPushButton(tr("另存为…"), centerPanel);
    topBar->addWidget(m_saveMapButton);

    m_quickSaveButton = new QPushButton(tr("保存"), centerPanel);
    topBar->addWidget(m_quickSaveButton);

    topBar->addStretch();

    m_editModeButton = new QPushButton(tr("编辑模式"), centerPanel);
    m_editModeButton->setCheckable(true);
    m_editModeButton->setChecked(true);
    topBar->addWidget(m_editModeButton);

    m_locateButton = new QPushButton(tr("定位"), centerPanel);
    topBar->addWidget(m_locateButton);

    centerLayout->addLayout(topBar);

    m_view = new MapGraphicsView(centerPanel);
    m_view->setMinimumSize(640, 480);
    m_view->viewport()->setCursor(Qt::CrossCursor);
    m_view->viewport()->installEventFilter(this);
    centerLayout->addWidget(m_view, 1);

    m_rotationSlider = new QSlider(Qt::Horizontal, centerPanel);
    m_rotationSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rotationSlider->setRange(-1800, 1800);
    m_rotationSlider->setSingleStep(5);
    m_rotationSlider->setPageStep(30);
    m_rotationSlider->setValue(0);
    m_rotationSlider->setToolTip(tr("地图旋转控制"));
    centerLayout->addWidget(m_rotationSlider);

    mainSplitter->addWidget(centerPanel);

    auto rightPanel = new QFrame(m_mapPage);
    rightPanel->setFrameShape(QFrame::StyledPanel);
    rightPanel->setMinimumWidth(260);
    auto rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(8);

    auto routeBox = new QGroupBox(tr("路线发送"), rightPanel);
    auto routeLayout = new QVBoxLayout(routeBox);
    routeLayout->setContentsMargins(8, 8, 8, 8);
    routeLayout->setSpacing(6);

    auto routeForm = new QFormLayout();
    routeForm->setContentsMargins(0, 0, 0, 0);
    routeForm->setSpacing(4);

    m_routeStartCombo = new QComboBox(routeBox);
    routeForm->addRow(tr("起点"), m_routeStartCombo);

    m_routeEndCombo = new QComboBox(routeBox);
    routeForm->addRow(tr("终点"), m_routeEndCombo);

    routeLayout->addLayout(routeForm);

    m_routeAddButton = new QPushButton(tr("添加路线"), routeBox);
    routeLayout->addWidget(m_routeAddButton);

    m_routeQueueList = new QListWidget(routeBox);
    m_routeQueueList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_routeQueueList->setAlternatingRowColors(true);
    routeLayout->addWidget(m_routeQueueList, 1);

    auto queueButtons = new QHBoxLayout();
    queueButtons->setSpacing(4);
    m_routeRemoveButton = new QPushButton(tr("移除"), routeBox);
    queueButtons->addWidget(m_routeRemoveButton);
    m_routeClearButton = new QPushButton(tr("清空"), routeBox);
    queueButtons->addWidget(m_routeClearButton);
    queueButtons->addStretch();
    routeLayout->addLayout(queueButtons);

    auto controlButtons = new QHBoxLayout();
    controlButtons->setSpacing(4);
    m_routeStartButton = new QPushButton(tr("开始"), routeBox);
    controlButtons->addWidget(m_routeStartButton);
    m_routePauseButton = new QPushButton(tr("暂停"), routeBox);
    controlButtons->addWidget(m_routePauseButton);
    m_routeResumeButton = new QPushButton(tr("恢复"), routeBox);
    controlButtons->addWidget(m_routeResumeButton);
    m_routeStopButton = new QPushButton(tr("停止"), routeBox);
    controlButtons->addWidget(m_routeStopButton);
    routeLayout->addLayout(controlButtons);

    auto loopLayout = new QHBoxLayout();
    loopLayout->setSpacing(4);
    m_routeLoopCheck = new QCheckBox(tr("循环发送"), routeBox);
    loopLayout->addWidget(m_routeLoopCheck);
    m_routeLoopIntervalSpin = new QSpinBox(routeBox);
    m_routeLoopIntervalSpin->setRange(0, 3600);
    m_routeLoopIntervalSpin->setSuffix(tr(" 秒"));
    m_routeLoopIntervalSpin->setValue(kDefaultRouteLoopIntervalSeconds);
    m_routeLoopIntervalSpin->setEnabled(false);
    loopLayout->addWidget(m_routeLoopIntervalSpin);
    loopLayout->addStretch();
    routeLayout->addLayout(loopLayout);

    m_routeStatusLabel = new QLabel(tr("待命"), routeBox);
    m_routeStatusLabel->setWordWrap(true);
    m_routeStatusLabel->setMinimumHeight(40);
    QPalette statusPalette = m_routeStatusLabel->palette();
    statusPalette.setColor(QPalette::WindowText, QColor(55, 55, 55));
    m_routeStatusLabel->setPalette(statusPalette);
    routeLayout->addWidget(m_routeStatusLabel);

    rightLayout->addWidget(routeBox, 1);
    rightLayout->addStretch();

    mainSplitter->addWidget(rightPanel);

    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);
    mainSplitter->setCollapsible(0, true);
    mainSplitter->setCollapsible(2, true);
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

    handleEditModeToggled(m_editModeButton && m_editModeButton->isChecked());

    if (!m_routeTimer) {
        m_routeTimer = new QTimer(this);
    }
    m_routeTimer->setSingleShot(true);
    connect(m_routeTimer, &QTimer::timeout, this, &Map::handleRouteTimerTick);
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

    QQueue<int> queue;
    QSet<int> visited;
    QHash<int, int> cameFromPath;

    queue.enqueue(startId);
    visited.insert(startId);

    while (!queue.isEmpty()) {
        const int current = queue.dequeue();
        if (current == endId) {
            break;
        }

        const QList<int> outgoing = m_outgoingPathIds.value(current);
        for (int pathId : outgoing) {
            const MapPath *path = pathById(pathId);
            if (!path) {
                continue;
            }
            const int nextPoint = path->endId;
            if (visited.contains(nextPoint)) {
                continue;
            }
            visited.insert(nextPoint);
            cameFromPath.insert(nextPoint, pathId);
            queue.enqueue(nextPoint);
        }
    }

    if (!cameFromPath.contains(endId)) {
        return result;
    }

    int current = endId;
    while (current != startId) {
        const int pathId = cameFromPath.value(current);
        result.prepend(pathId);
        const MapPath *path = pathById(pathId);
        if (!path) {
            result.clear();
            return result;
        }
        current = path->startId;
    }

    return result;
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
    const bool hasQueue = !m_routeQueue.isEmpty();
    const bool running = m_waitingForSegmentCompletion || (m_activeRouteIndex >= 0 && m_activeRouteIndex < m_routeQueue.size());

    if (m_routeStartButton) {
        m_routeStartButton->setEnabled(hasQueue && !running && !m_pauseRequested);
    }
    if (m_routePauseButton) {
        m_routePauseButton->setEnabled(running && !m_pauseRequested);
    }
    if (m_routeResumeButton) {
        m_routeResumeButton->setEnabled(hasQueue && !running && m_pauseRequested);
    }
    if (m_routeStopButton) {
        m_routeStopButton->setEnabled(hasQueue || running);
    }
    if (m_routeRemoveButton) {
        m_routeRemoveButton->setEnabled(hasQueue && !running);
    }
    if (m_routeClearButton) {
        m_routeClearButton->setEnabled(hasQueue && !running);
    }
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

void Map::resetRouteProgress()
{
    for (RouteStep &step : m_routeQueue) {
        step.progressEdgeIndex = 0;
    }
    m_activeRouteIndex = -1;
    m_waitingForSegmentCompletion = false;
}

void Map::dispatchNextEdge()
{
    if (m_routeQueue.isEmpty()) {
        m_activeRouteIndex = -1;
        m_waitingForSegmentCompletion = false;
        updateRouteControlState();
        return;
    }

    if (m_pauseRequested) {
        updateRouteControlState();
        return;
    }

    if (m_activeRouteIndex < 0 || m_activeRouteIndex >= m_routeQueue.size()) {
        m_activeRouteIndex = 0;
    }

    while (m_activeRouteIndex < m_routeQueue.size()) {
        RouteStep &step = m_routeQueue[m_activeRouteIndex];
        if (step.pathIds.isEmpty()) {
            if (!rebuildRouteStep(step)) {
                setRouteStatusText(tr("点%1至点%2没有可用路径").arg(step.fromId).arg(step.toId), true);
                m_activeRouteIndex = -1;
                updateRouteControlState();
                return;
            }
        }

        const QList<QPointF> polyline = composePolyline(step.pathIds);
        if (polyline.isEmpty()) {
            setRouteStatusText(tr("路线数据无效"), true);
            m_activeRouteIndex = -1;
            updateRouteControlState();
            return;
        }

        double startTheta = 0.0;
        if (const MapPoint *startPoint = pointById(step.fromId)) {
            startTheta = startPoint->theta;
        }
        double endTheta = 0.0;
        if (const MapPoint *endPoint = pointById(step.toId)) {
            endTheta = endPoint->theta;
        }

        setRouteStatusText(tr("发送: 点%1 → 点%2").arg(step.fromId).arg(step.toId));
        m_waitingForSegmentCompletion = true;
        updateRouteControlState();
        emit routeSegmentDispatched(step.fromId, step.toId, polyline, startTheta, endTheta);
        return;
    }

    m_waitingForSegmentCompletion = false;
    m_activeRouteIndex = -1;
    updateRouteControlState();
    emit routeQueueCompletedOnce();
    if (m_routeLoopCheck && m_routeLoopCheck->isChecked()) {
        scheduleNextCycle();
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

void Map::scheduleNextCycle()
{
    if (!m_routeLoopCheck || !m_routeLoopCheck->isChecked() || !m_routeTimer) {
        updateRouteControlState();
        return;
    }

    const int intervalSec = m_routeLoopIntervalSpin ? m_routeLoopIntervalSpin->value() : 0;
    if (intervalSec <= 0) {
        resetRouteProgress();
        dispatchNextEdge();
        return;
    }

    m_routeTimer->start(intervalSec * 1000);
    setRouteStatusText(tr("等待下一轮 (%1 秒)").arg(intervalSec));
    updateRouteControlState();
}

void Map::clearMapData()
{
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
    resetRouteProgress();

    refreshPointUi();
    refreshPathUi();
    refreshSelectors();
    refreshRouteQueueUi();
    updateRouteControlState();
}

bool Map::saveMapToFile(const QString &filePath) const
{
    if (filePath.isEmpty()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(m_mapPage, tr("保存地图"), tr("无法写入文件: %1").arg(file.errorString()));
        return false;
    }

    QJsonDocument doc(serializeMap());
    file.write(doc.toJson(QJsonDocument::Indented));
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
    return true;
}

QJsonObject Map::serializeMap() const
{
    QJsonObject root;
    root.insert(QStringLiteral("gridWidth"), m_gridWidth);
    root.insert(QStringLiteral("gridHeight"), m_gridHeight);
    root.insert(QStringLiteral("cellSizeMeters"), m_cellSizeMeters);
    root.insert(QStringLiteral("rotationDeg"), m_mapRotationDeg);

    QJsonArray pointsArray;
    for (auto it = m_points.cbegin(); it != m_points.cend(); ++it) {
        const MapPoint &point = it.value();
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), point.id);
        obj.insert(QStringLiteral("x"), point.mapPosition.x());
        obj.insert(QStringLiteral("y"), point.mapPosition.y());
        obj.insert(QStringLiteral("theta"), point.theta);
        pointsArray.append(obj);
    }
    root.insert(QStringLiteral("points"), pointsArray);

    QJsonArray pathsArray;
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        const MapPath &path = it.value();
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), path.id);
        obj.insert(QStringLiteral("start"), path.startId);
        obj.insert(QStringLiteral("end"), path.endId);
        obj.insert(QStringLiteral("type"), path.type == PathType::Line ? QStringLiteral("line")
                                                                        : QStringLiteral("arc"));
        obj.insert(QStringLiteral("sagitta"), path.sagitta);
        pathsArray.append(obj);
    }
    root.insert(QStringLiteral("paths"), pathsArray);

    return root;
}

bool Map::deserializeMap(const QJsonObject &object)
{
    if (!object.contains(QStringLiteral("gridWidth")) || !object.contains(QStringLiteral("gridHeight"))) {
        return false;
    }

    const int width = object.value(QStringLiteral("gridWidth")).toInt();
    const int height = object.value(QStringLiteral("gridHeight")).toInt();
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (m_gridWidthSpin) {
        m_gridWidthSpin->setValue(width);
    }
    if (m_gridHeightSpin) {
        m_gridHeightSpin->setValue(height);
    }
    m_gridWidth = width;
    m_gridHeight = height;

    const double rotationDeg = object.value(QStringLiteral("rotationDeg")).toDouble(0.0);
    setMapRotation(rotationDeg);

    clearMapData();

    const QJsonArray pointsArray = object.value(QStringLiteral("points")).toArray();
    int maxPointId = 0;
    for (const QJsonValue &value : pointsArray) {
        const QJsonObject obj = value.toObject();
        const int id = obj.value(QStringLiteral("id")).toInt();
        const double x = obj.value(QStringLiteral("x")).toDouble();
        const double y = obj.value(QStringLiteral("y")).toDouble();
        const double theta = obj.value(QStringLiteral("theta")).toDouble();
        if (addPointInternal(x, y, theta, id)) {
            maxPointId = std::max(maxPointId, id);
        }
    }

    const QJsonArray pathsArray = object.value(QStringLiteral("paths")).toArray();
    int maxPathId = 0;
    for (const QJsonValue &value : pathsArray) {
        const QJsonObject obj = value.toObject();
        const int id = obj.value(QStringLiteral("id")).toInt();
        const int startId = obj.value(QStringLiteral("start")).toInt();
        const int endId = obj.value(QStringLiteral("end")).toInt();
        const QString type = obj.value(QStringLiteral("type")).toString();
        const double sagitta = obj.value(QStringLiteral("sagitta")).toDouble();
        bool ok = false;
        if (type == QStringLiteral("arc")) {
            ok = addArcPath(startId, endId, sagitta, id);
        } else {
            ok = addLinePath(startId, endId, id);
        }
        if (ok) {
            maxPathId = std::max(maxPathId, id);
        }
    }

    m_nextPointId = maxPointId + 1;
    m_nextPathId = maxPathId + 1;

    applyPresentationUpdates(true);
    refreshPointUi();
    refreshPathUi();
    refreshSelectors();
    refreshRouteQueueUi();
    updateRouteControlState();

    return true;
}















































