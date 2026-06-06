import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCharts 
import QtQuick.VirtualKeyboard

ApplicationWindow {
    id: mainWindow
    visible: true
    visibility: ApplicationWindow.FullScreen
    width: 1280
    height: 800
    title: "BVM Emergency Ventilator"
    color: "#121212" 

    // --- Floating Exit Button ---
    Button {
        text: "✕"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        width: 35
        height: 35
        z: 999  // Ensure it sits above the charts and panels!
        background: Rectangle { color: "#D32F2F"; radius: 5; border.color: "#B71C1C"; border.width: 2 }
        contentItem: Text { text: parent.text; color: "white"; font.bold: true; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        onClicked: VentCore.exitApp()
    }

    // --- Dynamic Settings Pop-up ---
    Popup {
        id: editPopup
        anchors.centerIn: parent
        width: 400
        height: 250
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        
        background: Rectangle {
            color: "#2a2a2a"
            radius: 12
            border.color: "#444"
            border.width: 2
        }

        property string activeSetting: ""
        property int tempValue: 0
        property string tempString: ""

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 20

            Text {
                text: "Edit " + editPopup.activeSetting
                color: "white"
                font.pixelSize: 24
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            StackLayout {
                id: popupStack
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Index 0: Mode Selection
                ComboBox { 
                    model: ["VCV", "PCV"]
                    currentIndex: find(editPopup.tempString)
                    onActivated: editPopup.tempString = currentText 
                }

                // Index 1: RR Slider
                ColumnLayout { 
                    Text { 
                        text: editPopup.tempValue + " BPM"
                        color: "#00ffcc"
                        font.pixelSize: 24
                        Layout.alignment: Qt.AlignHCenter 
                    }
                    Slider { 
                        from: 10; to: 40; stepSize: 1
                        value: editPopup.tempValue
                        onMoved: editPopup.tempValue = value
                        Layout.fillWidth: true 
                    } 
                }

                // Index 2: Tidal Volume Slider
                ColumnLayout { 
                    Text { 
                        text: editPopup.tempValue + " mL"
                        color: "#00ffcc"
                        font.pixelSize: 24
                        Layout.alignment: Qt.AlignHCenter 
                    }
                    Slider { 
                        from: 200; to: 800; stepSize: 10
                        value: editPopup.tempValue
                        onMoved: editPopup.tempValue = value
                        Layout.fillWidth: true 
                    } 
                }

                // Index 3: I:E Ratio Selection
                ComboBox { 
                    model: ["1:1", "1:2", "1:3", "1:4"]
                    currentIndex: find(editPopup.tempString)
                    onActivated: editPopup.tempString = currentText 
                }

                // Index 4: Target PIP Slider
                ColumnLayout { 
                    Text { 
                        text: editPopup.tempValue + " cmH2O"
                        color: "#00ffcc"
                        font.pixelSize: 24
                        Layout.alignment: Qt.AlignHCenter 
                    }
                    Slider { 
                        from: 10; to: 40; stepSize: 1
                        value: editPopup.tempValue
                        onMoved: editPopup.tempValue = value
                        Layout.fillWidth: true 
                    } 
                }
            }

            Button {
                text: "SAVE & SEND"
                Layout.fillWidth: true
                Layout.preferredHeight: 50
                font.pixelSize: 18
                font.bold: true
                onClicked: {
                    if (popupStack.currentIndex === 0) VentCore.mode = editPopup.tempString
                    if (popupStack.currentIndex === 1) VentCore.rr = editPopup.tempValue
                    if (popupStack.currentIndex === 2) VentCore.tidal_volume = editPopup.tempValue
                    if (popupStack.currentIndex === 3) VentCore.ie_ratio = editPopup.tempString
                    if (popupStack.currentIndex === 4) VentCore.target_pip = editPopup.tempValue
                    editPopup.close()
                }
            }
        }
    }

    // --- Main Layout ---
    GridLayout {
        anchors.fill: parent
        anchors.margins: 10
        columns: 2
        columnSpacing: 10

        // --- Left Panel: Graphs & ML Status ---
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#1e1e1e"
            radius: 10

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 5

                ChartView {
                    title: "Pressure (cmH2O)"
                    titleColor: "white"; backgroundColor: "transparent"; legend.visible: false; antialiasing: true
                    Layout.fillWidth: true; Layout.fillHeight: true; margins { top: 0; bottom: 0; left: 35; right: 15 }
                    ValueAxis { id: axisX_P; min: 0; max: 15; color: "#444"; labelsColor: "white"; tickCount: 6; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    
                    // FIX: Changed min to -10 so the zero-line is clearly visible!
                    ValueAxis { id: axisY_P; min: -10; max: 40; color: "#444"; labelsColor: "white"; tickCount: 6; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    
                    LineSeries { id: pressureSeries; axisX: axisX_P; axisY: axisY_P; color: "#00ffcc"; width: 3 }
                }

                ChartView {
                    title: "Flow (L/min)"
                    titleColor: "white"; backgroundColor: "transparent"; legend.visible: false; antialiasing: true
                    Layout.fillWidth: true; Layout.fillHeight: true; margins { top: 0; bottom: 0; left: 35; right: 15 }
                    ValueAxis { id: axisX_F; min: 0; max: 15; color: "#444"; labelsColor: "white"; tickCount: 6; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    ValueAxis { id: axisY_F; min: -60; max: 60; color: "#444"; labelsColor: "white"; tickCount: 5; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    LineSeries { id: flowSeries; axisX: axisX_F; axisY: axisY_F; color: "#ffcc00"; width: 3; visible: true }
                    LineSeries { id: calcFlowSeries; axisX: axisX_F; axisY: axisY_F; color: "#00ccff"; width: 3; visible: true }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 20
                    CheckBox {
                        text: "Physical Flow"
                        checked: true
                        onCheckedChanged: flowSeries.visible = checked
                        contentItem: Text { text: parent.text; color: "#ffcc00"; font.pixelSize: 14; leftPadding: parent.indicator.width + parent.spacing; verticalAlignment: Text.AlignVCenter }
                    }
                    CheckBox {
                        text: "Calculated Flow"
                        checked: true
                        onCheckedChanged: calcFlowSeries.visible = checked
                        contentItem: Text { text: parent.text; color: "#00ccff"; font.pixelSize: 14; leftPadding: parent.indicator.width + parent.spacing; verticalAlignment: Text.AlignVCenter }
                    }
                }

                ChartView {
                    title: "Volume (mL)"
                    titleColor: "white"; backgroundColor: "transparent"; legend.visible: false; antialiasing: true
                    Layout.fillWidth: true; Layout.fillHeight: true; margins { top: 0; bottom: 0; left: 35; right: 15 }
                    ValueAxis { id: axisX_V; min: 0; max: 15; color: "#444"; labelsColor: "white"; tickCount: 6; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    
                    // FIX: Changed min to -50 so the zero-line is clearly visible!
                    ValueAxis { id: axisY_V; min: -50; max: 800; color: "#444"; labelsColor: "white"; tickCount: 5; labelFormat: "%.0f"; labelsFont.pixelSize: 12 }
                    
                    LineSeries { id: volumeSeries; axisX: axisX_V; axisY: axisY_V; color: "#ff00ff"; width: 3 }
                }

                Text {
                    id: mlStatus
                    text: "ML Analysis: Standby"
                    color: "#ffcc00"
                    font.pixelSize: 16
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 10
                }
            }
        }

        // --- Right Panel: Settings Controls ---
        Rectangle {
            Layout.preferredWidth: 250
            Layout.fillHeight: true
            color: "#1e1e1e"
            radius: 10

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 15

                Text { text: "CONTROLS"; color: "white"; font.bold: true; font.pixelSize: 18; Layout.alignment: Qt.AlignHCenter }

                RowLayout {
                    Layout.fillWidth: true
                    Button { 
                        text: "START"; Layout.fillWidth: true; background: Rectangle { color: "#2E7D32"; radius: 5 }
                        contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        onClicked: VentCore.startVentilation() 
                    }
                    Button { 
                        text: "STOP"; Layout.fillWidth: true; background: Rectangle { color: "#F57C00"; radius: 5 }
                        contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        onClicked: VentCore.stopVentilation() 
                    }
                }
                
                Button { 
                    text: "EMERGENCY STOP"; Layout.fillWidth: true; background: Rectangle { color: "#D32F2F"; radius: 5 }
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter }
                    onClicked: VentCore.emergencyStop() 
                }

                Button { 
                    text: "HOME CALIBRATE"; Layout.fillWidth: true; background: Rectangle { color: "#1565C0"; radius: 5 }
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    onClicked: VentCore.calibrateHome() 
                }
                
                // --- REBOOT BUTTON ---
                Button { 
                    text: "REBOOT SYSTEM"; Layout.fillWidth: true; background: Rectangle { color: "#8E24AA"; radius: 5 } // Purple color
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    onClicked: VentCore.rebootSystem() 
                }

                Button {
                    text: "DATA LOGGING SETTINGS"
                    Layout.fillWidth: true
                    background: Rectangle { color: "#555"; radius: 5 }
                    contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                    onClicked: loggingDialog.open()
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: "#444"; Layout.topMargin: 5; Layout.bottomMargin: 5 }

                Text { text: "SETTINGS"; color: "white"; font.bold: true; font.pixelSize: 18; Layout.alignment: Qt.AlignHCenter }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    ColumnLayout {
                        width: parent.width - 15 // Leave room for scrollbar
                        spacing: 10

                        component SettingRow : Rectangle {
                            id: rootRow 
                            property string label
                            property string val
                            property int stackIndex
                            
                            Layout.fillWidth: true
                            height: 50 // Slimmer profile
                            color: "#2a2a2a"
                            radius: 8
                            border.color: mouseArea.pressed ? "#00ffcc" : "#444"
                            
                            ColumnLayout {
                                anchors.centerIn: parent
                                Text { text: rootRow.label; color: "gray"; font.pixelSize: 12; Layout.alignment: Qt.AlignHCenter }
                                Text { text: rootRow.val; color: "white"; font.pixelSize: 16; font.bold: true; Layout.alignment: Qt.AlignHCenter }
                            }

                            MouseArea {
                                id: mouseArea
                                anchors.fill: parent
                                onClicked: {
                                    editPopup.activeSetting = rootRow.label
                                    popupStack.currentIndex = rootRow.stackIndex
                                    if (rootRow.stackIndex === 0) editPopup.tempString = VentCore.mode
                                    if (rootRow.stackIndex === 1) editPopup.tempValue = VentCore.rr
                                    if (rootRow.stackIndex === 2) editPopup.tempValue = VentCore.tidal_volume
                                    if (rootRow.stackIndex === 3) editPopup.tempString = VentCore.ie_ratio
                                    if (rootRow.stackIndex === 4) editPopup.tempValue = VentCore.target_pip
                                    editPopup.open()
                                }
                            }
                        }

                        SettingRow { label: "Mode"; val: VentCore.mode; stackIndex: 0 }
                        SettingRow { label: "Resp Rate"; val: VentCore.rr + " BPM"; stackIndex: 1 }
                        SettingRow { label: "Tidal Vol"; val: VentCore.tidal_volume + " mL"; stackIndex: 2 }
                        SettingRow { label: "I:E Ratio"; val: VentCore.ie_ratio; stackIndex: 3 }
                        SettingRow { label: "Target PIP"; val: VentCore.target_pip + " cmH2O"; stackIndex: 4 }
                    }
                }
            }
        }
    }

    Dialog {
        id: loggingDialog
        title: "Data Logging Settings"
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        modal: true
        standardButtons: Dialog.NoButton
        
        background: Rectangle {
            color: "#1e1e1e"
            radius: 10
            border.color: "#444"
            border.width: 1
        }

        ColumnLayout {
            spacing: 15
            
            CheckBox {
                id: enableLoggingCheck
                text: "Save Ventilation Data"
                checked: VentCore.is_logging
                contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 16; leftPadding: parent.indicator.width + parent.spacing; verticalAlignment: Text.AlignVCenter }
            }
            
            RowLayout {
                Text { text: "File Name:"; color: "gray"; font.pixelSize: 14 }
                TextField {
                    id: logFilenameField
                    text: VentCore.log_filename
                    color: "white"
                    background: Rectangle { color: "#333"; radius: 5 }
                    Layout.fillWidth: true
                }
            }
            
            RowLayout {
                Text { text: "Max Rows Limit:"; color: "gray"; font.pixelSize: 14 }
                SpinBox {
                    id: logLimitSpin
                    value: VentCore.log_limit
                    from: 1000
                    to: 1000000
                    stepSize: 100
                }
            }
        }
    }

    Connections {
        target: VentCore
        function onTelemetry_updated(time, pressure, volume, flow, calc_flow) {
            pressureSeries.append(time, pressure)
            volumeSeries.append(time, volume)
            flowSeries.append(time, flow)
            calcFlowSeries.append(time, calc_flow)

            if (time > 15) {
                axisX_P.min = time - 15; axisX_P.max = time;
                axisX_V.min = time - 15; axisX_V.max = time;
                axisX_F.min = time - 15; axisX_F.max = time;
                
                if (pressureSeries.count > 300) {
                    pressureSeries.remove(0)
                    volumeSeries.remove(0)
                    flowSeries.remove(0)
                    calcFlowSeries.remove(0)
                }
            }
        }
        function onPatient_state_updated(label, color, conf, p_norm, p_obs, p_rest) {
            mlStatus.text = "ML Analysis: " + label
            mlStatus.color = color
        }
    }
}