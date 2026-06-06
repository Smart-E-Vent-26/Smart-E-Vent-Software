import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Effects

ApplicationWindow {
    visible: true
    width: 1400
    height: 900
    title: "BVM Ventilator - Patient State Display"
    color: "#0a0e27"
    
    // Status signals from Python
    Connections {
        target: patientState
        function onStateChanged(state) {
            console.log("State changed to:", state)
        }
        function onDiseaseTypeChanged(disease) {
            console.log("Disease type changed to:", disease)
        }
        function onConfidenceChanged(confidence) {
            console.log("Confidence changed to:", confidence)
        }
        function onStatusChanged(message) {
            console.log("Status:", message)
        }
    }
    
    // Main layout
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16
        
        // ====================================================================
        // HEADER
        // ====================================================================
        Rectangle {
            Layout.fillWidth: true
            height: 70
            color: "#1a1f3a"
            radius: 12
            border.color: "#00d4ff"
            border.width: 2
            
            LinearGradient {
                anchors.fill: parent
                start: Qt.point(0, 0)
                end: Qt.point(width, 0)
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#1a1f3a" }
                    GradientStop { position: 1.0; color: "#2d3557" }
                }
                radius: 12
            }
            
            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 20
                
                Column {
                    spacing: 4
                    
                    Text {
                        text: "BVM Ventilator System"
                        font.pixelSize: 22
                        font.bold: true
                        color: "#00d4ff"
                    }
                    
                    Text {
                        text: "ML-Based Patient State Classification"
                        font.pixelSize: 12
                        color: "#88ccff"
                    }
                }
                
                Item { Layout.fillWidth: true }
                
                Column {
                    spacing: 4
                    
                    Text {
                        text: Qt.formatDateTime(new Date(), "yyyy-MM-dd")
                        font.pixelSize: 13
                        color: "#aabbcc"
                        horizontalAlignment: Text.AlignRight
                    }
                    
                    Text {
                        text: Qt.formatDateTime(new Date(), "hh:mm:ss")
                        font.pixelSize: 13
                        color: "#aabbcc"
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }
        
        // ====================================================================
        // MAIN CONTENT
        // ====================================================================
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16
            
            // ================================================================
            // LEFT PANEL - DIAGNOSIS RESULTS
            // ================================================================
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: parent.width * 0.5
                color: "#0f1219"
                radius: 12
                border.color: "#00d4ff"
                border.width: 1
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Panel title
                    Text {
                        text: "📊 Classification Results"
                        font.pixelSize: 18
                        font.bold: true
                        color: "#00d4ff"
                    }
                    
                    // ============================================================
                    // DISEASE TYPE CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        height: 120
                        color: getDiseaseBackgroundColor(patientState.current_disease)
                        radius: 8
                        border.color: getDiseaseColor(patientState.current_disease)
                        border.width: 2
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 8
                            
                            Text {
                                text: "Disease Classification"
                                font.pixelSize: 11
                                color: "#888888"
                                font.capitalization: Font.CapitalizeEachWord
                            }
                            
                            Text {
                                text: patientState.current_disease || "Unknown"
                                font.pixelSize: 36
                                font.bold: true
                                font.weight: Font.ExtraBold
                                color: getDiseaseColor(patientState.current_disease)
                                lineHeight: 1.0
                            }
                        }
                    }
                    
                    // ============================================================
                    // CONFIDENCE SCORE CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        height: 100
                        color: "#1a1f2e"
                        radius: 8
                        border.color: "#00d4ff"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 8
                            
                            RowLayout {
                                spacing: 12
                                
                                Column {
                                    spacing: 4
                                    
                                    Text {
                                        text: "Confidence Score"
                                        font.pixelSize: 11
                                        color: "#888888"
                                    }
                                    
                                    Text {
                                        text: (patientState.current_confidence * 100).toFixed(1) + "%"
                                        font.pixelSize: 32
                                        font.bold: true
                                        color: getConfidenceColor(patientState.current_confidence)
                                    }
                                }
                                
                                Item { Layout.fillWidth: true }
                                
                                // Confidence gauge
                                Canvas {
                                    id: confidenceGauge
                                    width: 100
                                    height: 80
                                    
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        var centerX = width / 2
                                        var centerY = height / 2
                                        var radius = 35
                                        var angle = Math.PI + (patientState.current_confidence * Math.PI)
                                        
                                        // Background arc
                                        ctx.strokeStyle = "#333333"
                                        ctx.lineWidth = 6
                                        ctx.beginPath()
                                        ctx.arc(centerX, centerY, radius, Math.PI, 2 * Math.PI)
                                        ctx.stroke()
                                        
                                        // Confidence arc
                                        ctx.strokeStyle = getConfidenceColor(patientState.current_confidence)
                                        ctx.lineWidth = 6
                                        ctx.beginPath()
                                        ctx.arc(centerX, centerY, radius, Math.PI, angle)
                                        ctx.stroke()
                                    }
                                    
                                    Connections {
                                        target: patientState
                                        function onConfidenceChanged() {
                                            confidenceGauge.requestPaint()
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    // ============================================================
                    // STATE STATUS CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        height: 70
                        color: "#1a1f2e"
                        radius: 8
                        border.color: getStateColor(patientState.current_state)
                        border.width: 2
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12
                            
                            // Status indicator
                            Rectangle {
                                width: 16
                                height: 16
                                radius: 8
                                color: getStateColor(patientState.current_state)
                                
                                SequentialAnimation on opacity {
                                    running: patientState.current_state === "CLASSIFYING"
                                    loops: Animation.Infinite
                                    
                                    PropertyAnimation { to: 0.3; duration: 500 }
                                    PropertyAnimation { to: 1.0; duration: 500 }
                                }
                            }
                            
                            Column {
                                spacing: 2
                                
                                Text {
                                    text: "Current State"
                                    font.pixelSize: 11
                                    color: "#888888"
                                }
                                
                                Text {
                                    text: patientState.current_state || "STANDBY"
                                    font.pixelSize: 16
                                    font.bold: true
                                    color: getStateColor(patientState.current_state)
                                }
                            }
                            
                            Item { Layout.fillWidth: true }
                            
                            Text {
                                text: patientState.classification_time > 0 ? 
                                    (patientState.classification_time * 1000).toFixed(2) + " ms" : 
                                    "—"
                                font.pixelSize: 12
                                color: "#666666"
                            }
                        }
                    }
                    
                    // ============================================================
                    // PROBABILITIES CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#1a1f2e"
                        radius: 8
                        border.color: "#00d4ff"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12
                            
                            Text {
                                text: "Disease Probabilities"
                                font.pixelSize: 12
                                color: "#00d4ff"
                                font.bold: true
                            }
                            
                            // Normal probability
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                
                                RowLayout {
                                    spacing: 12
                                    
                                    Text {
                                        text: "Normal"
                                        font.pixelSize: 11
                                        color: "#aabbcc"
                                        Layout.preferredWidth: 80
                                    }
                                    
                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: 20
                                        color: "#0a0e1a"
                                        radius: 4
                                        border.color: "#00ff00"
                                        border.width: 1
                                        
                                        Rectangle {
                                            height: parent.height
                                            width: parent.width * (patientState.current_probabilities["Normal"] || 0)
                                            color: "#00ff00"
                                            radius: 4
                                            
                                            Behavior on width {
                                                PropertyAnimation { duration: 300; easing.type: Easing.OutCubic }
                                            }
                                        }
                                    }
                                    
                                    Text {
                                        text: ((patientState.current_probabilities["Normal"] || 0) * 100).toFixed(1) + "%"
                                        font.pixelSize: 11
                                        color: "#ffffff"
                                        Layout.preferredWidth: 50
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }
                            }
                            
                            // Obstructive probability
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                
                                RowLayout {
                                    spacing: 12
                                    
                                    Text {
                                        text: "Obstructive"
                                        font.pixelSize: 11
                                        color: "#aabbcc"
                                        Layout.preferredWidth: 80
                                    }
                                    
                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: 20
                                        color: "#0a0e1a"
                                        radius: 4
                                        border.color: "#ffaa00"
                                        border.width: 1
                                        
                                        Rectangle {
                                            height: parent.height
                                            width: parent.width * (patientState.current_probabilities["Obstructive"] || 0)
                                            color: "#ffaa00"
                                            radius: 4
                                            
                                            Behavior on width {
                                                PropertyAnimation { duration: 300; easing.type: Easing.OutCubic }
                                            }
                                        }
                                    }
                                    
                                    Text {
                                        text: ((patientState.current_probabilities["Obstructive"] || 0) * 100).toFixed(1) + "%"
                                        font.pixelSize: 11
                                        color: "#ffffff"
                                        Layout.preferredWidth: 50
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }
                            }
                            
                            // Restrictive probability
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                
                                RowLayout {
                                    spacing: 12
                                    
                                    Text {
                                        text: "Restrictive"
                                        font.pixelSize: 11
                                        color: "#aabbcc"
                                        Layout.preferredWidth: 80
                                    }
                                    
                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: 20
                                        color: "#0a0e1a"
                                        radius: 4
                                        border.color: "#ff3333"
                                        border.width: 1
                                        
                                        Rectangle {
                                            height: parent.height
                                            width: parent.width * (patientState.current_probabilities["Restrictive"] || 0)
                                            color: "#ff3333"
                                            radius: 4
                                            
                                            Behavior on width {
                                                PropertyAnimation { duration: 300; easing.type: Easing.OutCubic }
                                            }
                                        }
                                    }
                                    
                                    Text {
                                        text: ((patientState.current_probabilities["Restrictive"] || 0) * 100).toFixed(1) + "%"
                                        font.pixelSize: 11
                                        color: "#ffffff"
                                        Layout.preferredWidth: 50
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }
                            }
                            
                            Item { Layout.fillHeight: true }
                        }
                    }
                }
            }
            
            // ================================================================
            // RIGHT PANEL - STATUS & DETAILS
            // ================================================================
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: parent.width * 0.5
                color: "#0f1219"
                radius: 12
                border.color: "#00d4ff"
                border.width: 1
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Panel title
                    Text {
                        text: "ℹ️ Status & Information"
                        font.pixelSize: 18
                        font.bold: true
                        color: "#00d4ff"
                    }
                    
                    // ============================================================
                    // STATUS MESSAGE CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        height: 70
                        color: "#1a1f2e"
                        radius: 8
                        border.color: "#00d4ff"
                        border.width: 1
                        
                        Text {
                            anchors.fill: parent
                            anchors.margins: 16
                            text: patientState.statusChanged ? patientState.statusChanged : "Ready for classification"
                            font.pixelSize: 14
                            color: "#00ff00"
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    
                    // ============================================================
                    // METRICS CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        height: 120
                        color: "#1a1f2e"
                        radius: 8
                        border.color: "#00d4ff"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12
                            
                            Text {
                                text: "Classification Metrics"
                                font.pixelSize: 12
                                color: "#00d4ff"
                                font.bold: true
                            }
                            
                            GridLayout {
                                columns: 2
                                columnSpacing: 20
                                rowSpacing: 10
                                Layout.fillWidth: true
                                
                                // Timestamp
                                Column {
                                    spacing: 4
                                    
                                    Text {
                                        text: "Last Updated"
                                        font.pixelSize: 10
                                        color: "#666666"
                                    }
                                    
                                    Text {
                                        text: Qt.formatDateTime(new Date(), "hh:mm:ss")
                                        font.pixelSize: 12
                                        color: "#aabbcc"
                                    }
                                }
                                
                                // Classification Time
                                Column {
                                    spacing: 4
                                    
                                    Text {
                                        text: "Classification Time"
                                        font.pixelSize: 10
                                        color: "#666666"
                                    }
                                    
                                    Text {
                                        text: patientState.classification_time > 0 ? 
                                            (patientState.classification_time * 1000).toFixed(2) + " ms" : 
                                            "—"
                                        font.pixelSize: 12
                                        color: "#aabbcc"
                                    }
                                }
                                
                                // Features Count
                                Column {
                                    spacing: 4
                                    
                                    Text {
                                        text: "Features Used"
                                        font.pixelSize: 10
                                        color: "#666666"
                                    }
                                    
                                    Text {
                                        text: "70+"
                                        font.pixelSize: 12
                                        color: "#aabbcc"
                                    }
                                }
                                
                                // Model
                                Column {
                                    spacing: 4
                                    
                                    Text {
                                        text: "ML Model"
                                        font.pixelSize: 10
                                        color: "#666666"
                                    }
                                    
                                    Text {
                                        text: "Random Forest"
                                        font.pixelSize: 12
                                        color: "#aabbcc"
                                    }
                                }
                            }
                        }
                    }
                    
                    // ============================================================
                    // DETAILED REPORT CARD
                    // ============================================================
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#1a1f2e"
                        radius: 8
                        border.color: "#00d4ff"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12
                            
                            Text {
                                text: "Detailed Report"
                                font.pixelSize: 12
                                color: "#00d4ff"
                                font.bold: true
                            }
                            
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                
                                TextEdit {
                                    text: formatDetailedReport()
                                    font.pixelSize: 10
                                    font.family: "Courier"
                                    color: "#00ff00"
                                    readOnly: true
                                    wrapMode: TextEdit.Wrap
                                    selectByMouse: true
                                    
                                    background: Rectangle {
                                        color: "#0a0e1a"
                                        border.color: "#333333"
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                            }
                        }
                    }
                    
                    // ============================================================
                    // ACTION BUTTONS
                    // ============================================================
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        
                        Button {
                            Layout.preferredWidth: 120
                            text: "Classify"
                            enabled: patientState.current_state !== "CLASSIFYING"
                            
                            background: Rectangle {
                                color: parent.enabled ? "#00d4ff" : "#333333"
                                radius: 6
                                border.color: "#00d4ff"
                                border.width: 1
                            }
                            
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? "#000000" : "#666666"
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        
                        Button {
                            Layout.preferredWidth: 120
                            text: "Reset"
                            
                            background: Rectangle {
                                color: "#1a1f2e"
                                radius: 6
                                border.color: "#ff6600"
                                border.width: 1
                            }
                            
                            contentItem: Text {
                                text: parent.text
                                color: "#ff6600"
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            
                            onClicked: {
                                patientState.updateState("STANDBY")
                            }
                        }
                        
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }
    
    // ========================================================================
    // HELPER FUNCTIONS
    // ========================================================================
    
    function getDiseaseColor(disease) {
        switch(disease) {
            case "Normal": return "#00ff00"
            case "Obstructive": return "#ffaa00"
            case "Restrictive": return "#ff3333"
            default: return "#aabbcc"
        }
    }
    
    function getDiseaseBackgroundColor(disease) {
        switch(disease) {
            case "Normal": return "#0a2a0a"
            case "Obstructive": return "#2a1a0a"
            case "Restrictive": return "#2a0a0a"
            default: return "#0f1219"
        }
    }
    
    function getConfidenceColor(confidence) {
        if (confidence >= 0.9) return "#00ff00"
        if (confidence >= 0.7) return "#ffaa00"
        if (confidence >= 0.5) return "#ff6600"
        return "#ff3333"
    }
    
    function getStateColor(state) {
        switch(state) {
            case "STANDBY": return "#666666"
            case "CLASSIFYING": return "#ffaa00"
            case "CLASSIFIED": return "#00ff00"
            case "ERROR": return "#ff3333"
            default: return "#aabbcc"
        }
    }
    
    function formatDetailedReport() {
        var report = "Patient State Report\n"
        report += "═══════════════════════════════════════\n\n"
        
        report += "Classification Results:\n"
        report += "  Disease Type: " + (patientState.current_disease || "Unknown") + "\n"
        report += "  Confidence: " + (patientState.current_confidence * 100).toFixed(2) + "%\n"
        report += "  State: " + (patientState.current_state || "STANDBY") + "\n\n"
        
        report += "Probabilities:\n"
        try {
            var probs = patientState.current_probabilities
            report += "  Normal: " + ((probs["Normal"] || 0) * 100).toFixed(2) + "%\n"
            report += "  Obstructive: " + ((probs["Obstructive"] || 0) * 100).toFixed(2) + "%\n"
            report += "  Restrictive: " + ((probs["Restrictive"] || 0) * 100).toFixed(2) + "%\n\n"
        } catch(e) {
            report += "  [Pending classification]\n\n"
        }
        
        report += "System Information:\n"
        report += "  Timestamp: " + Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm:ss") + "\n"
        report += "  Classification Time: " + 
            (patientState.classification_time > 0 ? 
                (patientState.classification_time * 1000).toFixed(3) + " ms" : 
                "—") + "\n"
        report += "  Features Used: 70+ (Time + Frequency Domain)\n"
        report += "  ML Model: Random Forest Classifier\n"
        
        return report
    }
}
