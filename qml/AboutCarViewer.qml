import QtQuick
import QtQuick3D
import QtQuick3D.AssetUtils

Rectangle {
    id: root
    color: "#f4f7f1"

    property real yawAngle: 32
    property real pitchAngle: -12
    property real rollAngle: 0
    property real zoomDistance: 5.4
    property real lastX: 0
    property real lastY: 0
    property bool dragging: false
    property int dragButton: Qt.NoButton
    property url aboutCarModelSource: ""
    property url aboutCarFallbackImage: "qrc:/image/car.jpg"

    View3D {
        id: view3d
        anchors.fill: parent
        camera: camera
        opacity: vehicleModel.status === RuntimeLoader.Error ? 0 : 1
        environment: SceneEnvironment {
            clearColor: "#f4f7f1"
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        PerspectiveCamera {
            id: camera
            position: Qt.vector3d(0, 0.36, root.zoomDistance)
            eulerRotation.x: -6
            fieldOfView: 28
            clipNear: 0.01
            clipFar: 120
        }

        DirectionalLight {
            eulerRotation.x: -38
            eulerRotation.y: 34
            brightness: 4.5
            castsShadow: false
        }

        DirectionalLight {
            eulerRotation.x: 28
            eulerRotation.y: -145
            brightness: 2.4
            castsShadow: false
        }

        PointLight {
            position: Qt.vector3d(0.0, 1.8, 5.5)
            brightness: 6.5
            castsShadow: false
        }

        Node {
            id: modelRoot
            eulerRotation.x: root.pitchAngle
            eulerRotation.y: root.yawAngle
            eulerRotation.z: root.rollAngle

            RuntimeLoader {
                id: vehicleModel
                source: root.aboutCarModelSource
                scale: Qt.vector3d(1, 1, 1)

                onStatusChanged: {
                    if (status === RuntimeLoader.Success) {
                        console.log("About 3D model loaded:", source)
                    } else if (status === RuntimeLoader.Error) {
                        console.warn("About 3D model load failed:", source, errorString)
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: "transparent"
        visible: vehicleModel.status === RuntimeLoader.Error

        Image {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.92, 360)
            height: Math.min(parent.height * 0.82, 220)
            source: root.aboutCarFallbackImage
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            radius: 8
            color: "#ddfbfcf8"

            Text {
                anchors.centerIn: parent
                width: parent.width - 16
                text: vehicleModel.errorString.length > 0 ? "三维模型暂不可用，已回退车辆图片" : "三维模型暂不可用"
                color: "#51655a"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }
    }

    NumberAnimation {
        id: autoRotate
        target: root
        property: "yawAngle"
        from: root.yawAngle
        to: root.yawAngle + 360
        duration: 26000
        loops: Animation.Infinite
        running: !root.dragging
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        onPressed: function(mouse) {
            root.dragging = true
            root.dragButton = mouse.button
            root.lastX = mouse.x
            root.lastY = mouse.y
            autoRotate.stop()
        }

        onPositionChanged: function(mouse) {
            if (!(mouse.buttons & Qt.LeftButton)) {
                return
            }

            var dx = mouse.x - root.lastX
            var dy = mouse.y - root.lastY
            root.lastX = mouse.x
            root.lastY = mouse.y

            if (root.dragButton === Qt.RightButton || (mouse.modifiers & Qt.ShiftModifier)) {
                root.rollAngle += dx * 0.55
                root.pitchAngle = Math.max(-179, Math.min(179, root.pitchAngle + dy * 0.42))
                return
            }

            root.yawAngle += dx * 0.6
            root.pitchAngle = Math.max(-179, Math.min(179, root.pitchAngle + dy * 0.52))
        }

        onReleased: function() {
            root.dragging = false
            root.dragButton = Qt.NoButton
            autoRotate.from = root.yawAngle
            autoRotate.to = root.yawAngle + 360
            autoRotate.restart()
        }

        onWheel: function(wheel) {
            root.zoomDistance = Math.max(2.8, Math.min(9.5, root.zoomDistance - wheel.angleDelta.y / 260))
            wheel.accepted = true
        }
    }
}
