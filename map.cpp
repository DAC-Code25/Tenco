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
#include "supportpagestyle.h"
#include "parameterwheelguard.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
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
#include <QCursor>
#include <QPen>
#include <QPushButton>
#include <QUrl>
#include <QScrollArea>
#include <QScopedValueRollback>
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
constexpr double kVehicleLengthMeters = 0.80;
constexpr double kVehicleWidthMeters = 0.30;
constexpr double kRouteReplanSnapThresholdMeters = 1.0;
constexpr double kPointSelectionThresholdMeters = 0.5;
constexpr int kDefaultRouteLoopIntervalSeconds = 3;
constexpr int kDefaultMapPointDwellMs = 2000;
constexpr double kArcSagittaEpsilon = 1e-3;
constexpr double kGeometryEpsilon = 1e-6;
constexpr int kMinArcSegments = 24;
constexpr int kMapSchemaVersion = 3;
constexpr int kMaxMapSchemaVersion = 4;
const QColor kBidirectionalPathColor("#16a34a");
const QColor kSingleDirectionPathColor("#d97706");
const QColor kPlannedRouteColor("#2563eb");
const QColor kReturnPlanColor("#0891b2");
const QColor kLocalizationTrailColor("#a21caf");

QPen plannedRoutePen(bool returning = false)
{
    QPen pen(returning ? kReturnPlanColor : kPlannedRouteColor, 3.4,
             returning ? Qt::DashDotLine : Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
    pen.setCosmetic(true);
    return pen;
}

QByteArray localizationTrailFrame(const MapFrameBinding &binding)
{
    auto value = binding.toJson();
    auto context = value["context"].toObject();
    // Planning revisions do not move existing coordinates; coordinate changes do.
    context.remove("controllerProfileRevision");
    context.remove("mapRevision");
    value["context"] = context;
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

QJsonObject taskIdentity(const TrackingSnapshot &status)
{
    return {{"boot", status.bootId}, {"task", status.taskId}, {"revision", status.taskRevision},
            {"execution", status.executionId}};
}

QJsonObject checkpointIdentity(const TrackingSnapshot &status)
{
    auto identity = taskIdentity(status);
    identity["step"] = status.stepId;
    identity["event"] = status.waitingEventId;
    return identity;
}

double editedCoordinate(QDoubleSpinBox *editor, double original)
{
    if (!editor) return original;
    // An untouched editor displays a rounded number. Keep the underlying map
    // precision when updating another field or simply clicking "update".
    const double displayed = QString::number(original, 'f', editor->decimals()).toDouble();
    return editor->value() == displayed ? original : editor->value();
}

QList<QTableWidgetItem *> pointIdItems(QTableWidget *table, int id)
{
    for (int row = 0; row < table->rowCount(); ++row)
        if (auto *item = table->item(row, 0); item && item->text() == QString::number(id))
            return {item};
    return {};
}
constexpr int kRowWorkCheckpointTableNameColumn = 0;
constexpr int kRowWorkCheckpointTableProgressColumn = 1;
constexpr int kRowWorkCheckpointTableDwellColumn = 2;
constexpr int kRowWorkCheckpointTableForwardColumn = 3;
constexpr int kRowWorkCheckpointTableBackwardColumn = 4;
constexpr int kRowWorkCheckpointTableCaptureColumn = 5;
constexpr int kRowWorkCheckpointTableTimeoutColumn = 6;

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

    QDir().mkpath(mapsDirectory());
    initializeUi();
    ensureScene();
    ensureVehicleItem();
    m_localizationTrailRefreshTimer = new QTimer(this);
    m_localizationTrailRefreshTimer->setSingleShot(true);
    m_localizationTrailRefreshTimer->setInterval(100);
    connect(m_localizationTrailRefreshTimer, &QTimer::timeout, this, &Map::refreshLocalizationTrailGraphics);
    connect(&ConfigManager::instance(), &ConfigManager::configChanged, this, &Map::applyRuntimeConfig);
    handleModuleActivated();
    refreshPlanningRevision();
}

void Map::applyRuntimeConfig()
{
    if (m_tracking) m_tracking->invalidatePreparedPlan();
    refreshRowWorkControlState();
    setRowWorkStatusText(tr("地图运行配置已应用"));
}

void Map::handleModuleActivated()
{
    if (!m_initialized) {
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
        syncCommittedMapState();

        m_initialized = true;
        setRouteStatusText(tr("可离线编辑地图与路线；连接工控机后下发任务。"));
    }

}

void Map::updateVehiclePose(double x, double y, double theta)
{
    m_vehiclePoseX = x;
    m_vehiclePoseY = y;
    m_vehiclePoseTheta = normalizeAngle(theta);
    m_hasVehiclePose = true;
    // Move only the vehicle item for each accepted pose; Qt coalesces repaints.
    refreshVehicleGraphics();
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
    if (watched == m_mapNameLabel && event->type() == QEvent::Resize) {
        updateMapNameDisplay();
    }
    if (m_view && watched == m_view->viewport()) {
        if (!canEditRowWorkPlan()) {
            m_ctrlDragActive = m_altRotationActive = m_ctrlAltPathActive = false;
            m_draggedPointId = -1;
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            m_draggedPointMoved = false;
            m_draggedPointRotated = false;
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_editModeEnabled && canEditRowWorkPlan()) {
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
                            const int newId = addPathOrWarn(startId, endId, type, sagitta, std::nullopt,
                                m_pathDirectionCombo && m_pathDirectionCombo->currentData().toBool());
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
                                pointIdItems(m_pointTable, pointId);
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
                                pointIdItems(m_pointTable, pointId);
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
                const double newTheta = normalizeAngle(m_rotationStartTheta + delta);
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
    if (!canEditRowWorkPlan()) return;

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
        if (!chosen || !canEditRowWorkPlan() || !m_editModeEnabled) {
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
            } else {
                setRouteStatusText(tr("无法新增地图点：点或路径编号不可用，或新点与自动连接的上一点重合"), true);
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
            const bool added = addRowWorkCheckpoint(mapPos);
            m_rowWorkClickPlacementMode = false;
            if (m_rowWorkAddCheckpointFromMapButton) {
                QSignalBlocker blocker(m_rowWorkAddCheckpointFromMapButton);
                m_rowWorkAddCheckpointFromMapButton->setChecked(false);
            }
            if (added) setRowWorkStatusText(hasRowWorkLine() ? tr("已添加地图中间点")
                : tr("已记录中间点，可继续沿途添加，最后记录终点"));
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
            } else {
                setRouteStatusText(tr("无法新增地图点：点或路径编号不可用，或新点与自动连接的上一点重合"), true);
            }
            return;
        }

        double bestDist = 0.0;
        const int nearestId = findNearestPointId(mapPos, kPointSelectionThresholdMeters, &bestDist);

        if (nearestId != -1) {
            m_pendingPointSelection = nearestId;
            if (m_pointTable) {
                const QList<QTableWidgetItem *> items =
                    pointIdItems(m_pointTable, nearestId);
                if (!items.isEmpty()) {
                    const int row = items.first()->row();
                    m_pointTable->setCurrentCell(row, 0, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                }
            }
            // Selecting an existing point must keep its sampled coordinates.
            // The click can be anywhere inside the selection radius.
            handlePointSelectionChanged();
            return;
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

    setRouteStatusText(tr("地图尺寸已更新为 %1×%2，显示旋转 %3°")
                           .arg(width)
                           .arg(height)
                           .arg(m_mapRotationDeg, 0, 'f', 1));
}

void Map::handleAddPointFromInput()
{
    if (!canEditRowWorkPlan()) return;
    if (!m_pointXSpin || !m_pointYSpin || !m_pointThetaSpin) {
        return;
    }

    const double x = m_pointXSpin->value();
    const double y = m_pointYSpin->value();
    const double theta = m_pointThetaSpin->value();

    if (!addPointInternal(x, y, theta)) {
        QMessageBox::warning(m_mapPage, tr("添加点"), tr("无法添加该点：点或路径编号不可用，或新点与自动连接的上一点重合"));
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

    const auto *current = m_pointTable->currentItem();
    const int row = current && current->isSelected() ? current->row() : items.first()->row();
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
        if (m_pointDwellSpin) m_pointDwellSpin->setValue(point->dwellMs);
    }
}

void Map::handleUpdatePoint()
{
    if (!canEditRowWorkPlan()) return;
    if (!m_pendingPointSelection.has_value()) {
        QMessageBox::information(m_mapPage, tr("更新点"), tr("请先选择需要更新的点"));
        return;
    }

    const int pointId = m_pendingPointSelection.value();
    MapPoint *point = pointById(pointId);
    if (!point) {
        return;
    }

    point->mapPosition = QPointF(editedCoordinate(m_pointXSpin, point->mapPosition.x()),
                                editedCoordinate(m_pointYSpin, point->mapPosition.y()));
    point->theta = normalizeAngle(editedCoordinate(m_pointThetaSpin, point->theta));
    if (m_pointDwellSpin) point->dwellMs = m_pointDwellSpin->value();

    updatePointGraphics(*point);
    refreshPathsForPoint(pointId);
    refreshPointUi();
    refreshPathUi();
    refreshPlanningRevision();
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

    addPathOrWarn(startId, endId, type, sagitta, std::nullopt,
                  m_pathDirectionCombo && m_pathDirectionCombo->currentData().toBool());
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
    if (!canEditRowWorkPlan()) return;
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
    if (!canEditRowWorkPlan() || !m_routeStartCombo || !m_routeEndCombo) {
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
    m_routeQueue.append(step);

    refreshRouteQueueUi();
    if (m_routeQueueList) m_routeQueueList->setCurrentRow(m_routeQueue.size() - 1);
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
    if (!canEditRowWorkPlan() || !m_routeQueueList) {
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
    if (!canEditRowWorkPlan() || m_routeQueue.isEmpty()) {
        return;
    }

    m_routeQueue.clear();
    refreshRouteQueueUi();
    updateRouteControlState();
}

void Map::handleRouteStart()
{
    submitRouteTask();
}

void Map::submitRouteTask()
{
    refreshPlanningRevision();
    if (!m_coordinator || m_routeQueue.isEmpty() || !canEditRowWorkPlan()) return;
    QString error;
    const auto sections = buildRouteSections(&error);
    if (!error.isEmpty()) { setRouteStatusText(error, true); return; }
    auto options = taskOptions();
    options.repeatUntilStopped = m_routeLoopCheck && m_routeLoopCheck->isChecked();
    const auto compiled = TaskCompiler::route(sections, m_binding, options, tr("常规路线"));
    if (!compiled.ok()) { setRouteStatusText(compiled.error, true); return; }
    auto plan = compiled.plan;
    if (options.repeatUntilStopped && m_routeLoopIntervalSpin && m_routeLoopIntervalSpin->value() > 0) {
        auto steps = plan["steps"].toArray();
        if (steps.size() >= 1000) {
            setRouteStatusText(tr("任务步骤过多，添加循环间隔后将超过 1000 步，请减少点位或路线"), true);
            return;
        }
        steps.append(QJsonObject{{"stepId", "cycle-wait"}, {"type", "wait"}, {"completion",
            QJsonObject{{"type", "timer"}, {"durationMs", m_routeLoopIntervalSpin->value() * 1000}}}});
        plan["steps"] = steps;
    }
    if (m_coordinator->uploadAndStart(plan))
        setRouteStatusText(tr("正在下发完整路线，就绪后自动执行"));
}

QList<TaskRouteSection> Map::buildRouteSections(QString *error)
{
    if (error) error->clear();
    QList<TaskRouteSection> sections;
    for (auto step : m_routeQueue) {
        if (!rebuildRouteStep(step)) {
            if (error) *error = tr("路线中存在无法连通的步骤");
            return {};
        }
        int currentId = step.fromId;
        int sectionStart = currentId;
        QList<int> sectionPaths;
        for (int pathId : step.pathIds) {
            const auto *path = pathById(pathId);
            if (!path || (path->startId != currentId && (!path->bidirectional || path->endId != currentId))) {
                if (error) *error = tr("路线中存在无法连通的步骤");
                return {};
            }
            currentId = path->startId == currentId ? path->endId : path->startId;
            sectionPaths.append(pathId);
            const auto *end = pointById(currentId);
            if (end && (end->dwellMs > 0 || currentId == step.toId)) {
                TaskRouteSection section;
                section.id = QStringLiteral("route-%1-%2").arg(sectionStart).arg(currentId);
                section.points = composePolyline(sectionPaths, sectionStart);
                section.goalMapYaw = end->theta;
                section.dwellMs = end->dwellMs;
                // In a loop the return to A already performs A's dwell once
                // per lap. A second leading dwell would double it at the seam.
                if (sections.isEmpty() && !(m_routeLoopCheck && m_routeLoopCheck->isChecked())) {
                    if (const auto *start = pointById(step.fromId)) {
                        section.startDwellMs = start->dwellMs;
                        section.startMapYaw = start->theta;
                    }
                }
                sections.append(section);
                sectionStart = currentId;
                sectionPaths.clear();
            }
        }
    }
    return sections;
}

void Map::handleRouteResume()
{
    if (m_rowWorkPendingCaptureTarget != RowWorkPendingCaptureTarget::None) return;
    if (!m_coordinator || !m_tracking || !m_tracking->commandFresh() || m_tracking->busy()
        || m_coordinator->actionPending() || m_taskReviewOpen || !m_taskReviewIdentity.isEmpty()) return;
    refreshPlanningRevision();
    const auto status = m_tracking->snapshot();
    if (status.state != "Ready" && status.state != "Paused") return;
    if (!hasConfirmedTask()) {
        requestTaskReview();
        return;
    }
    if (status.state == "Ready") m_coordinator->startTask();
    else m_coordinator->resumeTask();
}

bool Map::hasConfirmedTask() const
{
    if (!m_tracking || !m_binding.isUsable()) return false;
    const auto plan = m_tracking->preparedPlan();
    const auto status = m_tracking->snapshot();
    return !status.taskId.isEmpty() && plan["taskId"].toString() == status.taskId
        && plan["revision"].toInt() == status.taskRevision
        && plan["context"].toObject() == m_binding.context.toJson();
}

void Map::requestTaskReview()
{
    const auto status = m_tracking->snapshot();
    if (status.taskId.isEmpty() || status.taskRevision < 1) return;
    m_taskReviewIdentity = taskIdentity(status);
    updateRouteControlState();
    setRouteStatusText(tr("正在读取工控机实际任务，核对不会触发运动"));
    if (!m_tracking->readTask(status.taskId, status.taskRevision)) {
        m_taskReviewIdentity = {};
        updateRouteControlState();
        setRouteStatusText(tr("上次任务读取尚未结束或连接已变化，请稍后重新点击核对"), true);
    }
}

void Map::handleLocateCurrentPosition()
{
    const auto source = m_poseClient ? m_poseClient->snapshot() : ControlPoseSnapshot{};
    const bool sourceCurrent = !m_poseClient || (source.validForControl && m_binding.isUsable() &&
        source.originRevision == m_binding.context.originRevision &&
        source.calibrationId == m_binding.context.calibrationId);
    if (!m_view || !m_hasVehiclePose || !sourceCurrent ||
        !std::isfinite(m_vehiclePoseX) || !std::isfinite(m_vehiclePoseY) ||
        !std::isfinite(m_vehiclePoseTheta)) {
        setRouteStatusText(tr("当前位置不可用，暂不能定位或新增地图点"), true);
        return;
    }

    const QPointF vehiclePos(m_vehiclePoseX, m_vehiclePoseY);
    m_view->centerOn(mapToScene(vehiclePos));
    if (m_rowWorkPendingCaptureTarget != RowWorkPendingCaptureTarget::None) {
        setRouteStatusText(tr("正在采样，请保持车辆停稳，完成后再记录下一点"));
        return;
    }
    if (!canEditRowWorkPlan()) {
        setRouteStatusText(tr("已定位到车辆；任务进行中，暂不新增地图点"));
        return;
    }
    if (!m_editModeEnabled) {
        setRouteStatusText(tr("已定位到车辆；开启编辑模式后可新增地图点"));
        return;
    }
    if (requestPoseCapture(RowWorkPendingCaptureTarget::MapPoint))
        setRouteStatusText(tr("正在采集停稳后的定位样本，完成后新增地图点…"));
    else
        setRouteStatusText(tr("采样未开始：请确认停稳、定位连接和地图绑定"), true);
}

void Map::handleEditModeToggled(bool checked)
{
    m_editModeEnabled = checked;
    if (m_editModeButton) {
        m_editModeButton->setText(checked ? tr("编辑中") : tr("编辑模式"));
        m_editModeButton->setToolTip(checked ? tr("地图编辑已启用，点击退出编辑")
                                            : tr("点击启用地图编辑"));
    }
    if (!checked) {
        if (m_rowWorkPendingCaptureTarget == RowWorkPendingCaptureTarget::MapPoint) {
            m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
            m_pendingCaptureBinding = {};
        }
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
    updateRouteControlState();
}

void Map::handleLoadMap()
{
    if (!canEditRowWorkPlan()) {
        setRouteStatusText(tr("任务正在准备、执行或暂停，暂不能切换地图"), true);
        return;
    }
    if (!ensureMapsDirectory()) return;
    const QString startDir = mapsDirectory();

    const QString filePath = QFileDialog::getOpenFileName(m_mapPage, tr("加载地图"), startDir,
                                                          tr("地图文件 (*.json);;所有文件 (*.*)"));
    if (filePath.isEmpty()) {
        LoggingManager::audit(QStringLiteral("map.load"),
                              QStringLiteral("cancelled"));
        return;
    }

    if (!canEditRowWorkPlan() || !confirmMapChanges(tr("加载地图")) || !canEditRowWorkPlan()) return;

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
    if (!canEditRowWorkPlan()) {
        setRouteStatusText(tr("任务正在准备、执行或暂停，暂不能新建地图"), true);
        return;
    }
    if (!confirmMapChanges(tr("新建地图")) || !canEditRowWorkPlan()) {
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
    ParameterWheelGuard::install(m_mapPage);
    SupportPageStyle::apply(m_mapPage, QStringLiteral(R"(
QFrame#mapLeftPanel, QFrame#mapRightPanel, QFrame#mapCenterPanel {
    background:white; border:1px solid #dfe7e3; border-radius:10px;
}
QGroupBox { background:transparent; border:1px solid #e4eae6; border-radius:8px;
    margin-top:12px; padding:12px 6px 6px; }
QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px;
    color:#24775c; font-weight:600; }
QPushButton { padding:4px 8px; min-height:18px; border-radius:6px; }
QPushButton#mapEditModeButton:checked {
    background:#24775c; color:white; border-color:#24775c; font-weight:600;
}
QPushButton#mapEditModeButton:checked:hover { background:#1e664e; border-color:#1e664e; }
QPushButton#mapEditModeButton:checked:pressed { background:#18563f; border-color:#18563f; }
QPushButton#mapEditModeButton:checked:disabled {
    background:#dce9e1; color:#668574; border-color:#c0d5c8;
}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { padding:4px 7px; min-height:18px; }
QListWidget, QTableWidget { background:white; border:1px solid #e4eae6;
    alternate-background-color:#f5f8f6; selection-background-color:#e3f0e8;
    selection-color:#18563c; border-radius:6px; }
QHeaderView::section { background:#f3f7f4; color:#526a5b; border:none;
    border-bottom:1px solid #e4eae6; padding:5px 2px; }
QTabBar::tab { padding:8px 10px; }
QLabel#mapTaskNoticeLabel { color:#718278; font-size:12px; }
QLabel[mapPlanningHint="true"] { color:#647a6c; font-size:12px; }
QGroupBox#mapTaskExecutionPanel { background:#f8fbf9; }
)"));

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

    auto editorTabs = new QTabWidget(leftPanel);
    editorTabs->setObjectName(QStringLiteral("mapEditorTabs"));
    leftPanelLayout->addWidget(editorTabs, 1);

    auto leftScrollArea = new QScrollArea(editorTabs);
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

    m_buildGridButton = new QPushButton(tr("更新尺寸"), gridBox);
    m_buildGridButton->setObjectName(QStringLiteral("mapBuildGridButton"));
    m_buildGridButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    gridForm->addRow(QString(), m_buildGridButton);

    m_frameBindingButton = new QPushButton(tr("坐标适配…"), gridBox);
    m_frameBindingButton->setObjectName(QStringLiteral("mapFrameBindingButton"));
    m_frameBindingButton->setToolTip(tr("为旧地图核实平移、旋转及坐标版本；新空白地图自动使用当前原点，此处不修改基站坐标。"));
    gridForm->addRow(QString(), m_frameBindingButton);
    connect(m_frameBindingButton, &QPushButton::clicked, this, &Map::confirmFrameBinding);


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

    m_pointDwellSpin = new QSpinBox(pointBox);
    m_pointDwellSpin->setObjectName(QStringLiteral("mapPointDwellSpin"));
    m_pointDwellSpin->setRange(0, 3600000);
    m_pointDwellSpin->setSingleStep(100);
    m_pointDwellSpin->setValue(kDefaultMapPointDwellMs);
    m_pointDwellSpin->setToolTip(tr("到点、对准并停稳后的停留时间，默认 2000 毫秒（2 秒）；0 表示不增加停留。"));
    configureSpinField(m_pointDwellSpin);
    pointForm->addRow(tr("停留 (ms)"), m_pointDwellSpin);

    pointLayout->addLayout(pointForm);

    m_autoConnectPointsCheck = new QCheckBox(tr("按序自动连接单向直线"), pointBox);
    m_autoConnectPointsCheck->setObjectName(QStringLiteral("mapAutoConnectPointsCheck"));
    m_autoConnectPointsCheck->setToolTip(tr("开启后，新添加的点自动与编号在前的上一点建立单向直线路径；适用于输入、地图点击、记录车辆点和批量生成。已有点和路径不改变，加载地图不自动补线。"));
    pointLayout->addWidget(m_autoConnectPointsCheck);

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
    m_pointTable->setColumnCount(5);
    m_pointTable->setHorizontalHeaderLabels({tr("编号"), tr("X"), tr("Y"), tr("角度"), tr("停留(ms)")});
    m_pointTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_pointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pointTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_pointTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    for (int col = 0; col < m_pointTable->columnCount(); ++col) {
        if (auto *item = m_pointTable->horizontalHeaderItem(col)) {
            item->setTextAlignment(Qt::AlignCenter);
        }
    }
    m_pointTable->horizontalHeader()->setStretchLastSection(false);
    m_pointTable->verticalHeader()->setVisible(false);
    m_pointTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pointTable->verticalHeader()->setDefaultSectionSize(28);
    m_pointTable->verticalHeader()->setMinimumSectionSize(24);
    m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pointTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pointTable->setAlternatingRowColors(true);
    m_pointTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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
    m_batchDirectionCombo->addItem(tr("+X 方向"));
    m_batchDirectionCombo->addItem(tr("-X 方向"));
    m_batchDirectionCombo->addItem(tr("+Y 方向"));
    m_batchDirectionCombo->addItem(tr("-Y 方向"));
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
    m_pathDirectionCombo = new QComboBox(pathBox);
    m_pathDirectionCombo->setObjectName("mapPathDirectionCombo");
    m_pathDirectionCombo->addItem(tr("双向 ↔"), true);
    m_pathDirectionCombo->addItem(tr("单向 →"), false);
    m_pathDirectionCombo->setToolTip(tr("双向允许沿同一路径往返；单向仅允许从所选起点到终点。"));
    configureComboField(m_pathDirectionCombo);
    pathForm->addRow(tr("通行方向"), m_pathDirectionCombo);

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

    leftScrollArea->setWidget(leftScrollWidget);
    editorTabs->addTab(leftScrollArea, tr("点位"));

    const auto addEditorPage = [editorTabs](QWidget *content, const QString &name,
                                           const QString &title, bool expand) {
        auto scroll = new QScrollArea(editorTabs);
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto page = new QWidget(scroll);
        auto layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        layout->addWidget(content, expand ? 1 : 0);
        if (!expand) layout->addStretch();
        scroll->setWidget(page);
        editorTabs->addTab(scroll, title);
    };
    addEditorPage(pathBox, QStringLiteral("mapPathScrollArea"), tr("路径"), true);
    addEditorPage(gridBox, QStringLiteral("mapSettingsScrollArea"), tr("地图"), false);

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
    m_mapNameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_mapNameLabel->installEventFilter(this);
    topBar->addWidget(m_mapNameLabel, 1);

    auto fileBar = new QHBoxLayout();
    fileBar->setSpacing(6);

    m_mousePositionLabel = new QLabel(tr("X: 0.000  Y: 0.000"), centerPanel);
    m_mousePositionLabel->setObjectName(QStringLiteral("mapMousePositionLabel"));
    m_mousePositionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_mousePositionLabel->setMinimumWidth(90);

    m_newMapButton = new QPushButton(tr("新建"), centerPanel);
    m_newMapButton->setObjectName(QStringLiteral("mapNewButton"));
    fileBar->addWidget(m_newMapButton);

    m_loadMapButton = new QPushButton(tr("加载…"), centerPanel);
    m_loadMapButton->setObjectName(QStringLiteral("mapLoadButton"));
    fileBar->addWidget(m_loadMapButton);

    m_saveMapButton = new QPushButton(tr("另存为…"), centerPanel);
    m_saveMapButton->setObjectName(QStringLiteral("mapSaveAsButton"));
    fileBar->addWidget(m_saveMapButton);

    m_quickSaveButton = new QPushButton(tr("保存"), centerPanel);
    m_quickSaveButton->setObjectName(QStringLiteral("mapQuickSaveButton"));
    fileBar->addWidget(m_quickSaveButton);

    fileBar->addStretch();

    m_editModeButton = new QPushButton(tr("编辑模式"), centerPanel);
    m_editModeButton->setObjectName(QStringLiteral("mapEditModeButton"));
    m_editModeButton->setCheckable(true);
    m_editModeButton->setChecked(true);
    m_editModeButton->setMinimumWidth(84);
    topBar->addWidget(m_editModeButton);

    m_locateButton = new QPushButton(tr("定位"), centerPanel);
    m_locateButton->setObjectName(QStringLiteral("mapLocateButton"));
    m_locateButton->setToolTip(tr("定位到当前车辆；编辑模式下按序新增含当前位置和朝向的地图点。任务进行中仅定位。"));
    topBar->addWidget(m_locateButton);

    centerLayout->addLayout(topBar);
    centerLayout->addLayout(fileBar);

    m_view = new MapGraphicsView(centerPanel);
    m_view->setObjectName(QStringLiteral("mapGraphicsView"));
    m_view->setMinimumSize(320, 240);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_view->viewport()->setCursor(Qt::CrossCursor);
    m_view->viewport()->installEventFilter(this);
    centerLayout->addWidget(m_view, 1);

    auto *trailBar = new QHBoxLayout;
    auto *trailLegend = new QLabel(tr("<span style='color:%1'>━ 双向路径</span> · "
                                     "<span style='color:%2'>━ 单向路径</span> · "
                                     "<span style='color:%3'>┄ 规划路线</span> · "
                                     "<span style='color:%4'>━ 实时定位轨迹</span>")
        .arg(kBidirectionalPathColor.name(), kSingleDirectionPathColor.name(),
             kPlannedRouteColor.name(), kLocalizationTrailColor.name()), centerPanel);
    trailLegend->setObjectName(QStringLiteral("mapLocalizationTrailLegend"));
    trailLegend->setTextFormat(Qt::RichText);
    trailLegend->setWordWrap(true);
    trailLegend->setToolTip(tr("定位轨迹来自融合定位，保留最近记录，不代表独立地面实测；定位中断处不连线。"));
    trailBar->addWidget(trailLegend, 1);
    auto *clearTrailButton = new QPushButton(tr("清除定位轨迹"), centerPanel);
    clearTrailButton->setObjectName(QStringLiteral("mapClearLocalizationTrailButton"));
    connect(clearTrailButton, &QPushButton::clicked, this, &Map::clearLocalizationTrail);
    trailBar->addWidget(clearTrailButton);
    centerLayout->addLayout(trailBar);

    m_rotationSlider = new QSlider(Qt::Horizontal, centerPanel);
    m_rotationSlider->setObjectName(QStringLiteral("mapRotationSlider"));
    m_rotationSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rotationSlider->setRange(-1800, 1800);
    m_rotationSlider->setSingleStep(5);
    m_rotationSlider->setPageStep(30);
    m_rotationSlider->setValue(0);
    m_rotationSlider->setToolTip(tr("仅旋转地图显示，不改变地图点坐标或任务规划。"));
    auto *viewFooter = new QHBoxLayout;
    viewFooter->addWidget(m_mousePositionLabel);
    viewFooter->addWidget(m_rotationSlider, 1);
    centerLayout->addLayout(viewFooter);

    mainSplitter->addWidget(centerPanel);

    auto rightPanel = new QFrame(m_mapPage);
    rightPanel->setObjectName(QStringLiteral("mapRightPanel"));
    rightPanel->setFrameShape(QFrame::StyledPanel);
    rightPanel->setMinimumWidth(300);
    auto rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(8);

    auto* ipcBox = new QGroupBox(tr("任务执行"), rightPanel);
    ipcBox->setObjectName("mapTaskExecutionPanel");
    auto* ipcLayout = new QGridLayout(ipcBox);
    int ipcButtonIndex = 0;
    auto addIpcButton = [this, ipcBox, ipcLayout, &ipcButtonIndex](const QString& text, auto action) {
        auto* button = new QPushButton(text, ipcBox);
        ipcLayout->addWidget(button, 3 + ipcButtonIndex / 2, ipcButtonIndex % 2); ++ipcButtonIndex;
        connect(button, &QPushButton::clicked, this, action);
        return button;
    };
    m_taskPauseButton = addIpcButton(tr("暂停任务"), [this] { if (m_coordinator) m_coordinator->pauseTask(); });
    m_taskPauseButton->setObjectName("mapTaskPauseButton");
    m_taskPauseButton->setToolTip(tr("立即发送暂停请求，工控机平滑减速；确认实测停稳后可继续同一任务。"));
    m_taskResumeButton = addIpcButton(tr("继续任务"), [this] { handleRouteResume(); });
    m_taskResumeButton->setObjectName("mapTaskResumeButton");
    m_taskStopButton = addIpcButton(tr("结束任务"), [this] { if (m_coordinator) m_coordinator->abortTask(); });
    auto *emergencyButton = addIpcButton(tr("急停"), [this] { if (m_coordinator) m_coordinator->emergencyStop(); });
    emergencyButton->setObjectName("mapTaskEmergencyButton");
    emergencyButton->setStyleSheet("QPushButton { color: #b42318; font-weight: 600; }");
    m_taskResetButton = addIpcButton(tr("复位故障"), [this] { if (m_coordinator) m_coordinator->resetFault(); });
    m_taskResetButton->setObjectName("mapTaskResetButton");
    m_taskResetButton->setToolTip(tr("排除故障并确认停稳后复位。只恢复待命，不恢复原任务、不自动运动；实体急停须先在现场解除。"));
    m_cancelStartButton = addIpcButton(tr("取消后续启动"), [this] { if (m_coordinator) m_coordinator->cancelPendingAction(); });
    m_cancelStartButton->setObjectName("mapCancelPendingStartButton");
    ipcLayout->removeWidget(m_taskResetButton);
    ipcLayout->removeWidget(m_cancelStartButton);
    ipcLayout->addWidget(m_taskResetButton, 5, 0, 1, 2);
    ipcLayout->addWidget(m_cancelStartButton, 6, 0, 1, 2);
    m_checkpointReviewButton = new QPushButton(tr("核对拍摄结果"), ipcBox);
    m_checkpointReviewButton->setObjectName("mapCheckpointReviewButton");
    m_checkpointReviewButton->setToolTip(tr("拍照结果未知时，核实实际照片后确认，不会再次拍摄。"));
    m_checkpointReviewButton->hide();
    ipcLayout->addWidget(m_checkpointReviewButton, 7, 0, 1, 2);
    connect(m_checkpointReviewButton, &QPushButton::clicked, this, &Map::handleCheckpointReview);
    const int statusRow = 0;
    m_trackingStatusLabel = new QLabel(tr("未连接"), ipcBox);
    m_trackingStatusLabel->setObjectName("mapTrackingStatusLabel");
    m_trackingStatusLabel->setWordWrap(true);
    m_trackingStatusLabel->setStyleSheet("font-size:16px; font-weight:600; color:#244d3a;");
    ipcLayout->addWidget(m_trackingStatusLabel, statusRow, 0, 1, 2);
    m_taskProgressLabel = new QLabel(tr("等待任务"), ipcBox);
    m_taskProgressLabel->setObjectName("mapTaskProgressLabel");
    m_taskProgressLabel->setWordWrap(true);
    ipcLayout->addWidget(m_taskProgressLabel, statusRow + 1, 0, 1, 2);
    m_routeStatusLabel = new QLabel(ipcBox);
    m_routeStatusLabel->setObjectName("mapTaskNoticeLabel");
    m_routeStatusLabel->setWordWrap(true);
    m_routeStatusLabel->setMinimumHeight(36);
    m_routeStatusLabel->setMaximumHeight(58);
    ipcLayout->addWidget(m_routeStatusLabel, statusRow + 2, 0, 1, 2);
    auto taskTabWidget = new QTabWidget(rightPanel);
    taskTabWidget->setObjectName(QStringLiteral("mapTaskTabWidget"));
    taskTabWidget->setDocumentMode(true);
    taskTabWidget->setElideMode(Qt::ElideNone);

    auto routePage = new QWidget(taskTabWidget);
    routePage->setObjectName(QStringLiteral("mapRouteTaskPage"));
    auto *routeOuterLayout = new QVBoxLayout(routePage);
    routeOuterLayout->setContentsMargins(8, 8, 8, 8);
    routeOuterLayout->setSpacing(8);
    auto *routeScroll = new QScrollArea(routePage);
    routeScroll->setObjectName(QStringLiteral("mapRoutePrimitiveScrollArea"));
    routeScroll->setWidgetResizable(true);
    routeScroll->setFrameShape(QFrame::NoFrame);
    routeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *routePrimitivePage = new QWidget(routeScroll);
    auto routeLayout = new QVBoxLayout(routePrimitivePage);
    routeLayout->setContentsMargins(8, 8, 8, 8);
    routeLayout->setSpacing(8);

    auto *routeDescription = new QLabel(tr("按地图路径编排多点路线，依序执行。"), routePage);
    routeDescription->setObjectName(QStringLiteral("mapRouteTaskDescription"));
    routeDescription->setProperty("mapPlanningHint", true);
    routeDescription->setWordWrap(true);
    routeDescription->setToolTip(tr("先建立地图点与可通行路径，再按顺序添加路线。常规任务也可以是直线；速度和跟踪参数统一由工控机维护。"));
    routeLayout->addWidget(routeDescription);

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
    m_routeQueueList->setMinimumHeight(120);
    m_routeQueueEmptyLabel = new QLabel(tr("尚未添加路线\n选择起点和终点，点击“添加路线”"), m_routeQueueList->viewport());
    m_routeQueueEmptyLabel->setObjectName(QStringLiteral("mapRouteQueueEmptyLabel"));
    m_routeQueueEmptyLabel->setProperty("mapPlanningHint", true);
    m_routeQueueEmptyLabel->setAlignment(Qt::AlignCenter);
    m_routeQueueEmptyLabel->setWordWrap(true);
    m_routeQueueEmptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *queueEmptyLayout = new QVBoxLayout(m_routeQueueList->viewport());
    queueEmptyLayout->setContentsMargins(12, 12, 12, 12);
    queueEmptyLayout->addWidget(m_routeQueueEmptyLabel);
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
    m_routeStartButton = new QPushButton(tr("下发并执行"), routePage);
    m_routeStartButton->setObjectName(QStringLiteral("mapRouteStartButton"));
    m_routeStartButton->setProperty("supportPrimary", true);
    controlButtons->addWidget(m_routeStartButton);

    auto loopLayout = new QHBoxLayout();
    loopLayout->setSpacing(4);
    m_routeLoopCheck = new QCheckBox(tr("循环执行"), routePage);
    m_routeLoopCheck->setObjectName(QStringLiteral("mapRouteLoopCheck"));
    loopLayout->addWidget(m_routeLoopCheck);
    m_routeLoopIntervalSpin = new QSpinBox(routePage);
    m_routeLoopIntervalSpin->setObjectName(QStringLiteral("mapRouteLoopIntervalSpin"));
    m_routeLoopIntervalSpin->setToolTip(tr("每轮任务结束后，下一轮开始前的等待时间。"));
    m_routeLoopIntervalSpin->setRange(0, 3600);
    m_routeLoopIntervalSpin->setSuffix(tr(" 秒"));
    m_routeLoopIntervalSpin->setValue(kDefaultRouteLoopIntervalSeconds);
    m_routeLoopIntervalSpin->setEnabled(false);
    loopLayout->addWidget(m_routeLoopIntervalSpin);
    loopLayout->addStretch();
    routeLayout->addLayout(loopLayout);

    m_routeStartHintLabel = new QLabel(routePage);
    m_routeStartHintLabel->setObjectName(QStringLiteral("mapRouteStartHintLabel"));
    m_routeStartHintLabel->setProperty("mapPlanningHint", true);
    m_routeStartHintLabel->setWordWrap(true);
    routeScroll->setWidget(routePrimitivePage);
    routeOuterLayout->addWidget(routeScroll, 1);
    routeOuterLayout->addWidget(m_routeStartHintLabel);
    routeOuterLayout->addLayout(controlButtons);

    auto rowWorkPage = new QWidget(taskTabWidget);
    rowWorkPage->setObjectName(QStringLiteral("mapRowWorkPage"));
    auto rowWorkLayout = new QVBoxLayout(rowWorkPage);
    rowWorkLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkLayout->setSpacing(8);

    auto rowWorkPrimitiveScrollArea = new QScrollArea(rowWorkPage);
    rowWorkPrimitiveScrollArea->setObjectName(QStringLiteral("mapRowWorkPrimitiveScrollArea"));
    rowWorkPrimitiveScrollArea->setWidgetResizable(true);
    rowWorkPrimitiveScrollArea->setFrameShape(QFrame::NoFrame);
    rowWorkPrimitiveScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_rowWorkPrimitivePage = new QWidget(rowWorkPrimitiveScrollArea);
    m_rowWorkPrimitivePage->setObjectName(QStringLiteral("mapRowWorkPrimitivePage"));
    auto primitiveLayout = new QVBoxLayout(m_rowWorkPrimitivePage);
    primitiveLayout->setContentsMargins(8, 8, 8, 8);
    primitiveLayout->setSpacing(8);

    auto *rowDescription = new QLabel(tr("示教起点、检查点与终点；先回起点，再往返作业。"), m_rowWorkPrimitivePage);
    rowDescription->setObjectName(QStringLiteral("mapRowWorkTaskDescription"));
    rowDescription->setProperty("mapPlanningHint", true);
    rowDescription->setWordWrap(true);
    rowDescription->setToolTip(tr("去程 A→B、回程 B→A，检查点可分别设置停留与拍照。保留各记录点的位置；回起点准备过程不触发检查点作业。"));
    primitiveLayout->addWidget(rowDescription);

    auto rowWorkTeachBox = new QGroupBox(tr("作业线示教"), m_rowWorkPrimitivePage);
    rowWorkTeachBox->setObjectName(QStringLiteral("mapRowWorkTeachBox"));
    auto rowWorkTeachLayout = new QFormLayout(rowWorkTeachBox);
    rowWorkTeachLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkTeachLayout->setSpacing(6);

    m_rowWorkStartLabel = new QLabel(tr("未记录"), rowWorkTeachBox);
    m_rowWorkStartLabel->setObjectName(QStringLiteral("mapRowWorkStartLabel"));
    m_rowWorkStartLabel->setWordWrap(true);
    rowWorkTeachLayout->addRow(tr("起点 A"), m_rowWorkStartLabel);

    m_rowWorkEndLabel = new QLabel(tr("未记录"), rowWorkTeachBox);
    m_rowWorkEndLabel->setObjectName(QStringLiteral("mapRowWorkEndLabel"));
    m_rowWorkEndLabel->setWordWrap(true);
    rowWorkTeachLayout->addRow(tr("终点 B"), m_rowWorkEndLabel);
    m_rowWorkPlanLabel = new QLabel(rowWorkTeachBox);
    m_rowWorkPlanLabel->setObjectName("mapRowWorkPlanLabel");
    m_rowWorkPlanLabel->setWordWrap(true);
    rowWorkTeachLayout->addRow(m_rowWorkPlanLabel);

    auto rowWorkTeachButtons = new QGridLayout();
    rowWorkTeachButtons->setSpacing(4);
    m_rowWorkCaptureStartButton = new QPushButton(tr("记录起点"), rowWorkTeachBox);
    m_rowWorkCaptureStartButton->setObjectName(QStringLiteral("mapRowWorkCaptureStartButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkCaptureStartButton, 0, 0);
    m_rowWorkCaptureEndButton = new QPushButton(tr("记录终点"), rowWorkTeachBox);
    m_rowWorkCaptureEndButton->setObjectName(QStringLiteral("mapRowWorkCaptureEndButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkCaptureEndButton, 0, 1);
    m_rowWorkClearLineButton = new QPushButton(tr("清除作业线"), rowWorkTeachBox);
    m_rowWorkClearLineButton->setObjectName(QStringLiteral("mapRowWorkClearLineButton"));
    rowWorkTeachButtons->addWidget(m_rowWorkClearLineButton, 1, 0, 1, 2);
    rowWorkTeachLayout->addRow(rowWorkTeachButtons);

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
        {tr("名称"), tr("站距(m)"), tr("停留(ms)"), tr("去程"), tr("回程"), tr("拍照"), tr("超时(ms)")});
    m_rowWorkCheckpointTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_rowWorkCheckpointTable->horizontalHeader()->setMinimumSectionSize(64);
    m_rowWorkCheckpointTable->setColumnWidth(0, 72);
    m_rowWorkCheckpointTable->setColumnWidth(1, 82);
    m_rowWorkCheckpointTable->setColumnWidth(2, 88);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_rowWorkCheckpointTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    const QStringList checkpointColumnHints{
        tr("检查点名称"), tr("沿示教路线距起点 A 的距离，单位米"), tr("到点停稳后的停留时间，单位毫秒"),
        tr("去程 A→B 经过此点时执行作业"), tr("回程 B→A 经过此点时执行作业"),
        tr("选定方向到点停稳后拍照；需上位机在线协调"), tr("等待拍照结果的超时时间，单位毫秒")};
    for (int column = 0; column < checkpointColumnHints.size(); ++column)
        m_rowWorkCheckpointTable->horizontalHeaderItem(column)->setToolTip(checkpointColumnHints[column]);
    m_rowWorkCheckpointTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_rowWorkCheckpointTable->verticalHeader()->setVisible(false);
    m_rowWorkCheckpointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rowWorkCheckpointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rowWorkCheckpointTable->setAlternatingRowColors(true);
    m_rowWorkCheckpointTable->setMinimumHeight(180);
    m_rowWorkCheckpointTable->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *checkpointHint = new QLabel(tr("横向滚动表格可设置去程 / 回程、拍照和超时。"), rowWorkCheckpointBox);
    checkpointHint->setObjectName(QStringLiteral("mapRowWorkCheckpointHint"));
    checkpointHint->setProperty("mapPlanningHint", true);
    checkpointHint->setWordWrap(true);
    rowWorkCheckpointLayout->addWidget(checkpointHint);
    rowWorkCheckpointLayout->addWidget(m_rowWorkCheckpointTable);

    primitiveLayout->addWidget(rowWorkCheckpointBox);

    auto rowWorkParamsBox = new QGroupBox(tr("作业设置"), m_rowWorkPrimitivePage);
    rowWorkParamsBox->setObjectName(QStringLiteral("mapRowWorkParamsBox"));
    auto rowWorkParamsLayout = new QFormLayout(rowWorkParamsBox);
    rowWorkParamsLayout->setContentsMargins(8, 8, 8, 8);
    rowWorkParamsLayout->setSpacing(6);

    auto *navigationHint = new QLabel(tr("使用工控机导航参数"), rowWorkParamsBox);
    navigationHint->setObjectName(QStringLiteral("mapNavigationParametersHint"));
    navigationHint->setToolTip(tr("速度、停车和转向容差统一使用工控机已应用的导航参数。修改入口：维护 → 工控机 → 融合 / 导航参数。"));
    navigationHint->setWordWrap(true);
    rowWorkParamsLayout->addRow(navigationHint);

    m_rowWorkLoopCheck = new QCheckBox(tr("循环往返"), rowWorkParamsBox);
    m_rowWorkLoopCheck->setObjectName(QStringLiteral("mapRowWorkLoopCheck"));
    rowWorkParamsLayout->addRow(QString(), m_rowWorkLoopCheck);

    primitiveLayout->addWidget(rowWorkParamsBox);

    m_rowWorkStartButton = new QPushButton(tr("下发并执行"), rowWorkPage);
    m_rowWorkStartButton->setObjectName(QStringLiteral("mapRowWorkStartButton"));
    m_rowWorkStartButton->setProperty("supportPrimary", true);
    rowWorkPrimitiveScrollArea->setWidget(m_rowWorkPrimitivePage);
    rowWorkLayout->addWidget(rowWorkPrimitiveScrollArea, 1);
    m_rowWorkStartHintLabel = new QLabel(rowWorkPage);
    m_rowWorkStartHintLabel->setObjectName(QStringLiteral("mapRowWorkStartHintLabel"));
    m_rowWorkStartHintLabel->setProperty("mapPlanningHint", true);
    m_rowWorkStartHintLabel->setWordWrap(true);
    rowWorkLayout->addWidget(m_rowWorkStartHintLabel);
    rowWorkLayout->addWidget(m_rowWorkStartButton);

    taskTabWidget->addTab(routePage, tr("常规任务"));
    taskTabWidget->addTab(rowWorkPage, tr("直线任务"));
    rightLayout->addWidget(taskTabWidget, 1);
    rightLayout->addWidget(ipcBox);

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
    connect(m_pointTable, &QTableWidget::currentCellChanged, this,
            [this] { handlePointSelectionChanged(); });
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
    connect(m_routeQueueList, &QListWidget::currentRowChanged, this, [this] { updateRouteControlState(); });
    connect(m_routeStartCombo, &QComboBox::currentIndexChanged, this, [this] { updateRouteControlState(); });
    connect(m_routeEndCombo, &QComboBox::currentIndexChanged, this, [this] { updateRouteControlState(); });
    connect(m_routeLoopCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!canEditRowWorkPlan()) {
            QSignalBlocker blocker(m_routeLoopCheck);
            m_routeLoopCheck->setChecked(!checked);
            return;
        }
        if (m_routeLoopIntervalSpin) {
            m_routeLoopIntervalSpin->setEnabled(checked && canEditRowWorkPlan());
        }
        updateRouteControlState();
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
    connect(m_rowWorkStartButton, &QPushButton::clicked, this, &Map::handleRowWorkStart);
    connect(m_rowWorkLoopCheck, &QCheckBox::toggled, this, &Map::handleRowWorkLoopChanged);
    connect(m_rowWorkCheckpointTable, &QTableWidget::cellChanged, this, &Map::handleRowWorkCheckpointCellChanged);
    connect(m_rowWorkCheckpointTable, &QTableWidget::currentCellChanged, this,
            [this] { refreshRowWorkControlState(); });
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

bool Map::addPointInternal(double x, double y, double theta, std::optional<int> forcedId,
                           std::optional<int> dwellMs)
{
    const int dwell = dwellMs.value_or(forcedId ? 0 :
        (m_pointDwellSpin ? m_pointDwellSpin->value() : kDefaultMapPointDwellMs));
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(theta) || dwell < 0 || dwell > 3600000)
        return false;
    const int candidateId = forcedId.value_or(m_nextPointId);
    if (candidateId < 0 || candidateId == std::numeric_limits<int>::max()) return false;
    if (m_points.contains(candidateId)) {
        return false;
    }
    const bool autoConnect = !forcedId && m_autoConnectPointsCheck && m_autoConnectPointsCheck->isChecked();
    int predecessor = -1;
    if (autoConnect) {
        for (auto it = m_points.cbegin(); it != m_points.cend(); ++it)
            if (it.key() < candidateId && it.key() > predecessor) predecessor = it.key();
        if (const auto *previous = pointById(predecessor);
            previous && QLineF(previous->mapPosition, QPointF(x, y)).length() <= 1e-7) {
            setRouteStatusText(tr("新点与上一点 %1 重合，未新增点或路径").arg(predecessor), true);
            return false;
        }
        if (predecessor >= 0 &&
            (m_nextPathId < 0 || m_nextPathId >= std::numeric_limits<int>::max())) {
            setRouteStatusText(tr("路径编号已耗尽，未新增点或路径"), true);
            return false;
        }
    }
    if (!m_scene) ensureScene();
    // Do not consume a point number when an interactive addition is rejected.
    const int pointId = forcedId ? *forcedId : allocatePointId();

    m_nextPointId = std::max(m_nextPointId, pointId + 1);

    MapPoint point;
    point.id = pointId;
    point.mapPosition = QPointF(x, y);
    point.theta = normalizeAngle(theta);
    point.dwellMs = dwell;

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

    if (autoConnect && predecessor >= 0 && findPathId(predecessor, pointId) == -1 &&
        addLinePath(predecessor, pointId))
        refreshPathUi();

    if (!forcedId) refreshPlanningRevision();

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
    if (m_pendingPointSelection == pointId) {
        m_pendingPointSelection.reset();
        if (m_pointTable) {
            QSignalBlocker blocker(m_pointTable);
            m_pointTable->clearSelection();
            m_pointTable->setCurrentCell(-1, -1);
        }
    }


    QList<int> affectedPaths;
    for (auto pit = m_paths.cbegin(); pit != m_paths.cend(); ++pit) {
        if (pit.value().startId == pointId || pit.value().endId == pointId) {
            affectedPaths.append(pit.key());
        }
    }
    for (int pathId : affectedPaths) {
        removePathInternal(pathId);
    }

}

int Map::addPathOrWarn(int startId, int endId, PathType type, double sagitta, std::optional<int> forcedId, bool bidirectional)
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

            existingPath->bidirectional = bidirectional;
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
            setRouteStatusText(tr("已更新路径: 点%1 %2 点%3").arg(startId).arg(bidirectional ? QStringLiteral("↔") : QStringLiteral("→")).arg(endId));
            return existingId;
        }
    }

    bool success = false;
    if (type == PathType::Arc) {
        success = addArcPath(startId, endId, sagitta, forcedId, bidirectional);
    } else {
        success = addLinePath(startId, endId, forcedId, bidirectional);
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
    setRouteStatusText(tr("已添加路径: 点%1 %2 点%3").arg(startId).arg(bidirectional ? QStringLiteral("↔") : QStringLiteral("→")).arg(endId));
    return createdId;
}

bool Map::addLinePath(int startId, int endId, std::optional<int> forcedId, bool bidirectional)
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
    path.bidirectional = bidirectional;
    path.type = PathType::Line;
    path.polyline = {startPoint->mapPosition, endPoint->mapPosition};
    path.sagitta = 0.0;

    return addPathInternal(std::move(path));
}

bool Map::addArcPath(int startId, int endId, double sagitta, std::optional<int> forcedId, bool bidirectional)
{
    const MapPoint *startPoint = pointById(startId);
    const MapPoint *endPoint = pointById(endId);
    if (!startPoint || !endPoint || startId == endId) {
        return false;
    }

    if (std::abs(sagitta) < kArcSagittaEpsilon) {
        return addLinePath(startId, endId, forcedId, bidirectional);
    }

    MapPath path;
    path.id = forcedId.value_or(-1);
    path.startId = startId;
    path.endId = endId;
    path.bidirectional = bidirectional;
    path.type = PathType::Arc;
    path.sagitta = sagitta;

    return addPathInternal(std::move(path));
}
bool Map::addPathInternal(MapPath &&path)
{
    if (path.id == std::numeric_limits<int>::max() ||
        (path.id < 0 && (m_nextPathId < 0 || m_nextPathId >= std::numeric_limits<int>::max())))
        return false;
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

    polyline.first() = startPoint->mapPosition;
    polyline.last() = endPoint->mapPosition;
    path.polyline = polyline;
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

    // Direction belongs to this edge. Another edge with reversed endpoints
    // may follow different geometry and does not make this path bidirectional.
    QPen pen(path.bidirectional ? kBidirectionalPathColor : kSingleDirectionPathColor);
    // Wide base paths remain visible beside the narrower plan and live trail
    // when all three share coordinates; widths stay readable at every zoom.
    pen.setWidthF(5.0);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    path.pathItem->setPath(scenePath);
    path.pathItem->setPen(pen);
    path.pathItem->setVisible(m_pathsVisible);
    path.pathItem->setToolTip(tr("路径 %1 · %2 · 点%3 至 点%4")
        .arg(path.id).arg(path.bidirectional ? tr("双向") : tr("单向"))
        .arg(path.startId).arg(path.endId));
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
    refreshRouteGraphics();
}

void Map::applyPresentationUpdates(bool recenterView)
{
    rebuildGrid();
    refreshAllPointGraphics();
    refreshAllPathGeometry();
    refreshVehicleGraphics();
    refreshLocalizationTrailGraphics();
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
    m_vehicleItem->setOpacity(m_hasVehiclePose ? 1.0 : 0.35);
    m_vehicleItem->setToolTip(m_hasVehiclePose ? tr("当前有效位置") : tr("最后已知位置，当前定位不可用"));
    if (!m_hasVehiclePose) {
        return;
    }

    m_vehicleItem->setPos(mapToScene(QPointF(m_vehiclePoseX, m_vehiclePoseY)));
    m_vehicleItem->setRotation(qRadiansToDegrees(mapHeadingToSceneAngle(m_vehiclePoseTheta)));
}

void Map::clearLocalizationTrail()
{
    if (m_localizationTrailRefreshTimer) m_localizationTrailRefreshTimer->stop();
    m_localizationTrail.clear();
    m_localizationTrailFrame.clear();
    m_localizationTrailBoot.clear();
    m_localizationTrailSequence = 0;
    m_localizationTrailStartsSegment = true;
    m_localizationTrailAge.invalidate();
    if (m_localizationTrailItem) m_localizationTrailItem->setPath(QPainterPath{});
}

void Map::appendLocalizationTrail(const ControlPoseSnapshot &pose)
{
    if (!pose.validForControl || pose.bootId.isEmpty() || !m_binding.isUsable() ||
        !std::isfinite(pose.position.x()) || !std::isfinite(pose.position.y()) || !std::isfinite(pose.yaw)) {
        m_localizationTrailStartsSegment = true;
        return;
    }
    const auto frame = localizationTrailFrame(m_binding);
    if (!m_localizationTrailFrame.isEmpty() && m_localizationTrailFrame != frame)
        clearLocalizationTrail();
    if (pose.bootId == m_localizationTrailBoot && pose.seq <= m_localizationTrailSequence)
        return;
    const bool gap = m_localizationTrailAge.isValid() && m_localizationTrailAge.elapsed() > 1000;
    m_localizationTrail.append({pose.position, pose.yaw,
        m_localizationTrailStartsSegment || gap || pose.bootId != m_localizationTrailBoot});
    if (m_localizationTrail.size() > kMaxLocalizationTrailSamples) {
        m_localizationTrail.removeFirst();
        m_localizationTrail.front().startsSegment = true;
    }
    m_localizationTrailFrame = frame;
    m_localizationTrailBoot = pose.bootId;
    m_localizationTrailSequence = pose.seq;
    m_localizationTrailStartsSegment = false;
    m_localizationTrailAge.restart();
    // Keep every accepted sample; rebuild the bounded path at most 10 Hz.
    // A single-shot timer also paints the last sample when delivery stops.
    if (m_localizationTrailRefreshTimer && !m_localizationTrailRefreshTimer->isActive())
        m_localizationTrailRefreshTimer->start();
}

void Map::refreshLocalizationTrailGraphics()
{
    if (m_localizationTrailRefreshTimer) m_localizationTrailRefreshTimer->stop();
    if (!m_scene) return;
    if (!m_localizationTrailFrame.isEmpty() &&
        (!m_binding.isUsable() || m_localizationTrailFrame != localizationTrailFrame(m_binding)))
        clearLocalizationTrail();
    if (!m_localizationTrailItem) {
        QPen pen(kLocalizationTrailColor, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        pen.setCosmetic(true);
        m_localizationTrailItem = m_scene->addPath(QPainterPath{}, pen);
        m_localizationTrailItem->setZValue(12.0);
        m_localizationTrailItem->setToolTip(tr("融合定位轨迹（非独立地面实测）"));
        m_localizationTrailItem->setAcceptedMouseButtons(Qt::NoButton);
    }
    QPainterPath path;
    if (m_binding.isUsable()) {
        const MapFrameAdapter adapter(m_binding);
        for (const auto &sample : std::as_const(m_localizationTrail)) {
            const auto scene = mapToScene(adapter.fromEnu(sample.enuPosition));
            if (sample.startsSegment) path.moveTo(scene);
            else path.lineTo(scene);
        }
    }
    m_localizationTrailItem->setPath(path);
}

void Map::refreshPathsForPoint(int pointId)
{
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
        if (it.value().startId == pointId || it.value().endId == pointId) {
            refreshPathGeometry(it.value());
        }
    }
    refreshRouteGraphics();
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
        edge.bidirectional = path.bidirectional;
        edges.append(edge);
    }

    return MapRoutePlanner::findPathIds(startId, endId, edges, ConfigManager::instance().routePlanning());
}

QList<QPointF> Map::composePolyline(const QList<int> &pathIds, int startId) const
{
    QList<QPointF> polyline;
    if (pathIds.isEmpty()) return polyline;
    const auto *first = pathById(pathIds.first());
    if (!first) return {};
    int current = startId > 0 ? startId : first->startId;
    for (int pathId : pathIds) {
        const auto *path = pathById(pathId);
        if (!path || path->polyline.size() < 2) return {};
        auto segment = path->polyline;
        if (path->startId == current) current = path->endId;
        else if (path->bidirectional && path->endId == current) {
            std::reverse(segment.begin(), segment.end());
            current = path->startId;
        } else return {};
        if (polyline.isEmpty()) polyline = segment;
        else {
            if (!pointsAlmostEqual(polyline.last(), segment.first())) return {};
            for (int i = 1; i < segment.size(); ++i) polyline.append(segment.at(i));
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
        pointIdItems(m_pointTable, pointId);
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

bool Map::rebuildRouteStep(RouteStep &step)
{
    QList<int> pathIds = findRoutePathIds(step.fromId, step.toId);
    if (pathIds.isEmpty()) {
        step.pathIds.clear();
        return false;
    }
    step.pathIds = pathIds;
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

        auto dwellItem = new QTableWidgetItem(QString::number(point.dwellMs));
        dwellItem->setFlags(dwellItem->flags() & ~Qt::ItemIsEditable);
        dwellItem->setTextAlignment(Qt::AlignCenter);
        m_pointTable->setItem(row, 4, dwellItem);
        ++row;
    }

    m_pointTable->resizeRowsToContents();

    if (selectedId.has_value()) {
        const QList<QTableWidgetItem *> items =
            pointIdItems(m_pointTable, selectedId.value());
        if (!items.isEmpty()) {
            const int selectedRow = items.first()->row();
            m_pointTable->setCurrentCell(selectedRow, 0,
                                         QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
}

void Map::refreshPathUi()
{
    refreshRouteGraphics();
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

        auto typeItem = new QTableWidgetItem((path.type == PathType::Line ? tr("直线") : tr("圆弧")) + (path.bidirectional ? tr(" · 双向") : tr(" · 单向")));
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
    updateRouteControlState();
}

void Map::refreshRouteQueueUi()
{
    refreshRouteGraphics();
    if (!m_routeQueueList) {
        return;
    }

    const int selectedRow = m_routeQueueList->currentRow();
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
    if (!m_routeQueue.isEmpty())
        m_routeQueueList->setCurrentRow(std::clamp(selectedRow, 0, int(m_routeQueue.size()) - 1));
    if (m_routeQueueEmptyLabel) m_routeQueueEmptyLabel->setVisible(m_routeQueue.isEmpty());
}
void Map::refreshRouteGraphics()
{
    if (!m_scene || (!m_routePlanItem && m_routeQueue.isEmpty())) return;
    if (!m_routePlanItem) {
        m_routePlanItem = m_scene->addPath(QPainterPath{}, plannedRoutePen());
        m_routePlanItem->setZValue(-3.0);
        m_routePlanItem->setToolTip(tr("常规任务规划路线"));
        m_routePlanItem->setAcceptedMouseButtons(Qt::NoButton);
    }
    QPainterPath plan;
    for (const auto &step : std::as_const(m_routeQueue)) {
        // Use the same current graph lookup as task compilation, so editing
        // an edge or adding a shorter route cannot leave an old preview.
        const auto points = composePolyline(findRoutePathIds(step.fromId, step.toId), step.fromId);
        for (qsizetype i = 0; i < points.size(); ++i) {
            const auto scene = mapToScene(points[i]);
            if (i == 0) plan.moveTo(scene);
            else plan.lineTo(scene);
        }
    }
    m_routePlanItem->setPath(plan);
}
void Map::updateRouteControlState()
{
    const bool editable = canEditRowWorkPlan();
    const bool online = m_tracking && m_tracking->commandFresh();
    const auto state = m_tracking ? m_tracking->snapshot() : TrackingSnapshot{};
    const bool knownExecution = m_tracking && m_tracking->isConfigured() && !state.bootId.isEmpty() &&
        !state.taskId.isEmpty() && state.taskRevision > 0 && !state.executionId.isEmpty();
    const bool pending = m_coordinator && m_coordinator->actionPending();
    QString routeIssue;
    for (int i = 1; i < m_routeQueue.size(); ++i) {
        const auto *previousEnd = pointById(m_routeQueue[i - 1].toId);
        const auto *nextStart = pointById(m_routeQueue[i].fromId);
        if (!previousEnd || !nextStart ||
            QLineF(previousEnd->mapPosition, nextStart->mapPosition).length() > .02) {
            routeIssue = tr("第 %1 条路线不衔接，请移除后重新编排。").arg(i + 1);
            break;
        }
    }
    if (routeIssue.isEmpty() && !m_routeQueue.isEmpty() && m_routeLoopCheck && m_routeLoopCheck->isChecked()) {
        const auto *first = pointById(m_routeQueue.first().fromId);
        const auto *last = pointById(m_routeQueue.last().toId);
        if (!first || !last || QLineF(first->mapPosition, last->mapPosition).length() > .02)
            routeIssue = tr("循环任务需将最后一条路线接回起点。");
    }
    const QString startHint = !editable ? tr("当前任务锁定计划；结束后可重新编排。")
        : m_routeQueue.isEmpty() ? tr("先添加路线，再下发执行。")
        : !routeIssue.isEmpty() ? routeIssue
        : !online ? tr("等待工控机连接与任务状态更新。")
        : tr("将按列表顺序下发完整任务。参数由工控机统一维护。");
    if (m_routeStartHintLabel) m_routeStartHintLabel->setText(startHint);
    if (m_routeStartButton) {
        m_routeStartButton->setText(tr("下发并执行"));
        m_routeStartButton->setEnabled(online && editable && !m_routeQueue.isEmpty() && routeIssue.isEmpty());
        m_routeStartButton->setToolTip(startHint);
    }
    if (m_routeStartCombo) m_routeStartCombo->setEnabled(editable);
    if (m_routeEndCombo) m_routeEndCombo->setEnabled(editable);
    if (m_routeAddButton) {
        const int from = m_routeStartCombo ? m_routeStartCombo->currentData().toInt() : 0;
        const int to = m_routeEndCombo ? m_routeEndCombo->currentData().toInt() : 0;
        const bool continues = m_routeQueue.isEmpty() || m_routeQueue.last().toId == from;
        m_routeAddButton->setEnabled(editable && from > 0 && to > 0 && from != to && continues);
        m_routeAddButton->setToolTip(!editable ? tr("当前任务锁定计划，暂不能添加路线。")
            : !continues ? tr("新路线起点必须接续上一条路线的终点。")
            : tr("选择不同的起点和终点，沿地图已有通行路径添加一条路线。"));
    }
    if (m_locateButton) {
        const bool capturing = m_rowWorkPendingCaptureTarget != RowWorkPendingCaptureTarget::None;
        const bool recordsPoint = editable && m_editModeEnabled;
        m_locateButton->setText(capturing ? tr("采样中…") : recordsPoint ? tr("记录车辆点") : tr("定位车辆"));
        m_locateButton->setToolTip(capturing ? tr("请保持车辆停稳；采样完成后新增点位，失败时保留原地图。") : recordsPoint
            ? tr("定位到当前车辆，采集 1 秒停稳定位样本后新增含位置和朝向的地图点。")
            : tr("将地图视图居中到当前车辆位置。"));
    }
    if (m_taskPauseButton) m_taskPauseButton->setEnabled(knownExecution &&
        (state.isExecuting() || (state.state == "Paused" && state.autoResumePending)));
    const bool canContinue = state.state == "Paused" || state.state == "Ready";
    const bool reviewing = m_taskReviewOpen || !m_taskReviewIdentity.isEmpty();
    if (m_taskResumeButton) {
        m_taskResumeButton->setText(reviewing ? tr("正在核对任务…")
            : state.state == "Ready" ? tr("启动任务") : tr("继续任务"));
        m_taskResumeButton->setEnabled(online && canContinue && !state.taskId.isEmpty() && !pending
            && m_rowWorkPendingCaptureTarget == RowWorkPendingCaptureTarget::None
            && !m_tracking->busy() && !reviewing && !m_checkpointReviewOpen
            && m_checkpointReviewIdentity.isEmpty());
    }
    if (m_taskResumeButton) m_taskResumeButton->setToolTip(state.state == "Fault"
        ? tr("急停或故障不能直接继续。排除原因、确认停稳后点击“复位故障”，再重新下发任务。")
        : state.state == "Pausing" ? tr("工控机正在减速，确认实测停稳后才能继续。")
        : m_tracking && m_tracking->busy() ? tr("上一项操作结果仍待确认；确认完成后可继续，不会重复下发运动。")
        : !online ? tr("等待工控机任务状态更新后继续。")
        : !hasConfirmedTask() ? tr("先回读并核对工控机任务；核对不会触发运动，再次明确点击才启动或继续。")
        : state.state == "Ready" ? tr("明确启动工控机已经就绪的同一任务。")
        : tr("明确继续已暂停的同一任务；由工控机确认实时位姿、反馈和运行条件。"));
    if (m_taskStopButton) m_taskStopButton->setEnabled(knownExecution && !state.isTerminal());
    if (m_taskResetButton) m_taskResetButton->setEnabled(m_tracking && m_tracking->canResetFault() && !pending);
    if (m_cancelStartButton) { m_cancelStartButton->setEnabled(pending); m_cancelStartButton->setVisible(pending); }
    if (m_frameBindingButton) m_frameBindingButton->setEnabled(online && editable && !reviewing);
    if (m_checkpointReviewButton) {
        const bool needsReview = online && !m_externalReviewEventId.isEmpty()
            && state.waitingEventId == m_externalReviewEventId
            && (state.state == "Waiting" || state.state == "Paused");
        m_checkpointReviewButton->setVisible(needsReview);
        m_checkpointReviewButton->setEnabled(needsReview && !m_tracking->busy() && !pending
            && !reviewing && !m_checkpointReviewOpen && m_checkpointReviewIdentity.isEmpty());
    }
    if (m_routeRemoveButton) m_routeRemoveButton->setEnabled(editable && m_routeQueueList && m_routeQueueList->currentRow() >= 0);
    if (m_routeClearButton) m_routeClearButton->setEnabled(editable && !m_routeQueue.isEmpty());
    if (m_routeLoopCheck) m_routeLoopCheck->setEnabled(editable);
    if (m_routeLoopIntervalSpin) m_routeLoopIntervalSpin->setEnabled(editable && m_routeLoopCheck && m_routeLoopCheck->isChecked());
    if (m_newMapButton) m_newMapButton->setEnabled(editable);
    if (m_loadMapButton) m_loadMapButton->setEnabled(editable);
}

void Map::setRouteStatusText(const QString &text, bool warning)
{
    m_taskNoticeSource = TaskNoticeSource::Other;
    if (!m_routeStatusLabel) {
        return;
    }
    m_routeStatusLabel->setText(text.left(180));
    m_routeStatusLabel->setToolTip(text);
    m_routeStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
        .arg(warning ? "#b42318" : "#647a6c"));
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
    // Qt scene Y points down; ENU/standard Y points north (up).
    return normalizeAngle(-mapToStandardAngle(headingRad));
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
    const QString title = m_currentMapFilePath.isEmpty()
        ? tr("地图: %1（未保存）").arg(QFileInfo(suggestedMapFilePath(mapsDirectory())).fileName())
        : tr("地图: %1").arg(QFileInfo(m_currentMapFilePath).fileName());
    m_mapNameLabel->setText(m_mapNameLabel->fontMetrics().elidedText(title, Qt::ElideMiddle,
        m_mapNameLabel->contentsRect().width()));
    m_mapNameLabel->setToolTip(m_currentMapFilePath.isEmpty() ? title : m_currentMapFilePath);
}

QString Map::mapsDirectory()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("maps"));
}

QString Map::suggestedMapFilePath(const QString &directory) const
{
    const QDir mapDir(directory);
    const QString date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    QString path = mapDir.filePath(date + QStringLiteral(".json"));
    for (int suffix = 2; QFileInfo::exists(path); ++suffix)
        path = mapDir.filePath(QStringLiteral("%1-%2.json").arg(date).arg(suffix));
    return path;
}

bool Map::ensureMapsDirectory()
{
    if (QDir().mkpath(mapsDirectory())) return true;
    QMessageBox::warning(m_mapPage, tr("地图目录"), tr("无法创建地图目录，请检查程序文件夹权限。"));
    return false;
}

void Map::resetToBlankMap()
{
    m_newMapMayAutoBind = true;
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
    m_legacyRowMissionDocument = {};
    m_hasLegacyRowMissionDocument = false;

    bindNewMapToController();
    applyPresentationUpdates(true);
    updateMapNameDisplay();
    refreshRowWorkUi();
    syncCommittedMapState();
    setRouteStatusText(tr("已新建空白地图"));
}

bool Map::saveCurrentMapInteractive(bool forceChooseFile)
{
    QString filePath = m_currentMapFilePath;
    const bool saveAs = forceChooseFile || filePath.isEmpty();
    if (forceChooseFile || filePath.isEmpty()) {
        if (!ensureMapsDirectory()) return false;
        filePath = QFileDialog::getSaveFileName(m_mapPage, tr("保存地图"), suggestedMapFilePath(mapsDirectory()),
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
        pointObj.insert(QStringLiteral("dwellMs"), point->dwellMs);
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
        pathObj.insert(QStringLiteral("bidirectional"), path->bidirectional);
        pathsArray.append(pathObj);
    }
    root.insert(QStringLiteral("paths"), pathsArray);

    if (m_rowWorkPlan.hasStartPose || m_rowWorkPlan.hasEndPose || !m_rowWorkPlan.checkpoints.isEmpty()) {
        root.insert(QStringLiteral("rowWork"), RowWorkJson::planToJson(m_rowWorkPlan));
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
    for (const auto *key : {"rotationDeg", "gridWidth", "gridHeight", "cellSizeMeters"})
        content.remove(key);
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
    return confirmMapChanges(tr("关闭地图"));
}

bool Map::confirmMapChanges(const QString &title)
{
    if (!hasUnsavedMapChanges()) return true;
    QMessageBox dialog(QMessageBox::Question, title, tr("当前地图有未保存的修改，是否保存？"),
                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, m_mapPage);
    dialog.setObjectName(QStringLiteral("mapUnsavedChangesDialog"));
    dialog.button(QMessageBox::Save)->setText(tr("保存"));
    dialog.button(QMessageBox::Discard)->setText(tr("不保存"));
    dialog.button(QMessageBox::Cancel)->setText(tr("取消"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    const auto answer = dialog.exec();
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && saveCurrentMapInteractive(false));
}

void Map::clearMapData()
{
    m_pendingPointSelection.reset();
    clearLocalizationTrail();
    clearRowWorkGraphics();

    while (!m_paths.isEmpty()) {
        removePathInternal(m_paths.begin().key());
    }

    while (!m_points.isEmpty()) {
        removePointInternal(m_points.begin().key());
    }

    m_points.clear();
    m_paths.clear();

    m_routeQueue.clear();

    m_rowWorkPlan = RowWorkPlan{};
    m_rowWorkPlan.frameBinding = m_binding;
    m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    m_rowWorkPlan.frameId = QStringLiteral("map");
    m_rowWorkPlan.params.loopEnabled = true;
    m_legacyRowMissionDocument = {};
    m_hasLegacyRowMissionDocument = false;
    m_rowWorkCheckpointTableUpdating = false;
    m_rowWorkClickPlacementMode = false;
    m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;

    if (m_pointDwellSpin) m_pointDwellSpin->setValue(kDefaultMapPointDwellMs);
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
    m_newMapMayAutoBind = false;
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
        record.dwellMs = point.dwellMs;
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
        record.bidirectional = path.bidirectional;
        doc.paths.append(record);
    }

    if (m_rowWorkPlan.hasStartPose || m_rowWorkPlan.hasEndPose || !m_rowWorkPlan.checkpoints.isEmpty()) {
        doc.hasRowWorkPlan = true;
        doc.rowWorkPlan = m_rowWorkPlan;
    }

    if (m_hasLegacyRowMissionDocument) {
        doc.hasLegacyRowMissionDocument = true;
        doc.legacyRowMissionDocument = m_legacyRowMissionDocument;
    }

    return MapDocumentCodec::toJson(doc);
}

bool Map::deserializeMap(const QJsonObject &object)
{
    MapDocument doc;
    QString decodeError;
    if (!MapDocumentCodec::fromJson(object, &doc, &decodeError, kMaxMapSchemaVersion)) {
        qCWarning(lcMapModule) << "Map deserialize failed:" << decodeError;
        return false;
    }

    m_newMapMayAutoBind = false;
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
        if (addPointInternal(point.x, point.y, point.theta, point.id, point.dwellMs)) {
            maxPointId = std::max(maxPointId, point.id);
        }
    }

    int maxPathId = 0;
    for (const MapDocumentPath &path : doc.paths) {
        bool ok = false;
        if (path.type == MapDocumentPathType::Arc) {
            ok = addArcPath(path.startId, path.endId, path.sagitta, path.id, path.bidirectional);
        } else {
            ok = addLinePath(path.startId, path.endId, path.id, path.bidirectional);
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
    m_hasLegacyRowMissionDocument = doc.hasLegacyRowMissionDocument;
    m_legacyRowMissionDocument = doc.legacyRowMissionDocument;
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

    m_planningFingerprint.clear();
    refreshPlanningRevision();
    refreshControllerProfileRevision();
    return true;
}




void Map::refreshRowWorkUi()
{
    refreshRowWorkPlanSummary();
    refreshRowWorkCheckpointTable();
    refreshRowWorkControlState();
    refreshRowWorkGraphics();
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
    if (m_rowWorkLoopCheck) {
        QSignalBlocker blocker(m_rowWorkLoopCheck);
        m_rowWorkLoopCheck->setChecked(m_rowWorkPlan.params.loopEnabled);
    }
    if (m_rowWorkPlanLabel) {
        QString lineText = hasRowWorkLine()
                                     ? tr("%1 m · %2 个检查点")
                                           .arg(m_rowWorkPlan.lineLength(), 0, 'f', 2)
                                           .arg(m_rowWorkPlan.checkpoints.size())
                                     : RowWorkGeometry::hasUsableStart(m_rowWorkPlan)
                                         ? tr("已记录起点 · %1 个中间点 · 最后记录终点即可完成")
                                               .arg(m_rowWorkPlan.checkpoints.size())
                                         : tr("先记录起点，再沿途添加中间点，最后记录终点");
        const bool missingPositions = hasRowWorkLine() && std::any_of(
            m_rowWorkPlan.checkpoints.cbegin(), m_rowWorkPlan.checkpoints.cend(),
            [](const RowCheckpoint &checkpoint) { return !checkpoint.recordedMapPosition; });
        if (missingPositions)
            lineText += tr("\n旧地图仅保存直线站距，原始途经点坐标缺失，需重新采样以复现地面路线");
        else if (hasRowWorkLine() && !m_rowWorkPlan.checkpoints.isEmpty())
            lineText += tr("\n任务沿记录点行驶，保留示教路线的细微弯曲");
        m_rowWorkPlanLabel->setStyleSheet(missingPositions ? QStringLiteral("color:#946200;") : QString());
        m_rowWorkPlanLabel->setText(lineText);
    }

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
        if (checkpoint.recordedMapPosition) {
            progressItem->setFlags(progressItem->flags() & ~Qt::ItemIsEditable);
            progressItem->setToolTip(tr("由记录位置计算；如需修改路线，请重新记录该点位。"));
        } else if (!hasRowWorkLine()) {
            progressItem->setFlags(progressItem->flags() & ~Qt::ItemIsEditable);
            progressItem->setToolTip(tr("当前为距起点的暂记距离；记录终点后计算站距并保留记录位置。"));
        }
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
    const bool clientReady = m_tracking && m_tracking->commandFresh();
    const bool busy = m_tracking && m_tracking->busy();
    const bool hasLine = hasRowWorkLine();
    const bool hasStart = RowWorkGeometry::hasUsableStart(m_rowWorkPlan);
    const bool editable = canEditRowWorkPlan() && !busy;
    const bool mapEditingAllowed = editable;

    if (m_autoConnectPointsCheck) m_autoConnectPointsCheck->setEnabled(mapEditingAllowed);
    if (m_pointDwellSpin) m_pointDwellSpin->setEnabled(mapEditingAllowed);

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
        m_rowWorkCaptureStartButton->setEnabled(clientReady && editable);
        m_rowWorkCaptureStartButton->setToolTip(!editable ? tr("当前任务锁定计划，暂不能示教。")
            : !clientReady ? tr("等待工控机连接与任务状态更新后记录位置。")
            : tr("车辆停稳后，采集当前位置和航向作为起点 A。"));
    }
    if (m_rowWorkCaptureEndButton) {
        m_rowWorkCaptureEndButton->setEnabled(clientReady && editable);
        m_rowWorkCaptureEndButton->setToolTip(!editable ? tr("当前任务锁定计划，暂不能示教。")
            : !clientReady ? tr("等待工控机连接与任务状态更新后记录位置。")
            : tr("车辆停稳后，采集当前位置和航向作为终点 B。"));
    }
    if (m_rowWorkAddCheckpointFromVehicleButton) {
        m_rowWorkAddCheckpointFromVehicleButton->setEnabled(clientReady && hasStart && editable);
    }
    if (m_rowWorkAddCheckpointFromMapButton) {
        m_rowWorkAddCheckpointFromMapButton->setEnabled(hasStart && editable);
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
    if (m_rowWorkCheckpointTable) m_rowWorkCheckpointTable->setEnabled(editable);
    if (m_rowWorkLoopCheck) m_rowWorkLoopCheck->setEnabled(editable);
    if (m_rowWorkStartButton) {
        m_rowWorkStartButton->setEnabled(clientReady && hasLine && editable);
        const QString hint = !editable ? tr("当前任务锁定计划；结束后可重新示教。")
            : !hasLine ? tr("先记录起点和终点，可沿途添加检查点。")
            : !clientReady ? tr("等待工控机连接与任务状态更新。")
            : tr("先沿示教路径回 A，停稳后执行 A→B→A。");
        m_rowWorkStartButton->setToolTip(hint);
        if (m_rowWorkStartHintLabel) m_rowWorkStartHintLabel->setText(hint);
    }
}

void Map::refreshRowWorkGraphics()
{
    clearRowWorkGraphics();

    if (!m_scene || !RowWorkGeometry::hasUsableStart(m_rowWorkPlan)) {
        return;
    }

    const QPointF start = m_rowWorkPlan.startPose.toPointF();
    const QPointF end = m_rowWorkPlan.endPose.toPointF();
    const QPointF sceneStart = mapToScene(start);
    const QPointF sceneEnd = mapToScene(end);

    m_rowWorkStartMarker = m_scene->addEllipse(-6.0, -6.0, 12.0, 12.0,
                                               QPen(QColor(20, 120, 60)),
                                               QBrush(QColor(70, 180, 90)));
    m_rowWorkStartMarker->setPos(sceneStart);
    m_rowWorkStartMarker->setZValue(6.0);

    if (hasRowWorkLine()) {
        QPainterPath linePath;
        const auto line = RowWorkGeometry::pathBetweenProgress(m_rowWorkPlan, 0, m_rowWorkPlan.lineLength());
        for (qsizetype i = 0; i < line.size(); ++i) {
            const auto scene = mapToScene(line[i]);
            if (i == 0) linePath.moveTo(scene);
            else linePath.lineTo(scene);
        }
        m_rowWorkLineItem = m_scene->addPath(linePath, plannedRoutePen());
        m_rowWorkLineItem->setZValue(-3.5);

        m_rowWorkEndMarker = m_scene->addEllipse(-6.0, -6.0, 12.0, 12.0,
                                                 QPen(QColor(170, 80, 20)),
                                                 QBrush(QColor(230, 150, 70)));
        m_rowWorkEndMarker->setPos(sceneEnd);
        m_rowWorkEndMarker->setZValue(6.0);

    }

    for (int i = 0; i < m_rowWorkPlan.checkpoints.size(); ++i) {
        const RowCheckpoint &checkpoint = m_rowWorkPlan.checkpoints.at(i);
        const QPointF checkpointMapPos = RowWorkGeometry::checkpointPosition(m_rowWorkPlan, checkpoint);
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
    setRouteStatusText(text, warning);
}

void Map::updateRowWorkPlanVersion()
{
    if (m_rowWorkPlan.version == std::numeric_limits<int>::max()) {
        m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
        m_rowWorkPlan.version = 1;
    } else {
        m_rowWorkPlan.version = qMax(1, m_rowWorkPlan.version + 1);
    }
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

bool Map::addRowWorkCheckpoint(const QPointF &mapPos)
{
    if (!canEditRowWorkPlan()) return false;
    if (!RowWorkGeometry::hasUsableStart(m_rowWorkPlan)) {
        setRowWorkStatusText(tr("请先记录起点，再添加中间点"), true);
        return false;
    }

    RowCheckpoint checkpoint;
    checkpoint.name = nextCheckpointName();
    if (!RowWorkGeometry::setCheckpointPosition(m_rowWorkPlan, mapPos, &checkpoint)) {
        setRowWorkStatusText(tr("中间点位置无效，请检查起点和终点"), true);
        return false;
    }
    checkpoint.dwellMs = 2000;
    checkpoint.enabled = true;
    checkpoint.triggerOnForward = true;
    checkpoint.triggerOnBackward = true;

    auto candidate = m_rowWorkPlan;
    candidate.checkpoints.append(checkpoint);
    if (candidate.hasEndPose && !RowWorkGeometry::hasUsablePath(candidate)) {
        setRowWorkStatusText(tr("中间点与现有路线的位置关系冲突，未更改原路线，请重新记录点位"), true);
        return false;
    }
    m_rowWorkPlan = candidate;
    sortRowWorkCheckpoints();
    updateCheckpointRowNames();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
    return true;
}

void Map::sortRowWorkCheckpoints()
{
    std::stable_sort(m_rowWorkPlan.checkpoints.begin(), m_rowWorkPlan.checkpoints.end(), [](const RowCheckpoint &lhs, const RowCheckpoint &rhs) {
        return lhs.progress < rhs.progress;
    });
}

QString Map::nextCheckpointName() const
{
    return QStringLiteral("P%1").arg(m_rowWorkPlan.checkpoints.size() + 1);
}

bool Map::checkpointFromRow(int row, RowCheckpoint *outCheckpoint, QString *errorMessage) const
{
    const auto reject = [&](const QString &reason) {
        if (errorMessage) *errorMessage = reason;
        return false;
    };
    if (!outCheckpoint || !m_rowWorkCheckpointTable || row < 0
        || row >= m_rowWorkCheckpointTable->rowCount() || row >= m_rowWorkPlan.checkpoints.size()) {
        return reject(tr("中间点不存在，请重新选择"));
    }
    RowCheckpoint checkpoint = m_rowWorkPlan.checkpoints[row];

    if (const auto *nameItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableNameColumn)) {
        checkpoint.name = nameItem->text().trimmed();
    }
    if (const auto *progressItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableProgressColumn)) {
        const double original = checkpoint.progress;
        bool ok = true;
        checkpoint.progress = progressItem->text() == QString::number(original, 'f', 2)
            ? original : progressItem->text().toDouble(&ok);
        if (checkpoint.recordedMapPosition && checkpoint.progress != original)
            return reject(tr("记录点的站距由位置计算；如需修改路线，请重新记录该点位。"));
        if (!ok || !std::isfinite(checkpoint.progress) || checkpoint.progress < 0
            || (hasRowWorkLine() && checkpoint.progress > m_rowWorkPlan.lineLength())
            || (!hasRowWorkLine() && checkpoint.progress != original))
            return reject(tr("距离应为 0～%1 m 的有效数值").arg(m_rowWorkPlan.lineLength(), 0, 'f', 2));
    }
    if (const auto *dwellItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableDwellColumn)) {
        bool ok = false;
        checkpoint.dwellMs = dwellItem->text().toInt(&ok);
        if (!ok || checkpoint.dwellMs < 0 || checkpoint.dwellMs > 3600000)
            return reject(tr("停留时间应为 0～3600000 的整数，单位 ms"));
    }
    if (const auto *forwardItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableForwardColumn)) {
        checkpoint.triggerOnForward = forwardItem->checkState() == Qt::Checked;
    }
    if (const auto *backwardItem = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableBackwardColumn)) {
        checkpoint.triggerOnBackward = backwardItem->checkState() == Qt::Checked;
    }
    if (const auto* item = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableCaptureColumn)) checkpoint.capturePhoto = item->checkState() == Qt::Checked;
    if (const auto* item = m_rowWorkCheckpointTable->item(row, kRowWorkCheckpointTableTimeoutColumn)) {
        bool ok = false;
        checkpoint.actionTimeoutMs = item->text().toInt(&ok);
        if (!ok || checkpoint.actionTimeoutMs < 1000 || checkpoint.actionTimeoutMs > 3600000)
            return reject(tr("拍照超时应为 1000～3600000 的整数，单位 ms"));
    }
    *outCheckpoint = checkpoint;
    return true;
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
    if (m_rowWorkPendingCaptureTarget != RowWorkPendingCaptureTarget::None) return false;
    if (m_coordinator && m_coordinator->actionPending()) return false;
    return !m_tracking || (!m_tracking->busy() && (!m_tracking->connected() ||
        (!m_tracking->snapshot().isExecuting() && m_tracking->snapshot().state != "Paused")));
}

bool Map::submitRowWorkTask()
{
    if (!m_coordinator || !hasRowWorkLine() || !canEditRowWorkPlan()) return false;
    auto options = taskOptions(); options.repeatUntilStopped = m_rowWorkPlan.params.loopEnabled;
    const auto task = TaskCompiler::row(m_rowWorkPlan, m_binding, options, true, true);
    if (!task.ok()) { setRowWorkStatusText(task.error, true); return false; }
    const bool sent = m_coordinator->uploadAndStart(task.plan);
    if (sent) setRowWorkStatusText(tr("正在下发作业；车辆将先沿路径回到起点，停稳后开始作业"));
    return sent;
}


bool Map::requestPoseCapture(RowWorkPendingCaptureTarget target)
{
    if (!m_poseClient || !canEditRowWorkPlan()) return false;
    refreshPlanningRevision();
    if (!m_poseClient->capture(m_binding)) return false;
    m_rowWorkPendingCaptureTarget = target;
    m_pendingCaptureBinding = m_binding.toJson();
    m_pendingMapPointDwellMs = m_pointDwellSpin ? m_pointDwellSpin->value() : kDefaultMapPointDwellMs;
    updateRouteControlState();
    refreshRowWorkControlState();
    return true;
}

void Map::handleRowWorkCaptureStartPoint()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (requestPoseCapture(RowWorkPendingCaptureTarget::StartPose)) {
        setRowWorkStatusText(tr("正在采集停稳后的定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkCaptureEndPoint()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (requestPoseCapture(RowWorkPendingCaptureTarget::EndPose)) {
        setRowWorkStatusText(tr("正在采集停稳后的定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkAddCheckpointFromVehicle()
{
    if (!m_poseClient || !canEditRowWorkPlan()) return;
    if (!RowWorkGeometry::hasUsableStart(m_rowWorkPlan)) {
        setRowWorkStatusText(tr("请先记录起点，再添加中间点"), true);
        return;
    }
    if (requestPoseCapture(RowWorkPendingCaptureTarget::CheckpointFromVehicle)) {
        setRowWorkStatusText(tr("正在采集停稳后的定位样本…"));
    } else setRowWorkStatusText(tr("采样未开始：请确认停稳、定位和地图绑定"), true);
}

void Map::handleRowWorkAddCheckpointFromMap()
{
    if (!canEditRowWorkPlan()) return;
    if (!RowWorkGeometry::hasUsableStart(m_rowWorkPlan)) {
        setRowWorkStatusText(tr("请先记录起点，再添加中间点"), true);
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

void Map::handleRowWorkStart()
{
    refreshPlanningRevision();
    submitRowWorkTask();
}

void Map::handleRowWorkLoopChanged(bool checked)
{
    if (!canEditRowWorkPlan()) {
        if (m_rowWorkLoopCheck) {
            QSignalBlocker blocker(m_rowWorkLoopCheck);
            m_rowWorkLoopCheck->setChecked(m_rowWorkPlan.params.loopEnabled);
        }
        return;
    }
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
    if (m_rowWorkCheckpointTableUpdating || !canEditRowWorkPlan() || row < 0 || row >= m_rowWorkPlan.checkpoints.size()) {
        return;
    }

    RowCheckpoint checkpoint;
    QString error;
    if (!checkpointFromRow(row, &checkpoint, &error)) {
        refreshRowWorkCheckpointTable();
        setRowWorkStatusText(tr("未修改中间点：%1；已恢复原值").arg(error), true);
        return;
    }
    if (checkpoint.name.trimmed().isEmpty()) {
        checkpoint.name = QStringLiteral("P%1").arg(row + 1);
    }
    m_rowWorkPlan.checkpoints[row] = checkpoint;
    sortRowWorkCheckpoints();
    updateCheckpointRowNames();
    updateRowWorkPlanVersion();
    refreshRowWorkUi();
    setRowWorkStatusText(tr("已更新中间点"));
}

TaskCompileOptions Map::taskOptions() const
{
    TaskCompileOptions options;
    if (m_tracking) options.applyControllerDefaults(m_tracking->configuration());
    return options;
}
void Map::setControlCoordinator(ControlSessionCoordinator* c)
{
    if (m_coordinator || !c) return;
    m_coordinator = c; m_tracking = c->tracking(); m_poseClient = c->pose(); c->setBinding(m_binding);
    connect(c, &ControlSessionCoordinator::actionPendingChanged, this, [this] {
        updateRouteControlState(); refreshRowWorkControlState();
    });
    connect(m_tracking, &TrackingClient::configurationChanged, this, [this] {
        bindNewMapToController(); refreshControllerProfileRevision();
    });
    connect(c, &ControlSessionCoordinator::message, this, [this](const QString& text) {
        if (m_poseClient && m_poseClient->isReportingStreamError()) return;
        setRouteStatusText(text);
    });
    connect(m_tracking, &TrackingClient::statusChanged, this, &Map::applyTrackingStatus);
    connect(m_tracking, &TrackingClient::pendingChanged, this, [this] {
        updateRouteControlState(); refreshRowWorkControlState(); refreshControllerProfileRevision();
    });
    connect(m_tracking, &TrackingClient::availabilityChanged, this, [this](bool online, const QString& reason) {
        Q_UNUSED(online); Q_UNUSED(reason);
        refreshTaskStatus();
        updateRouteControlState(); refreshRowWorkPlanSummary(); refreshRowWorkControlState();
    });
    connect(m_tracking, &TrackingClient::taskReceived, this, &Map::reviewRemoteTask);
    connect(m_tracking, &TrackingClient::errorOccurred, this, [this] {
        m_taskReviewIdentity = {};
        m_checkpointReviewIdentity = {};
        updateRouteControlState();
    });
    connect(m_tracking, &TrackingClient::sessionChanged, this, [this](bool available) {
        if (available && !m_checkpointReviewIdentity.isEmpty())
            QTimer::singleShot(0, this, &Map::openCheckpointReview);
        updateRouteControlState();
    });
    connect(m_poseClient, &PoseClient::poseChanged, this, [this](const ControlPoseSnapshot& pose) {
        if (!m_binding.isUsable() || pose.originRevision != m_binding.context.originRevision ||
            pose.calibrationId != m_binding.context.calibrationId) {
            m_localizationTrailStartsSegment = true;
            m_hasVehiclePose = false; refreshVehicleGraphics(); return;
        }
        if (!pose.validForControl) {
            m_localizationTrailStartsSegment = true;
            m_hasVehiclePose = false; refreshVehicleGraphics(); return;
        }
        appendLocalizationTrail(pose);
        MapFrameAdapter adapter(m_binding); const auto point = adapter.fromEnu(pose.position);
        updateVehiclePose(point.x(), point.y(), adapter.yawFromEnu(pose.yaw));
    });
    connect(m_poseClient, &PoseClient::availabilityChanged, this, [this](bool ready, const QString&) {
        if (!ready) {
            m_localizationTrailStartsSegment = true;
            m_hasVehiclePose = false; refreshVehicleGraphics();
            if (m_rowWorkPendingCaptureTarget != RowWorkPendingCaptureTarget::None) {
                m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
                m_pendingCaptureBinding = {};
                setRowWorkStatusText(tr("定位连接已中断，本次采样已取消，请恢复连接并停稳后重新记录"), true);
                m_taskNoticeSource = TaskNoticeSource::CaptureInterrupted;
                updateRouteControlState();
                refreshRowWorkControlState();
            }
        }
        refreshRowWorkPlanSummary();
    });
    connect(m_poseClient, &PoseClient::frameChanged, this, [this] {
        m_localizationTrailStartsSegment = true;
    });
    connect(m_poseClient, &PoseClient::captureFinished, this, &Map::applyCapture);
    connect(m_poseClient, &PoseClient::errorOccurred, this, [this](const QString& reason) {
        if (m_poseClient->isReportingStreamError()) return;
        m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
        m_pendingCaptureBinding = {};
        updateRouteControlState();
        refreshRowWorkControlState();
        setRowWorkStatusText(TrackingJson::reasonText(reason), true);
    });
    connect(m_poseClient, &PoseClient::streamErrorOccurred, this, [this](const QString& reason) {
        updateRouteControlState();
        refreshRowWorkControlState();
        if (m_taskNoticeSource == TaskNoticeSource::CaptureInterrupted) return;
        setRowWorkStatusText(TrackingJson::reasonText(reason), true);
        m_taskNoticeSource = TaskNoticeSource::PoseStream;
    });
    connect(m_poseClient, &PoseClient::monitoringAvailabilityChanged, this, [this](bool current, const QString&) {
        if (current && m_taskNoticeSource == TaskNoticeSource::PoseStream)
            setRouteStatusText(QString());
    });
    bindNewMapToController();
    refreshControllerProfileRevision();
}
void Map::refreshControllerProfileRevision()
{
    if (!m_tracking || !m_tracking->fresh() || !m_binding.isUsable() ||
        !m_tracking->preparedPlan().isEmpty() || (m_coordinator && m_coordinator->actionPending())) return;
    const auto status = m_tracking->snapshot();
    if (status.state != "Idle" && !status.isTerminal()) return;
    const auto config = m_tracking->configuration();
    if (config["navigation_defaults"].toObject()["rotation_policy"].toString() != "vehicle_footprint" ||
        config["origin"].toObject()["active"].toObject()["originRevision"].toString() != m_binding.context.originRevision ||
        config["calibration"].toObject()["calibrationId"].toString() != m_binding.context.calibrationId) return;
    const auto profile = config["profile_revision"].toString();
    if (profile.isEmpty() || profile == m_binding.context.controllerProfileRevision) return;
    const auto previousContext = m_binding.context.toJson();
    m_binding.context.controllerProfileRevision = profile;
    auto update = [&](MapFrameBinding &binding) {
        if (binding.context.toJson() == previousContext)
            binding.context.controllerProfileRevision = profile;
    };
    update(m_rowWorkPlan.frameBinding);
    if (m_coordinator) m_coordinator->setBinding(m_binding);
    setRouteStatusText(tr("工控机控制配置已更新；地图坐标保持一致，可重新上传任务"));
}
void Map::bindNewMapToController()
{
    if (!m_newMapMayAutoBind || !m_tracking || !m_tracking->fresh() || !m_currentMapFilePath.isEmpty() ||
        !m_points.isEmpty() || !m_paths.isEmpty() || !m_routeQueue.isEmpty() ||
        m_rowWorkPlan.hasStartPose || m_rowWorkPlan.hasEndPose || !m_rowWorkPlan.checkpoints.isEmpty() ||
        m_hasLegacyRowMissionDocument || m_binding.confirmed || !m_binding.context.mapId.isEmpty() ||
        !m_binding.context.originRevision.isEmpty() || !m_binding.context.calibrationId.isEmpty() ||
        !m_binding.context.controllerProfileRevision.isEmpty() ||
        !m_binding.context.frameTransformRevision.isEmpty() || !m_binding.enuTranslation.isNull() ||
        m_binding.enuYawOffsetRad != 0)
        return;
    const auto config = m_tracking->configuration();
    if (config["navigation_defaults"].toObject()["rotation_policy"].toString() != "vehicle_footprint") return;
    const QString origin = config["origin"].toObject()["active"].toObject()["originRevision"].toString();
    const QString calibration = config["calibration"].toObject()["calibrationId"].toString();
    const QString profile = config["profile_revision"].toString();
    if (origin.isEmpty() || calibration.isEmpty() || profile.isEmpty()) return;
    m_binding.context = {TrackingJson::newId(), origin, TrackingJson::newId(), calibration, profile, 1};
    m_binding.confirmed = true;
    m_newMapMayAutoBind = false;
    m_rowWorkPlan.frameBinding = m_binding;
    if (m_coordinator) m_coordinator->setBinding(m_binding);
    syncCommittedMapState();
    setRouteStatusText(tr("新地图已使用工控机当前原点；可直接规划路径和上传任务"));
}
void Map::reviewRemoteTask(const QJsonObject &record)
{
    if (m_taskReviewOpen || !m_tracking) return;
    const auto current = m_tracking->snapshot();
    if (!m_tracking->commandFresh() || (current.state != "Ready" && current.state != "Paused")) return;
    m_taskReviewIdentity = {};
    QScopedValueRollback<bool> reviewing(m_taskReviewOpen, true);
    updateRouteControlState();
    const auto identityAtRead = taskIdentity(m_tracking->snapshot());
    refreshPlanningRevision();
    const auto bindingAtRead = m_binding.toJson();
    const auto plan = record["plan"].toObject();
    const bool matching = m_binding.isUsable() && plan["context"].toObject() == m_binding.context.toJson();
    const bool clean = !hasUnsavedMapChanges();
    QDialog dialog(m_mapPage);
    dialog.setObjectName("remoteTaskReviewDialog");
    dialog.setWindowTitle(tr("核对工控机实际执行计划"));
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(tr("任务 %1 · 版本 %2 · %3 个步骤\n计划身份 %4\n下方显示从工控机回读的实际计划。接续使用该计划，不使用本地新编辑的路线。")
        .arg(record["taskId"].toString()).arg(record["revision"].toInt())
        .arg(plan["steps"].toArray().size()).arg(record["planHash"].toString()), &dialog);
    summary->setWordWrap(true); layout->addWidget(summary);
    auto *splitter = new QSplitter(&dialog);
    auto *view = new QGraphicsView(splitter);
    view->setObjectName("remoteTaskPreview");
    auto *scene = new QGraphicsScene(view); view->setScene(scene);
    auto *steps = new QListWidget(splitter); steps->setObjectName("remoteTaskSteps");
    int index = 0;
    for (const auto &value : plan["steps"].toArray()) {
        const auto step = value.toObject();
        const auto type = step["type"].toString();
        QString detail;
        if (type == "follow_path" || type == "return_to_start") {
            const auto points = step["points"].toArray();
            const bool returning = type == "return_to_start";
            detail = returning ? tr("先沿路径返回起点，限速 %1 m/s").arg(step["speedLimit"].toDouble())
                : tr("路径，%1 点，限速 %2 m/s").arg(points.size()).arg(step["speedLimit"].toDouble());
            if (matching) {
                MapFrameAdapter adapter(m_binding); QPainterPath path; bool first = true;
                for (const auto &point : points) {
                    const auto xy = point.toArray();
                    if (xy.size() != 2 || !xy[0].isDouble() || !xy[1].isDouble()) continue;
                    const auto position = mapToScene(adapter.fromEnu({xy[0].toDouble(), xy[1].toDouble()}));
                    if (first) { path.moveTo(position); first = false; }
                    else path.lineTo(position);
                }
                scene->addPath(path, plannedRoutePen(returning));
                auto *label = scene->addText(QString::number(index + 1)); label->setPos(path.pointAtPercent(0));
            }
        } else if (type == "turn") detail = tr("原地转向，ENU 航向 %1°").arg(qRadiansToDegrees(step["targetYaw"].toDouble()), 0, 'f', 1);
        else if (type == "wait") {
            const auto completion = step["completion"].toObject();
            detail = completion["type"].toString() == "external_ack" ? tr("等待外部确认：%1").arg(completion["eventType"].toString())
                : tr("停留 %1 ms").arg(completion["durationMs"].toInt());
        } else detail = type;
        steps->addItem(tr("%1. %2 · %3").arg(++index).arg(step["stepId"].toString(), detail));
    }
    layout->addWidget(splitter, 1);
    auto *planLegend = new QLabel(tr("蓝色虚线：正式规划路径 · 青色点划线：返回起点计划"), &dialog);
    planLegend->setWordWrap(true);
    layout->addWidget(planLegend);
    auto *notice = new QLabel(!matching ? tr("当前地图坐标或版本不一致，不能接续。请加载对应地图并重新回读。")
        : !clean ? tr("本地地图有未保存修改，不能接续。请先保存或重新加载对应地图，然后重新回读。")
        : tr("请核对回读路径、步骤与当前地图、现场一致；确认不会触发运动。"), &dialog);
    notice->setWordWrap(true); layout->addWidget(notice);
    auto *verified = new QCheckBox(tr("已核对工控机实际计划与当前地图及现场一致"), &dialog);
    verified->setObjectName("remoteTaskVerified"); verified->setEnabled(matching && clean); layout->addWidget(verified);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("确认接续计划"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(verified, &QCheckBox::toggled, buttons->button(QDialogButtonBox::Ok), &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    QTimer::singleShot(0, &dialog, [view, scene] { view->fitInView(scene->itemsBoundingRect().adjusted(-10, -10, 10, 10), Qt::KeepAspectRatio); });
    const auto answer = dialog.exec();
    // End the modal guard before updating the ordinary controls on every exit.
    m_taskReviewOpen = false;
    updateRouteControlState();
    if (answer != QDialog::Accepted || !verified->isChecked()) return;
    refreshPlanningRevision();
    if (hasUnsavedMapChanges() || bindingAtRead != m_binding.toJson() || !m_coordinator
        || !m_tracking->commandFresh() || identityAtRead != taskIdentity(m_tracking->snapshot())) {
        setRowWorkStatusText(tr("地图在核对期间已改变，请重新读取任务"), true); return;
    }
    m_coordinator->confirmReadTask(record);
    setRouteStatusText(tr("任务核对已提交；确认完成后再次点击“启动任务”或“继续任务”，车辆才会运动"));
}
void Map::applyTrackingStatus(const TrackingSnapshot& status)
{
    if (!m_taskReviewIdentity.isEmpty() && (m_taskReviewIdentity != taskIdentity(status)
        || (status.state != "Ready" && status.state != "Paused"))) m_taskReviewIdentity = {};
    if (!m_checkpointReviewIdentity.isEmpty() && (m_checkpointReviewIdentity != checkpointIdentity(status)
        || (status.state != "Waiting" && status.state != "Paused"))) m_checkpointReviewIdentity = {};
    const QHash<QString, QString> states{
        {"Idle", tr("待命")}, {"Ready", tr("任务就绪")}, {"Starting", tr("正在启动")},
        {"Aligning", tr("对准路径")}, {"Tracking", tr("执行中")}, {"Braking", tr("减速停车")},
        {"Turning", tr("转向中")}, {"Waiting", tr("等待中")}, {"Pausing", tr("正在暂停")},
        {"Paused", tr("已暂停")}, {"Aborting", tr("正在结束")}, {"Completed", tr("已完成")},
        {"Aborted", tr("已结束")}, {"Fault", tr("故障停车")}};
    const bool delayed = m_tracking && !m_tracking->fresh();
    QString title = states.value(status.state, tr("未连接"));
    if (status.returningToStart && status.isExecuting()
        && status.state != "Pausing" && status.state != "Aborting")
        title = tr("正在返回起点") + QStringLiteral(" · ") + title;
    if (delayed && !status.state.isEmpty()) title += tr(" · 状态延迟");
    const QString detail = tr("任务 %1\n步骤 %2 · 循环 %3\n暂停 %4\n故障 %5\n结果 %6\n工控机独立执行；无线状态延迟不代表车辆已停止。")
        .arg(status.taskId, status.stepId).arg(status.loopIndex)
        .arg(TrackingJson::reasonText(status.pauseReason), TrackingJson::reasonText(status.faultCode), status.result);
    if (m_trackingStatusLabel) {
        m_trackingStatusLabel->setText(title);
        m_trackingStatusLabel->setToolTip(detail);
        const auto style = QStringLiteral("font-size:16px; font-weight:600; color:%1;")
            .arg(status.state == "Fault" ? "#b42318" : delayed || status.state == "Paused" ? "#946200" : "#244d3a");
        if (m_trackingStatusLabel->styleSheet() != style) m_trackingStatusLabel->setStyleSheet(style);
    }
    if (m_taskProgressLabel) {
        QString progress = status.taskId.isEmpty() ? tr("等待任务") : tr("步骤 %1").arg(status.stepIndex + 1);
        if (status.returningToStart && !status.isTerminal()) progress += tr(" · 回起点准备");
        if (status.lengthMeters > 0)
            progress += tr(" · %1 / %2 m").arg(status.progressMeters, 0, 'f', 2).arg(status.lengthMeters, 0, 'f', 2);
        if (status.loopIndex > 0) progress += tr(" · 第 %1 轮").arg(status.loopIndex + 1);
        if (status.turnRecovering) progress += tr("\n转向回点 · 偏移 %1 m").arg(status.turnOffsetMeters, 0, 'f', 2);
        else if (status.autoResumePending) progress += tr("\n等待恢复，停稳后自动继续");
        else if (!status.faultCode.isEmpty()) progress += "\n" + TrackingJson::reasonText(status.faultCode);
        else if (!status.pauseReason.isEmpty()) progress += "\n" + TrackingJson::reasonText(status.pauseReason);
        if (status.state == "Fault") progress += tr("\n排除原因并停稳后复位，再重新下发任务");
        else if ((status.state == "Paused" || status.state == "Ready") && !hasConfirmedTask())
            progress += tr("\n工控机已有任务，点击%1先核对计划")
                .arg(status.state == "Ready" ? tr("“启动任务”") : tr("“继续任务”"));
        m_taskProgressLabel->setText(progress);
        m_taskProgressLabel->setToolTip(detail);
    }
    updateRouteControlState(); refreshRowWorkPlanSummary(); refreshRowWorkControlState();
}

void Map::refreshTaskStatus()
{
    if (!m_tracking || !m_tracking->commandFresh()) {
        m_taskReviewIdentity = {};
        m_checkpointReviewIdentity = {};
    }
    applyTrackingStatus(m_tracking ? m_tracking->snapshot() : TrackingSnapshot{});
}

void Map::setExternalActionReviewEvent(const QString &eventId)
{
    if (m_externalReviewEventId == eventId) return;
    m_externalReviewEventId = eventId;
    m_checkpointReviewIdentity = {};
    updateRouteControlState();
}

void Map::handleCheckpointReview()
{
    if (!m_tracking || !m_coordinator || !m_tracking->commandFresh() || m_tracking->busy()
        || m_coordinator->actionPending() || m_checkpointReviewOpen
        || !m_checkpointReviewIdentity.isEmpty() || m_taskReviewOpen || !m_taskReviewIdentity.isEmpty()) return;
    const auto status = m_tracking->snapshot();
    if (m_externalReviewEventId.isEmpty() || status.waitingEventId != m_externalReviewEventId
        || (status.state != "Waiting" && status.state != "Paused")) return;
    m_checkpointReviewIdentity = checkpointIdentity(status);
    const auto generation = ++m_checkpointReviewGeneration;
    updateRouteControlState();
    if (m_tracking->hasSession()) {
        openCheckpointReview();
        return;
    }
    const auto expected = m_checkpointReviewIdentity;
    setRouteStatusText(tr("正在准备检查点核对连接，不会重拍或恢复运动"));
    m_coordinator->acquireSession();
    QTimer::singleShot(8000, this, [this, expected, generation] {
        if (m_checkpointReviewGeneration != generation || m_checkpointReviewIdentity != expected) return;
        m_checkpointReviewIdentity = {};
        updateRouteControlState();
        setRouteStatusText(tr("核对连接尚未就绪，请检查连接后重新点击；拍摄结果保持未确认"), true);
    });
}

void Map::openCheckpointReview()
{
    if (m_checkpointReviewIdentity.isEmpty() || m_checkpointReviewOpen || !m_tracking) return;
    const auto expected = m_checkpointReviewIdentity;
    const auto status = m_tracking->snapshot();
    m_checkpointReviewIdentity = {};
    if (!m_tracking->commandFresh() || !m_tracking->hasSession() || m_tracking->busy()
        || expected != checkpointIdentity(status) || status.waitingEventId != m_externalReviewEventId
        || (status.state != "Waiting" && status.state != "Paused")) {
        updateRouteControlState();
        return;
    }
    QScopedValueRollback<bool> reviewing(m_checkpointReviewOpen, true);
    updateRouteControlState();
    QMessageBox dialog(QMessageBox::Question, tr("检查点拍摄结果"),
        tr("请核对检查点 %1 的实际照片。\n“是”确认拍摄成功；“否”报告失败；取消保持等待。不会再次拍摄。")
            .arg(status.waitingEventId), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, m_mapPage);
    dialog.setObjectName("checkpointReviewDialog");
    dialog.setDefaultButton(QMessageBox::Cancel);
    const auto answer = dialog.exec();
    m_checkpointReviewOpen = false;
    updateRouteControlState();
    if (answer != QMessageBox::Yes && answer != QMessageBox::No) return;
    const auto current = m_tracking->snapshot();
    if (!m_tracking->commandFresh() || !m_tracking->hasSession() || m_tracking->busy()
        || expected != checkpointIdentity(current)
        || current.waitingEventId != m_externalReviewEventId
        || (current.state != "Waiting" && current.state != "Paused")) {
        setRouteStatusText(tr("检查点或连接已改变，本次确认已取消，请核对当前任务"), true);
        return;
    }
    emit externalActionResolved(current.waitingEventId, answer == QMessageBox::Yes);
}

void Map::applyCapture(const QJsonObject& capture)
{
    const auto target = m_rowWorkPendingCaptureTarget; m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    const auto expectedBinding = m_pendingCaptureBinding; m_pendingCaptureBinding = {};
    updateRouteControlState();
    refreshRowWorkControlState();
    if (target == RowWorkPendingCaptureTarget::None) return;
    if (!canEditRowWorkPlan() ||
        (target == RowWorkPendingCaptureTarget::MapPoint && !m_editModeEnabled) ||
        (!expectedBinding.isEmpty() && expectedBinding != m_binding.toJson())) {
        setRowWorkStatusText(tr("地图、坐标或编辑状态已改变，本次延迟到达的采样结果未应用"), true);
        return;
    }
    if (capture["state"].toString() != "succeeded" || !m_binding.isUsable() ||
        capture["originRevision"].toString() != m_binding.context.originRevision ||
        capture["calibrationId"].toString() != m_binding.context.calibrationId) {
        setRowWorkStatusText(tr("采样失败或坐标版本改变：%1").arg(TrackingJson::reasonText(capture["reason"].toString())), true); return;
    }
    const auto o = capture["pose"].toObject();
    if (!o["x"].isDouble() || !o["y"].isDouble() || !o["yaw"].isDouble()) {
        setRowWorkStatusText(tr("采样结果缺少有效的位置或朝向，未记录点位，请重新采样"), true);
        return;
    }
    MapFrameAdapter adapter(m_binding); const auto point = adapter.fromEnu({o["x"].toDouble(), o["y"].toDouble()});
    RowWorkPose pose{point.x(), point.y(), adapter.yawFromEnu(o["yaw"].toDouble())};
    if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.yaw)) {
        setRowWorkStatusText(tr("采样位置无效，请重新记录"), true);
        return;
    }
    if (target == RowWorkPendingCaptureTarget::MapPoint) {
        const int pointId = m_nextPointId;
        if (!addPointInternal(pose.x, pose.y, pose.yaw, std::nullopt, m_pendingMapPointDwellMs)) {
            setRouteStatusText(tr("采样完成但地图点未添加：点或路径编号不可用，或新点与自动连接的上一点重合"), true);
            return;
        }
        m_pendingPointSelection = pointId;
        refreshPointUi(); handlePointSelectionChanged(); refreshSelectors(); refreshPlanningRevision();
        setRouteStatusText(tr("已采样新增地图点 %1：%2 个样本，位置散布 %3 m")
            .arg(pointId).arg(capture["sampleCount"].toInt())
            .arg(capture["positionScatterMeters"].toDouble(), 0, 'f', 3));
        return;
    }
    if (!m_rowWorkPlan.frameBinding.isUsable() ||
        !m_rowWorkPlan.frameBinding.context.sameCoordinates(m_binding.context)) {
        m_rowWorkPlan = RowWorkPlan{}; m_rowWorkPlan.frameBinding = m_binding;
        m_rowWorkPlan.planId = RowWorkJson::generatePlanId();
    }
    switch (target) {
    case RowWorkPendingCaptureTarget::StartPose:
        if (!m_rowWorkPlan.checkpoints.isEmpty() &&
            (!pointsAlmostEqual(m_rowWorkPlan.startPose.toPointF(), point) ||
             std::abs(normalizeAngle(m_rowWorkPlan.startPose.yaw - pose.yaw)) > kGeometryEpsilon)) {
            setRowWorkStatusText(tr("已记录中间点，请先清空中间点再更改起点"), true);
            return;
        }
        m_rowWorkPlan.startPose = pose; m_rowWorkPlan.hasStartPose = true;
        break;
    case RowWorkPendingCaptureTarget::EndPose: {
        auto candidate = m_rowWorkPlan;
        candidate.endPose = pose; candidate.hasEndPose = true;
        if ((candidate.hasStartPose || !candidate.checkpoints.isEmpty()) &&
            !RowWorkGeometry::finalizeCheckpoints(&candidate)) {
            setRowWorkStatusText(tr("终点无效或中间点不在起终点之间，请重新记录终点"), true);
            return;
        }
        m_rowWorkPlan = candidate;
        sortRowWorkCheckpoints();
        break;
    }
    case RowWorkPendingCaptureTarget::CheckpointFromVehicle:
        if (addRowWorkCheckpoint(point))
            setRowWorkStatusText(hasRowWorkLine() ? tr("已记录当前位置为中间点")
                : tr("已记录中间点，可继续沿途添加，最后记录终点"));
        return;
    case RowWorkPendingCaptureTarget::None:
    case RowWorkPendingCaptureTarget::MapPoint: return;
    }
    updateRowWorkPlanVersion(); refreshRowWorkUi();
    setRowWorkStatusText(tr("示教完成：%1 个定位样本，散布 %2 m")
        .arg(capture["sampleCount"].toInt()).arg(capture["positionScatterMeters"].toDouble(), 0, 'f', 3));
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
    QDialog dialog(m_mapPage); dialog.setWindowTitle(tr("地图坐标绑定与变换"));
    auto* layout = new QFormLayout(&dialog);
    auto* text = new QLabel(tr("此处只维护本地图到工控机 ENU 的变换与版本绑定，不修改基准站坐标。\n地图南/东轴先转为东/北，再应用旋转和平移。请填写经测量确认的变换。\n原点 %1 · 标定 %2 · 控制配置 %3")
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
    m_coordinator->setBinding(m_binding);
    setRouteStatusText(tr("地图坐标已确认，请保存地图后上传新任务"));
}
