#ifndef MAP_H
#define MAP_H

#include "rowworktypes.h"
#include "rowmissiontypes.h"

#include <QObject>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QByteArray>
#include <QElapsedTimer>
#include <optional>

#include <QMouseEvent>

class QGraphicsScene;
class QGraphicsItemGroup;
class QGraphicsEllipseItem;
class QGraphicsTextItem;
class QGraphicsPathItem;
class QGraphicsLineItem;
class QGraphicsRectItem;
class QHBoxLayout;
class QVBoxLayout;
class QFormLayout;
class QTableWidget;
class QListWidget;
class QPushButton;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QSlider;
class QLabel;
class QTimer;
class QTabWidget;
class QFrame;
class QGroupBox;
class QWidget;
class QJsonObject;
class QShortcut;
class QLineEdit;
class RowWorkClient;

class MapGraphicsView;

namespace Ui {
class MainWindow;
}

class Map : public QObject
{
    Q_OBJECT

public:
    explicit Map(Ui::MainWindow *ui, QObject *parent = nullptr);

public slots:
    void handleModuleActivated();
    void updateVehiclePose(double x, double y, double theta);
    void handleRouteSegmentCompleted(bool success);
    void handleRowWorkStatusUpdate(const RowWorkStatus &status);
    void applyRuntimeConfig();

public:
    enum class PathType {
        Line,
        Arc
    };

signals:
    void routeSegmentDispatched(int fromPointId, int toPointId, const QList<QPointF> &pathPolyline,
                                double startTheta, double endTheta);
    void routeQueueCompletedOnce();
    void routeExecutionCancelled();
    void routeExecutionPauseRequested();
    void routeExecutionResumeRequested();
    void rowWorkAutoStartRequested();
    void rowWorkAutoStopRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void handleSceneClick(const QPointF &scenePos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void handleGenerateMap();
    void handleAddPointFromInput();
    void handlePointSelectionChanged();
    void handleUpdatePoint();
    void handleAddPointFromClickRequested();
    void handleRemoveSelectedPoints();
    void handleAddPath();
    void handleRemoveSelectedPaths();
    void handleBatchGeneratePoints();
    void handleRouteAdd();
    void handleRouteRemove();
    void handleRouteClear();
    void handleRouteStart();
    void handleRoutePause();
    void handleRouteResume();
    void handleRouteStop();
    void handleRouteTimerTick();
    void handleLocateCurrentPosition();
    void handleEditModeToggled(bool checked);
    void handleZoomIn();
    void handleZoomOut();
    void handleNewMap();
    void handleLoadMap();
    void handleSaveMap();
    void handleQuickSave();
    void handleTogglePathsVisibility();
    void handleSceneMouseMoved(const QPointF &scenePos);
    void handleRotationSliderValueChanged(int value);
    void handleRotationSpinChanged(double value);
    void handleRowWorkCaptureStartPoint();
    void handleRowWorkCaptureEndPoint();
    void handleRowWorkAddCheckpointFromVehicle();
    void handleRowWorkAddCheckpointFromMap();
    void handleRowWorkDeleteCheckpoint();
    void handleRowWorkClearCheckpoints();
    void handleRowWorkClearPlan();
    void handleRowWorkReadPlan();
    void handleRowWorkUploadPlan();
    void handleRowWorkStart();
    void handleRowWorkPause();
    void handleRowWorkResume();
    void handleRowWorkStop();
    void handleRowWorkLoopChanged(bool checked);
    void handleRowWorkCheckpointCellChanged(int row, int column);

    void handleRowMissionNameEdited();
    void handleRowMissionLoopChanged(bool checked);
    void handleRowMissionStepSelectionChanged();
    void handleRowMissionStepCellChanged(int row, int column);
    void handleRowMissionTypeChanged(int index);
    void handleRowMissionStepNameEdited();
    void handleRowMissionStepNoteEdited();
    void handleRowMissionTargetYawChanged(double value);
    void handleRowMissionDwellChanged(int value);
    void handleRowMissionAddRowLegStep();
    void handleRowMissionAddTransferStep();
    void handleRowMissionAddTurnStep();
    void handleRowMissionAddWaitStep();
    void handleRowMissionRemoveStep();
    void handleRowMissionMoveStepUp();
    void handleRowMissionMoveStepDown();
    void handleRowMissionClearSteps();
    void handleRowMissionImportCurrentRowWork();
    void handleRowMissionApplyStepEdits();

private:
    struct MapPoint {
        int id = -1;
        QPointF mapPosition;
        double theta = 0.0;
        QGraphicsItemGroup *markerGroup = nullptr;
        QGraphicsEllipseItem *circle = nullptr;
        QGraphicsPathItem *arrow = nullptr;
        QGraphicsTextItem *label = nullptr;
    };

    struct MapPath {
        int id = -1;
        int startId = -1;
        int endId = -1;
        PathType type = PathType::Line;
        QList<QPointF> polyline;
        double sagitta = 0.0;
        QPointF arcCenter;
        double radius = 0.0;
        double sweepAngleRad = 0.0;
        QGraphicsPathItem *pathItem = nullptr;
        QGraphicsPathItem *arrowItem = nullptr;
    };

    struct RouteStep {
        int fromId = -1;
        int toId = -1;
        QList<int> pathIds;
        QList<int> pointSequence;
        int progressEdgeIndex = 0;
    };

    enum class RowWorkPendingCaptureTarget {
        None,
        StartPose,
        EndPose,
        CheckpointFromVehicle
    };

    void initializeUi();
    void ensureScene();
    void rebuildGrid();
    void clearGrid();
    void ensureVehicleItem();

    int allocatePointId();
    int allocatePathId();
    void resetNextPointId();
    void resetNextPathId();

    bool addPointInternal(double x, double y, double theta, std::optional<int> forcedId = std::nullopt);
    void removePointInternal(int pointId);
    bool addLinePath(int startId, int endId, std::optional<int> forcedId = std::nullopt);
    bool addArcPath(int startId, int endId, double sagitta, std::optional<int> forcedId = std::nullopt);
    bool addPathInternal(MapPath &&path);
    void removePathInternal(int pathId);
    void refreshPathGeometry(MapPath &path);
    int addPathOrWarn(int startId, int endId, PathType type, double sagitta = 0.0, std::optional<int> forcedId = std::nullopt);
    int findPathId(int startId, int endId) const;
    void updatePointGraphics(MapPoint &point);
    void refreshAllPointGraphics();
    void refreshVehicleGraphics();
    void applyVehiclePoseFromUi();
    void refreshPathsForPoint(int pointId);
    void autoSelectRoutePoints(int startId, int endId);

    QList<int> findRoutePathIds(int startId, int endId) const;
    QList<QPointF> composePolyline(const QList<int> &pathIds) const;
    int findNearestPointId(const QPointF &mapPos, double thresholdMeters, double *outDistance = nullptr) const;
    void updatePointTableRow(int pointId);
    double angleFromMapVector(double dx, double dy) const;
    QList<int> buildPointSequenceFromPaths(const QList<int> &pathIds, int startId) const;
    bool rebuildRouteStep(RouteStep &step);
    bool rebuildRemainingRouteFrom(int startIndex, int startPointId, QString *errorMessage = nullptr);
    std::optional<int> resolveDynamicReplanStartPoint() const;
    bool tryDynamicReplanAfterFailure(QString *errorMessage = nullptr);

    void refreshPointUi();
    void refreshPathUi();
    void refreshSelectors();
    void refreshRouteQueueUi();
    void updateRouteControlState();
    void setRouteStatusText(const QString &text, bool warning = false);
    void setMapRotation(double rotationDeg);
    double normalizedRotationDeg(double rotationDeg) const;
    double mapHeadingToSceneAngle(double headingRad) const;
    void refreshAllPathGeometry();
    void applyPresentationUpdates(bool recenterView);

    QPointF sceneToMap(const QPointF &scenePoint) const;
    QPointF mapToScene(const QPointF &mapPoint) const;

    MapPath *pathById(int id);
    const MapPath *pathById(int id) const;

    MapPoint *pointById(int id);
    const MapPoint *pointById(int id) const;

    void updateMapNameDisplay();
    void updateVehiclePointBinding();
    void resetRouteProgress();
    void dispatchNextEdge();
    bool isRouteQueueContinuous() const;
    void scheduleNextCycle();
    void resetToBlankMap();
    bool saveCurrentMapInteractive(bool forceChooseFile);
    QByteArray buildComparableMapState() const;
    void syncCommittedMapState();
    bool hasUnsavedMapChanges() const;

    void clearMapData();
    bool saveMapToFile(const QString &filePath) const;
    bool loadMapFromFile(const QString &filePath);
    QJsonObject serializeMap() const;
    bool deserializeMap(const QJsonObject &object);
    void ensureRowWorkClient();
    void connectRowWorkClientSignals();
    void refreshRowWorkUi();
    void refreshRowWorkPlanSummary();
    void refreshRowWorkCheckpointTable();
    void refreshRowWorkControlState();
    void refreshRowWorkGraphics();
    void clearRowWorkGraphics();
    void setRowWorkStatusText(const QString &text, bool warning = false);
    void updateRowWorkPlanVersion();
    bool hasRowWorkLine() const;
    void clearRowWorkPlanInternal(bool keepStatusMessage = false);
    void addRowWorkCheckpoint(const QPointF &mapPos);
    void sortRowWorkCheckpoints();
    QString nextCheckpointName() const;
    RowCheckpoint checkpointFromRow(int row) const;
    void updateCheckpointRowNames();
    bool canEditRowWorkPlan() const;
    bool uploadRowWorkPlanIfNeeded(bool forceUpload);
    bool rowWorkPlanMatchesStatus() const;

    void refreshRowMissionUi();
    void refreshRowMissionSummary();
    void refreshRowMissionStepTable();
    void refreshRowMissionControlState();
    void refreshRowMissionStepEditor();
    void setRowMissionStatusText(const QString &text, bool warning = false);
    void updateRowMissionPlanVersion();
    bool canEditRowMissionPlan() const;
    RowMissionStep *rowMissionStepAt(int row);
    const RowMissionStep *rowMissionStepAt(int row) const;
    int rowMissionSelectedStepRow() const;
    QString rowMissionStepTypeText(RowMissionStepType type) const;
    QString rowMissionStepSummaryText(const RowMissionStep &step) const;
    RowMissionStep buildMissionStepFromCurrentRowWork(RowMissionStepType type) const;
    void insertRowMissionStep(const RowMissionStep &step, int row = -1);
    void moveRowMissionStep(int fromRow, int toRow);
    void updateRowMissionStepNames();
    void loadRowMissionStepEditor(int row);
    void applyRowMissionStepEditor(int row);
    void syncRowMissionEditorWidgets();
    void selectRowMissionStep(int row);

    Ui::MainWindow *ui;
    RowWorkClient *m_rowWorkClient = nullptr;

    QWidget *m_mapPage = nullptr;
    MapGraphicsView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;
    QGraphicsItemGroup *m_gridGroup = nullptr;
    QGraphicsEllipseItem *m_originItem = nullptr;

    double m_cellSizeMeters = 1.0;
    double m_cellSizePixels = 50.0;
    double m_mapRotationDeg = 0.0;
    double m_baseLatitudeDeg = 0.0;
    double m_baseLongitudeDeg = 0.0;
    bool m_initialized = false;
    bool m_pathsVisible = true;
    int m_gridWidth = 0;
    int m_gridHeight = 0;

    int m_nextPointId = 1;
    int m_nextPathId = 1;

    QHash<int, MapPoint> m_points;
    QHash<int, MapPath> m_paths;
    QHash<int, QList<int>> m_outgoingPathIds;

    QGraphicsItemGroup *m_vehicleItem = nullptr;
    QGraphicsRectItem *m_vehicleBody = nullptr;
    QGraphicsItemGroup *m_vehiclePoseMarker = nullptr;
    QGraphicsEllipseItem *m_vehiclePoseCircle = nullptr;
    QGraphicsPathItem *m_vehiclePoseArrow = nullptr;
    QGraphicsPathItem *m_rowWorkLineItem = nullptr;
    QGraphicsPathItem *m_rowWorkDirectionArrowItem = nullptr;
    QGraphicsEllipseItem *m_rowWorkStartMarker = nullptr;
    QGraphicsEllipseItem *m_rowWorkEndMarker = nullptr;
    QList<QGraphicsItemGroup *> m_rowWorkCheckpointMarkers;

    std::optional<int> m_vehicleCurrentPointId;
    bool m_hasVehiclePose = false;
    double m_vehiclePoseX = 0.0;
    double m_vehiclePoseY = 0.0;
    double m_vehiclePoseTheta = 0.0;
    bool m_hasRenderedVehiclePose = false;
    double m_lastRenderedVehiclePoseX = 0.0;
    double m_lastRenderedVehiclePoseY = 0.0;
    double m_lastRenderedVehiclePoseTheta = 0.0;
    QElapsedTimer m_vehiclePoseRefreshClock;
    bool m_editModeEnabled = false;
    int m_draggedPointId = -1;
    bool m_draggedPointMoved = false;
    bool m_draggedPointRotated = false;
    QPointF m_contextMenuScenePos;
    std::optional<int> m_altRouteStartPointId;
    bool m_altRotationActive = false;
    double m_rotationStartTheta = 0.0;
    double m_rotationReferenceAngle = 0.0;
    bool m_ctrlDragActive = false;
    QPointF m_ctrlDragOffset;
    bool m_ctrlAltPathActive = false;
    std::optional<int> m_ctrlAltStartPointId;
    bool m_waitingForSegmentCompletion = false;
    bool m_pauseRequested = false;

    // Left panel controls
    QSpinBox *m_gridWidthSpin = nullptr;
    QSpinBox *m_gridHeightSpin = nullptr;
    QDoubleSpinBox *m_gridRotationSpin = nullptr;
    QPushButton *m_buildGridButton = nullptr;
    QSlider *m_rotationSlider = nullptr;

    QDoubleSpinBox *m_pointXSpin = nullptr;
    QDoubleSpinBox *m_pointYSpin = nullptr;
    QDoubleSpinBox *m_pointThetaSpin = nullptr;
    QPushButton *m_addPointFromInputButton = nullptr;
    QPushButton *m_addPointFromClickButton = nullptr;
    QPushButton *m_updatePointButton = nullptr;
    QPushButton *m_removePointButton = nullptr;
    QPushButton *m_batchGenerateButton = nullptr;
    QTableWidget *m_pointTable = nullptr;
    QComboBox *m_batchBasePointCombo = nullptr;
    QComboBox *m_batchDirectionCombo = nullptr;
    QSpinBox *m_batchCountSpin = nullptr;
    QDoubleSpinBox *m_batchSpacingSpin = nullptr;
    QDoubleSpinBox *m_batchThetaSpin = nullptr;

    QComboBox *m_startPointCombo = nullptr;
    QComboBox *m_endPointCombo = nullptr;
    QComboBox *m_pathTypeCombo = nullptr;
    QDoubleSpinBox *m_arcSagittaSpin = nullptr;
    QPushButton *m_addPathButton = nullptr;
    QPushButton *m_removePathButton = nullptr;
    QTableWidget *m_pathTable = nullptr;

    // Right panel controls
    QComboBox *m_routeStartCombo = nullptr;
    QComboBox *m_routeEndCombo = nullptr;
    QPushButton *m_routeAddButton = nullptr;
    QListWidget *m_routeQueueList = nullptr;
    QPushButton *m_routeRemoveButton = nullptr;
    QPushButton *m_routeClearButton = nullptr;
    QPushButton *m_routeStartButton = nullptr;
    QPushButton *m_routePauseButton = nullptr;
    QPushButton *m_routeResumeButton = nullptr;
    QPushButton *m_routeStopButton = nullptr;
    QCheckBox *m_routeLoopCheck = nullptr;
    QSpinBox *m_routeLoopIntervalSpin = nullptr;
    QLabel *m_routeStatusLabel = nullptr;
    QLabel *m_mapNameLabel = nullptr;
    QLabel *m_mousePositionLabel = nullptr;
    QPushButton *m_loadMapButton = nullptr;
    QPushButton *m_saveMapButton = nullptr;
    QPushButton *m_quickSaveButton = nullptr;
    QPushButton *m_editModeButton = nullptr;
    QPushButton *m_locateButton = nullptr;
    QShortcut *m_togglePathsShortcut = nullptr;
    QLabel *m_rowWorkStartLabel = nullptr;
    QLabel *m_rowWorkEndLabel = nullptr;
    QPushButton *m_rowWorkCaptureStartButton = nullptr;
    QPushButton *m_rowWorkCaptureEndButton = nullptr;
    QPushButton *m_rowWorkClearLineButton = nullptr;
    QPushButton *m_rowWorkAddCheckpointFromVehicleButton = nullptr;
    QPushButton *m_rowWorkAddCheckpointFromMapButton = nullptr;
    QPushButton *m_rowWorkDeleteCheckpointButton = nullptr;
    QPushButton *m_rowWorkClearCheckpointsButton = nullptr;
    QTableWidget *m_rowWorkCheckpointTable = nullptr;
    QDoubleSpinBox *m_rowWorkBaseSpeedSpin = nullptr;
    QDoubleSpinBox *m_rowWorkMaxSpeedSpin = nullptr;
    QDoubleSpinBox *m_rowWorkEndpointSlowdownSpin = nullptr;
    QDoubleSpinBox *m_rowWorkEndpointArrivalSpin = nullptr;
    QDoubleSpinBox *m_rowWorkCheckpointToleranceSpin = nullptr;
    QDoubleSpinBox *m_rowWorkTurnAngularSpeedSpin = nullptr;
    QCheckBox *m_rowWorkLoopCheck = nullptr;
    QPushButton *m_rowWorkReadPlanButton = nullptr;
    QPushButton *m_rowWorkUploadPlanButton = nullptr;
    QPushButton *m_rowWorkStartButton = nullptr;
    QPushButton *m_rowWorkPauseButton = nullptr;
    QPushButton *m_rowWorkResumeButton = nullptr;
    QPushButton *m_rowWorkStopButton = nullptr;
    QLabel *m_rowWorkPlanLabel = nullptr;
    QLabel *m_rowWorkRuntimeLabel = nullptr;
    QLabel *m_rowWorkProgressLabel = nullptr;
    QLabel *m_rowWorkTargetLabel = nullptr;
    QLabel *m_rowWorkControlLabel = nullptr;
    QLabel *m_rowWorkPoseLabel = nullptr;
    QLabel *m_rowWorkFaultLabel = nullptr;
    QLabel *m_rowWorkEventLabel = nullptr;
    QLabel *m_rowWorkStatusLabel = nullptr;

    QTabWidget *m_rowWorkModeTabWidget = nullptr;
    QWidget *m_rowWorkPrimitivePage = nullptr;
    QWidget *m_rowWorkMissionPage = nullptr;
    QLineEdit *m_rowMissionNameEdit = nullptr;
    QLabel *m_rowMissionIdLabel = nullptr;
    QLabel *m_rowMissionSummaryLabel = nullptr;
    QLabel *m_rowMissionStatusLabel = nullptr;
    QCheckBox *m_rowMissionLoopCheck = nullptr;
    QTableWidget *m_rowMissionStepTable = nullptr;
    QPushButton *m_rowMissionAddRowLegButton = nullptr;
    QPushButton *m_rowMissionAddTransferButton = nullptr;
    QPushButton *m_rowMissionAddTurnButton = nullptr;
    QPushButton *m_rowMissionAddWaitButton = nullptr;
    QPushButton *m_rowMissionRemoveStepButton = nullptr;
    QPushButton *m_rowMissionMoveUpButton = nullptr;
    QPushButton *m_rowMissionMoveDownButton = nullptr;
    QPushButton *m_rowMissionClearButton = nullptr;
    QPushButton *m_rowMissionImportRowWorkButton = nullptr;
    QComboBox *m_rowMissionStepTypeCombo = nullptr;
    QLineEdit *m_rowMissionStepNameEdit = nullptr;
    QLineEdit *m_rowMissionStepNoteEdit = nullptr;
    QDoubleSpinBox *m_rowMissionTargetYawSpin = nullptr;
    QSpinBox *m_rowMissionDwellSpin = nullptr;
    QLabel *m_rowMissionPrimitiveSummaryLabel = nullptr;
    QPushButton *m_rowMissionApplyStepButton = nullptr;

    QList<RouteStep> m_routeQueue;
    int m_activeRouteIndex = -1;
    QTimer *m_routeTimer = nullptr;
    bool m_waitingForClickPlacement = false;
    std::optional<int> m_pendingPointSelection;
    QString m_currentMapFilePath;
    QString m_lastLoadDirectory;
    QString m_lastSaveDirectory;
    QByteArray m_committedMapState;
    static QString s_lastMapFilePath;
    RowWorkPlan m_rowWorkPlan;
    RowMissionPlan m_rowMissionPlan;
    RowWorkStatus m_rowWorkStatus;
    bool m_hasRowWorkStatus = false;
    bool m_rowWorkStatusDirty = false;
    bool m_rowWorkCheckpointTableUpdating = false;
    bool m_rowWorkClickPlacementMode = false;
    RowWorkPendingCaptureTarget m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    bool m_rowWorkPendingStartAfterUpload = false;
    bool m_rowMissionStepTableUpdating = false;
    bool m_rowMissionEditorUpdating = false;
    int m_rowMissionSelectedStepRow = -1;

    QPushButton *m_newMapButton = nullptr;
};

#endif // MAP_H
