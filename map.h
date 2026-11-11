#ifndef MAP_H
#define MAP_H

#include "rowworktypes.h"
#include "taskcompiler.h"
#include <QPointer>
#include <QJsonObject>

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
class QAction;
class ControlSessionCoordinator;
class TrackingClient;
class PoseClient;

class MapGraphicsView;

namespace Ui {
class MainWindow;
}

class Map : public QObject
{
    Q_OBJECT
    friend class MapRouteIntegrationTest;

public:
    explicit Map(Ui::MainWindow *ui, QObject *parent = nullptr);
    void setControlCoordinator(ControlSessionCoordinator* coordinator);
    bool confirmClose();
signals:
    void externalActionResolved(const QString &eventId, bool success);

public slots:
    void handleModuleActivated();
    void updateVehiclePose(double x, double y, double theta);
    void applyRuntimeConfig();
    void setExternalActionReviewEvent(const QString &eventId);

public:
    enum class PathType {
        Line,
        Arc
    };

signals:

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
    void handleRouteResume();
    void handleLocateCurrentPosition();
    void handleEditModeToggled(bool checked);
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
    void handleRowWorkStart();
    void handleRowWorkLoopChanged(bool checked);
    void handleRowWorkCheckpointCellChanged(int row, int column);


private:
    void confirmFrameBinding();
    void bindNewMapToController();
    void refreshControllerProfileRevision();
    void refreshPlanningRevision();
    QByteArray m_planningFingerprint;
    void submitRouteTask();
    QList<TaskRouteSection> buildRouteSections(QString *error = nullptr);
    void requestTaskReview();
    bool hasConfirmedTask() const;
    void handleCheckpointReview();
    void openCheckpointReview();
    QPushButton *m_taskPauseButton = nullptr, *m_taskResumeButton = nullptr,
                *m_taskStopButton = nullptr, *m_taskResetButton = nullptr, *m_cancelStartButton = nullptr,
                *m_checkpointReviewButton = nullptr, *m_frameBindingButton = nullptr;
    QJsonObject m_taskReviewIdentity, m_checkpointReviewIdentity;
    bool m_taskReviewOpen = false, m_checkpointReviewOpen = false;
    quint64 m_checkpointReviewGeneration = 0;
    QString m_externalReviewEventId;
    void applyCapture(const QJsonObject& capture);
    void applyTrackingStatus(const TrackingSnapshot& status);
    void reviewRemoteTask(const QJsonObject &record);
    TaskCompileOptions taskOptions() const;
    QPointer<ControlSessionCoordinator> m_coordinator;
    QPointer<TrackingClient> m_tracking;
    QPointer<PoseClient> m_poseClient;
    MapFrameBinding m_binding;
    bool m_newMapMayAutoBind = true;
    void refreshTaskStatus();
    QLabel* m_trackingStatusLabel = nullptr;
    QLabel* m_taskProgressLabel = nullptr;
    QLabel* m_routeStartHintLabel = nullptr;
    QLabel* m_rowWorkStartHintLabel = nullptr;
    QLabel* m_routeQueueEmptyLabel = nullptr;
    struct MapPoint {
        int id = -1;
        QPointF mapPosition;
        double theta = 0.0;
        int dwellMs = 0;
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
        bool bidirectional = false;
        QList<QPointF> polyline;
        double sagitta = 0.0;
        QPointF arcCenter;
        double radius = 0.0;
        double sweepAngleRad = 0.0;
        QGraphicsPathItem *pathItem = nullptr;
    };

    struct RouteStep {
        int fromId = -1;
        int toId = -1;
        QList<int> pathIds;
    };

    enum class RowWorkPendingCaptureTarget {
        None,
        StartPose,
        EndPose,
        CheckpointFromVehicle,
        MapPoint
    };
    bool requestPoseCapture(RowWorkPendingCaptureTarget target);

    void initializeUi();
    void ensureScene();
    void rebuildGrid();
    void clearGrid();
    void ensureVehicleItem();

    int allocatePointId();
    int allocatePathId();
    void resetNextPointId();
    void resetNextPathId();

    bool addPointInternal(double x, double y, double theta, std::optional<int> forcedId = std::nullopt,
                          std::optional<int> dwellMs = std::nullopt);
    void removePointInternal(int pointId);
    bool addLinePath(int startId, int endId, std::optional<int> forcedId = std::nullopt, bool bidirectional = false);
    bool addArcPath(int startId, int endId, double sagitta, std::optional<int> forcedId = std::nullopt, bool bidirectional = false);
    bool addPathInternal(MapPath &&path);
    void removePathInternal(int pathId);
    void refreshPathGeometry(MapPath &path);
    int addPathOrWarn(int startId, int endId, PathType type, double sagitta = 0.0, std::optional<int> forcedId = std::nullopt, bool bidirectional = false);
    int findPathId(int startId, int endId) const;
    void updatePointGraphics(MapPoint &point);
    void refreshAllPointGraphics();
    void refreshVehicleGraphics();
    void appendLocalizationTrail(const ControlPoseSnapshot &pose);
    void refreshLocalizationTrailGraphics();
    void clearLocalizationTrail();
    void refreshPathsForPoint(int pointId);
    void autoSelectRoutePoints(int startId, int endId);

    QList<int> findRoutePathIds(int startId, int endId) const;
    QList<QPointF> composePolyline(const QList<int> &pathIds, int startId = -1) const;
    int findNearestPointId(const QPointF &mapPos, double thresholdMeters, double *outDistance = nullptr) const;
    void updatePointTableRow(int pointId);
    double angleFromMapVector(double dx, double dy) const;
    bool rebuildRouteStep(RouteStep &step);

    void refreshPointUi();
    void refreshPathUi();
    void refreshSelectors();
    void refreshRouteQueueUi();
    void refreshRouteGraphics();
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
    static QString mapsDirectory();
    QString suggestedMapFilePath(const QString &directory) const;
    bool ensureMapsDirectory();
    void resetToBlankMap();
    bool saveCurrentMapInteractive(bool forceChooseFile);
    bool confirmMapChanges(const QString &title);
    QByteArray buildComparableMapState() const;
    void syncCommittedMapState();
    bool hasUnsavedMapChanges() const;

    void clearMapData();
    bool saveMapToFile(const QString &filePath);
    bool loadMapFromFile(const QString &filePath);
    QJsonObject serializeMap() const;
    bool deserializeMap(const QJsonObject &object);
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
    bool addRowWorkCheckpoint(const QPointF &mapPos);
    void sortRowWorkCheckpoints();
    QString nextCheckpointName() const;
    bool checkpointFromRow(int row, RowCheckpoint *outCheckpoint, QString *errorMessage) const;
    void updateCheckpointRowNames();
    bool canEditRowWorkPlan() const;
    bool submitRowWorkTask();

    Ui::MainWindow *ui;

    QWidget *m_mapPage = nullptr;
    MapGraphicsView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;
    QGraphicsItemGroup *m_gridGroup = nullptr;
    QGraphicsEllipseItem *m_originItem = nullptr;

    double m_cellSizeMeters = 1.0;
    double m_cellSizePixels = 50.0;
    double m_mapRotationDeg = 0.0;
    bool m_initialized = false;
    bool m_pathsVisible = true;
    int m_gridWidth = 0;
    int m_gridHeight = 0;

    int m_nextPointId = 1;
    int m_nextPathId = 1;

    QHash<int, MapPoint> m_points;
    QHash<int, MapPath> m_paths;

    QGraphicsItemGroup *m_vehicleItem = nullptr;
    QGraphicsRectItem *m_vehicleBody = nullptr;
    QGraphicsItemGroup *m_vehiclePoseMarker = nullptr;
    QGraphicsEllipseItem *m_vehiclePoseCircle = nullptr;
    QGraphicsPathItem *m_vehiclePoseArrow = nullptr;
    QGraphicsPathItem *m_localizationTrailItem = nullptr;
    QGraphicsPathItem *m_routePlanItem = nullptr;
    struct LocalizationTrailSample {
        QPointF enuPosition;
        double yaw = 0.0;
        bool startsSegment = true;
    };
    static constexpr qsizetype kMaxLocalizationTrailSamples = 6000;
    QList<LocalizationTrailSample> m_localizationTrail;
    QByteArray m_localizationTrailFrame;
    QString m_localizationTrailBoot;
    quint64 m_localizationTrailSequence = 0;
    bool m_localizationTrailStartsSegment = true;
    QElapsedTimer m_localizationTrailAge;
    QTimer *m_localizationTrailRefreshTimer = nullptr;
    QGraphicsPathItem *m_rowWorkLineItem = nullptr;
    QGraphicsEllipseItem *m_rowWorkStartMarker = nullptr;
    QGraphicsEllipseItem *m_rowWorkEndMarker = nullptr;
    QList<QGraphicsItemGroup *> m_rowWorkCheckpointMarkers;

    bool m_hasVehiclePose = false;
    double m_vehiclePoseX = 0.0;
    double m_vehiclePoseY = 0.0;
    double m_vehiclePoseTheta = 0.0;
    bool m_editModeEnabled = false;
    int m_draggedPointId = -1;
    bool m_draggedPointMoved = false;
    bool m_draggedPointRotated = false;
    bool m_altRotationActive = false;
    double m_rotationStartTheta = 0.0;
    double m_rotationReferenceAngle = 0.0;
    bool m_ctrlDragActive = false;
    QPointF m_ctrlDragOffset;
    bool m_ctrlAltPathActive = false;
    std::optional<int> m_ctrlAltStartPointId;

    // Left panel controls
    QSpinBox *m_gridWidthSpin = nullptr;
    QSpinBox *m_gridHeightSpin = nullptr;
    QDoubleSpinBox *m_gridRotationSpin = nullptr;
    QPushButton *m_buildGridButton = nullptr;
    QSlider *m_rotationSlider = nullptr;

    QDoubleSpinBox *m_pointXSpin = nullptr;
    QDoubleSpinBox *m_pointYSpin = nullptr;
    QDoubleSpinBox *m_pointThetaSpin = nullptr;
    QSpinBox *m_pointDwellSpin = nullptr;
    QCheckBox *m_autoConnectPointsCheck = nullptr;
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
    QComboBox *m_pathDirectionCombo = nullptr;
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
    QCheckBox *m_routeLoopCheck = nullptr;
    QSpinBox *m_routeLoopIntervalSpin = nullptr;
    QLabel *m_routeStatusLabel = nullptr;
    enum class TaskNoticeSource { Other, PoseStream, CaptureInterrupted };
    TaskNoticeSource m_taskNoticeSource = TaskNoticeSource::Other;
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
    QCheckBox *m_rowWorkLoopCheck = nullptr;
    QPushButton *m_rowWorkStartButton = nullptr;
    QLabel *m_rowWorkPlanLabel = nullptr;

    QWidget *m_rowWorkPrimitivePage = nullptr;

    QList<RouteStep> m_routeQueue;
    bool m_waitingForClickPlacement = false;
    std::optional<int> m_pendingPointSelection;
    QString m_currentMapFilePath;
    QString m_lastLoadDirectory;
    QString m_lastSaveDirectory;
    QByteArray m_committedMapState;
    static QString s_lastMapFilePath;
    RowWorkPlan m_rowWorkPlan;
    QJsonObject m_legacyRowMissionDocument;
    bool m_hasLegacyRowMissionDocument = false;
    bool m_rowWorkCheckpointTableUpdating = false;
    bool m_rowWorkClickPlacementMode = false;
    RowWorkPendingCaptureTarget m_rowWorkPendingCaptureTarget = RowWorkPendingCaptureTarget::None;
    QJsonObject m_pendingCaptureBinding;
    int m_pendingMapPointDwellMs = 0;

    QPushButton *m_newMapButton = nullptr;
};

#endif // MAP_H
