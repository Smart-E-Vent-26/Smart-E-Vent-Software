"""
BVM Ventilator - Patient State Display
PyQt6/PySide6 Application with ML Classifier Integration

Structure:
    root/
    ├── ui/
    │   ├── main.py (this file)
    │   └── PatientState.qml
    └── ML/
        ├── ml_classifier.py
        └── weights/
            └── *.pkl
"""

import sys
import os
import json
import pickle
import numpy as np
from pathlib import Path
from datetime import datetime
from typing import Dict, Any, Optional
import threading
import time

# Qt imports
try:
    from PyQt6.QtWidgets import QApplication, QMainWindow
    from PyQt6.QtQml import QQmlApplicationEngine, qmlRegisterType
    from PyQt6.QtCore import QUrl, QObject, pyqtSignal, pyqtSlot, QTimer, QThread
    from PyQt6.QtGui import QIcon
    QT_VERSION = 6
except ImportError:
    from PySide6.QtWidgets import QApplication, QMainWindow
    from PySide6.QtQml import QQmlApplicationEngine, qmlRegisterType
    from PySide6.QtCore import QUrl, QObject, Signal as pyqtSignal, Slot as pyqtSlot, QTimer, QThread
    from PySide6.QtGui import QIcon
    QT_VERSION = 6

# Add ML module to path
ML_PATH = os.path.join(os.path.dirname(__file__), '..', 'ML')
UI_PATH = os.path.dirname(__file__)
ROOT_PATH = os.path.dirname(UI_PATH)

if ML_PATH not in sys.path:
    sys.path.insert(0, ML_PATH)

# Import ML classifier
try:
    from ml_classifier import MLClassifier
except ImportError:
    print(f"Error: Could not import MLClassifier from {ML_PATH}")
    print(f"Make sure ml_classifier.py exists in {ML_PATH}")
    sys.exit(1)

# ============================================================================
# PATIENT STATE MODEL
# ============================================================================

class PatientState(QObject):
    """
    Manages patient state and communicates with QML
    
    Signals:
    --------
    stateChanged: Emitted when patient state is updated
    diseaseTypeChanged: Emitted when disease type is determined
    confidenceChanged: Emitted when confidence score updates
    waveformDataChanged: Emitted when waveform data is available
    statusChanged: Emitted when status message changes
    """
    
    stateChanged = pyqtSignal(str)
    diseaseTypeChanged = pyqtSignal(str)
    confidenceChanged = pyqtSignal(float)
    probabilitiesChanged = pyqtSignal(str)  # JSON string of probabilities
    waveformDataChanged = pyqtSignal(str)   # JSON string of waveform data
    statusChanged = pyqtSignal(str)
    classificationTimingChanged = pyqtSignal(str)
    
    def __init__(self, ml_classifier: 'MLClassifier'):
        """
        Initialize patient state manager
        
        Parameters
        ----------
        ml_classifier : MLClassifier
            The ML classifier instance
        """
        super().__init__()
        
        self.classifier = ml_classifier
        self.current_state = "STANDBY"
        self.current_disease = "Unknown"
        self.current_confidence = 0.0
        self.current_probabilities = {}
        self.waveform_data = {}
        self.last_update_time = None
        self.classification_time = 0.0
        
        # Waveform buffer for display
        self.pressure_buffer = []
        self.flow_buffer = []
        self.volume_buffer = []
        self.max_buffer_size = 1000  # ~1 second at 1kHz
        
    @pyqtSlot(str)
    def updateState(self, state: str):
        """Update patient state"""
        self.current_state = state
        self.stateChanged.emit(state)
        self.statusChanged.emit(f"State: {state}")
    
    @pyqtSlot(str, list)
    def classifyPatientFeatures(self, patient_id: str, features: list):
        """
        Classify patient based on extracted features
        
        Parameters
        ----------
        patient_id : str
            Patient identifier
        features : list
            Extracted features from waveforms
        """
        try:
            self.updateState("CLASSIFYING")
            self.statusChanged.emit("Running classification...")
            
            # Measure classification time
            start_time = time.time()
            
            # Convert features to numpy array
            features_array = np.array(features, dtype=np.float32)
            
            # Run classification
            disease_type, confidence, probabilities = self.classifier.predict(features_array)
            
            # Calculate classification time
            self.classification_time = time.time() - start_time
            
            # Update state
            self.current_disease = disease_type
            self.current_confidence = float(confidence)
            self.current_probabilities = probabilities
            self.last_update_time = datetime.now()
            
            # Emit signals
            self.diseaseTypeChanged.emit(disease_type)
            self.confidenceChanged.emit(confidence)
            self.probabilitiesChanged.emit(json.dumps(probabilities))
            self.classificationTimingChanged.emit(f"{self.classification_time*1000:.2f} ms")
            
            # Update state to classified
            self.updateState("CLASSIFIED")
            self.statusChanged.emit(f"✓ {disease_type} (Confidence: {confidence*100:.1f}%)")
            
        except Exception as e:
            self.updateState("ERROR")
            self.statusChanged.emit(f"Error: {str(e)}")
    
    @pyqtSlot(list, list, list)
    def updateWaveforms(self, pressure: list, flow: list, volume: list):
        """
        Update waveform data
        
        Parameters
        ----------
        pressure : list
            Pressure waveform data
        flow : list
            Flow waveform data
        volume : list
            Volume waveform data
        """
        try:
            self.pressure_buffer.extend(pressure)
            self.flow_buffer.extend(flow)
            self.volume_buffer.extend(volume)
            
            # Keep only recent data
            if len(self.pressure_buffer) > self.max_buffer_size:
                self.pressure_buffer = self.pressure_buffer[-self.max_buffer_size:]
                self.flow_buffer = self.flow_buffer[-self.max_buffer_size:]
                self.volume_buffer = self.volume_buffer[-self.max_buffer_size:]
            
            # Emit waveform data
            waveform_json = json.dumps({
                'pressure': self.pressure_buffer[-100:],  # Send last 100 points
                'flow': self.flow_buffer[-100:],
                'volume': self.volume_buffer[-100:]
            })
            self.waveformDataChanged.emit(waveform_json)
            
        except Exception as e:
            print(f"Error updating waveforms: {e}")
    
    @pyqtSlot(result=str)
    def getStateString(self) -> str:
        """Get current state as formatted string"""
        return f"State: {self.current_state}\nDisease: {self.current_disease}\nConfidence: {self.current_confidence*100:.1f}%"
    
    @pyqtSlot(result=str)
    def getDiseaseType(self) -> str:
        """Get current disease type"""
        return self.current_disease
    
    @pyqtSlot(result=float)
    def getConfidence(self) -> float:
        """Get current confidence score"""
        return self.current_confidence
    
    @pyqtSlot(result=str)
    def getProbabilities(self) -> str:
        """Get probabilities as JSON string"""
        return json.dumps(self.current_probabilities)
    
    @pyqtSlot(result=str)
    def getDetailedReport(self) -> str:
        """Get detailed classification report"""
        report = {
            'timestamp': self.last_update_time.isoformat() if self.last_update_time else None,
            'state': self.current_state,
            'disease_type': self.current_disease,
            'confidence': self.current_confidence,
            'probabilities': self.current_probabilities,
            'classification_time_ms': self.classification_time * 1000
        }
        return json.dumps(report, indent=2)

# ============================================================================
# ML CLASSIFIER WRAPPER
# ============================================================================

class MLClassifierThread(QThread):
    """
    Run ML classifier in separate thread to avoid UI blocking
    
    Signals
    -------
    classificationDone : Emitted when classification is complete
    """
    
    classificationDone = pyqtSignal(str, float, dict)  # disease_type, confidence, probabilities
    error = pyqtSignal(str)
    
    def __init__(self, classifier: 'MLClassifier'):
        super().__init__()
        self.classifier = classifier
        self.features = None
    
    def setFeatures(self, features: np.ndarray):
        """Set features to classify"""
        self.features = features
    
    def run(self):
        """Run classification in thread"""
        try:
            if self.features is None:
                self.error.emit("No features provided")
                return
            
            disease_type, confidence, probabilities = self.classifier.predict(self.features)
            self.classificationDone.emit(disease_type, float(confidence), probabilities)
            
        except Exception as e:
            self.error.emit(f"Classification error: {str(e)}")

# ============================================================================
# MAIN APPLICATION WINDOW
# ============================================================================

class BVMApplication(QMainWindow):
    """
    Main application window for BVM Ventilator Patient State Display
    """
    
    def __init__(self, app: QApplication):
        super().__init__()
        
        self.app = app
        self.qml_engine = None
        self.patient_state = None
        self.classifier = None
        self.classifier_thread = None
        
        # Initialize
        self.init_classifier()
        self.init_ui()
        
    def init_classifier(self):
        """Initialize ML classifier"""
        try:
            print(f"Loading ML classifier from {ML_PATH}...")
            self.classifier = MLClassifier(
                weights_dir=os.path.join(ML_PATH, 'weights')
            )
            print("✓ ML Classifier loaded successfully")
        except Exception as e:
            print(f"✗ Error loading ML classifier: {e}")
            print(f"  Weights directory: {os.path.join(ML_PATH, 'weights')}")
            print(f"  Available files: {os.listdir(ML_PATH) if os.path.exists(ML_PATH) else 'ML_PATH not found'}")
    
    def init_ui(self):
        """Initialize QML UI"""
        try:
            # Create QML engine
            self.qml_engine = QQmlApplicationEngine()
            
            # Register Python classes to QML
            self.patient_state = PatientState(self.classifier)
            self.qml_engine.rootContext().setContextProperty("patientState", self.patient_state)
            self.qml_engine.rootContext().setContextProperty("appWindow", self)
            
            # Load QML file
            qml_file = os.path.join(UI_PATH, "PatientState.qml")
            if not os.path.exists(qml_file):
                print(f"✗ QML file not found: {qml_file}")
                print(f"  Creating default QML file...")
                self.create_default_qml(qml_file)
            
            qml_url = QUrl.fromLocalFile(os.path.abspath(qml_file))
            self.qml_engine.load(qml_url)
            
            if not self.qml_engine.rootObjects():
                print("✗ Failed to load QML file")
                sys.exit(1)
            
            # Get root object
            root = self.qml_engine.rootObjects()[0]
            root.show()
            self.show()
            
            print("✓ UI initialized successfully")
            
        except Exception as e:
            print(f"✗ Error initializing UI: {e}")
            sys.exit(1)
    
    def create_default_qml(self, qml_file: str):
        """Create default QML file if it doesn't exist"""
        default_qml = '''import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 1200
    height: 800
    title: "BVM Ventilator - Patient State Display"
    
    color: "#1e1e1e"
    
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20
        
        // Header
        Rectangle {
            Layout.fillWidth: true
            height: 60
            color: "#2d2d2d"
            radius: 8
            
            Text {
                anchors.fill: parent
                anchors.margins: 15
                text: "BVM Ventilator - ML Patient Diagnosis"
                font.pixelSize: 24
                font.bold: true
                color: "#00ff00"
                verticalAlignment: Text.AlignVCenter
            }
        }
        
        // Main content
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 20
            
            // Left panel - Classification results
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: parent.width * 0.5
                color: "#2d2d2d"
                radius: 8
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 15
                    
                    Text {
                        text: "Classification Results"
                        font.pixelSize: 18
                        font.bold: true
                        color: "#00ff00"
                    }
                    
                    // Disease Type
                    Rectangle {
                        Layout.fillWidth: true
                        height: 80
                        color: "#3d3d3d"
                        radius: 4
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 5
                            
                            Text {
                                text: "Disease Type"
                                font.pixelSize: 12
                                color: "#aaaaaa"
                            }
                            
                            Text {
                                text: patientState.diseaseTypeChanged ? patientState.getDiseaseType() : "Unknown"
                                font.pixelSize: 28
                                font.bold: true
                                color: getDiseaseColor(patientState.getDiseaseType())
                            }
                        }
                    }
                    
                    // Confidence Score
                    Rectangle {
                        Layout.fillWidth: true
                        height: 80
                        color: "#3d3d3d"
                        radius: 4
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 5
                            
                            Text {
                                text: "Confidence Score"
                                font.pixelSize: 12
                                color: "#aaaaaa"
                            }
                            
                            RowLayout {
                                spacing: 15
                                
                                Text {
                                    text: (patientState.getConfidence() * 100).toFixed(1) + "%"
                                    font.pixelSize: 28
                                    font.bold: true
                                    color: getConfidenceColor(patientState.getConfidence())
                                }
                                
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 30
                                    color: "#1e1e1e"
                                    radius: 4
                                    
                                    Rectangle {
                                        height: parent.height
                                        width: parent.width * patientState.getConfidence()
                                        color: getConfidenceColor(patientState.getConfidence())
                                        radius: 4
                                    }
                                }
                            }
                        }
                    }
                    
                    // State
                    Rectangle {
                        Layout.fillWidth: true
                        height: 60
                        color: "#3d3d3d"
                        radius: 4
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 5
                            
                            Text {
                                text: "Current State"
                                font.pixelSize: 12
                                color: "#aaaaaa"
                            }
                            
                            Text {
                                text: patientState.stateChanged ? patientState.current_state : "STANDBY"
                                font.pixelSize: 18
                                font.bold: true
                                color: "#00ff00"
                            }
                        }
                    }
                    
                    // Probabilities
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#3d3d3d"
                        radius: 4
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 10
                            
                            Text {
                                text: "Disease Probabilities"
                                font.pixelSize: 12
                                color: "#aaaaaa"
                            }
                            
                            // Disease probability items
                            Repeater {
                                model: ["Normal", "Obstructive", "Restrictive"]
                                
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 50
                                    color: "#1e1e1e"
                                    radius: 4
                                    
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 3
                                        
                                        RowLayout {
                                            spacing: 10
                                            
                                            Text {
                                                text: modelData
                                                font.pixelSize: 12
                                                color: "#cccccc"
                                                Layout.preferredWidth: 100
                                            }
                                            
                                            Rectangle {
                                                Layout.fillWidth: true
                                                height: 20
                                                color: "#3d3d3d"
                                                radius: 3
                                                
                                                Rectangle {
                                                    height: parent.height
                                                    width: parent.width * getProbability(modelData)
                                                    color: getDiseaseColorByName(modelData)
                                                    radius: 3
                                                }
                                            }
                                            
                                            Text {
                                                text: (getProbability(modelData) * 100).toFixed(1) + "%"
                                                font.pixelSize: 11
                                                color: "#ffffff"
                                                Layout.preferredWidth: 50
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            // Right panel - Status and Waveforms
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: parent.width * 0.5
                color: "#2d2d2d"
                radius: 8
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 15
                    
                    Text {
                        text: "Status & Information"
                        font.pixelSize: 18
                        font.bold: true
                        color: "#00ff00"
                    }
                    
                    // Status message
                    Rectangle {
                        Layout.fillWidth: true
                        height: 60
                        color: "#3d3d3d"
                        radius: 4
                        
                        Text {
                            anchors.fill: parent
                            anchors.margins: 15
                            text: patientState.statusChanged ? patientState.statusChanged : "Ready"
                            font.pixelSize: 14
                            color: "#00ff00"
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    
                    // Detailed report
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#3d3d3d"
                        radius: 4
                        
                        TextEdit {
                            anchors.fill: parent
                            anchors.margins: 15
                            text: "Classification Report:\\n" + patientState.getDetailedReport()
                            font.pixelSize: 10
                            font.family: "Courier"
                            color: "#00ff00"
                            readOnly: true
                            wrapMode: TextEdit.Wrap
                        }
                    }
                }
            }
        }
        
        // Footer with buttons
        Rectangle {
            Layout.fillWidth: true
            height: 50
            color: "#2d2d2d"
            radius: 8
            
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 10
                
                Button {
                    text: "Refresh"
                    onClicked: {
                        // Placeholder for refresh action
                    }
                }
                
                Item { Layout.fillWidth: true }
                
                Text {
                    text: "Classification time: " + (patientState.classificationTimingChanged || "0.00 ms")
                    font.pixelSize: 12
                    color: "#aaaaaa"
                }
            }
        }
    }
    
    function getDiseaseColor(disease) {
        if (disease === "Normal") return "#00ff00"
        if (disease === "Obstructive") return "#ffaa00"
        if (disease === "Restrictive") return "#ff0000"
        return "#ffffff"
    }
    
    function getDiseaseColorByName(name) {
        if (name === "Normal") return "#00ff00"
        if (name === "Obstructive") return "#ffaa00"
        if (name === "Restrictive") return "#ff0000"
        return "#ffffff"
    }
    
    function getConfidenceColor(confidence) {
        if (confidence >= 0.9) return "#00ff00"
        if (confidence >= 0.7) return "#ffaa00"
        return "#ff0000"
    }
    
    function getProbability(disease) {
        try {
            var probs = JSON.parse(patientState.getProbabilities())
            if (disease === "Normal") return probs["Normal"] || 0
            if (disease === "Obstructive") return probs["Obstructive"] || 0
            if (disease === "Restrictive") return probs["Restrictive"] || 0
        } catch (e) {
            return 0
        }
        return 0
    }
}
'''
        
        with open(qml_file, 'w') as f:
            f.write(default_qml)
        print(f"✓ Created default QML file: {qml_file}")

# ============================================================================
# MAIN ENTRY POINT
# ============================================================================

def main():
    """Main application entry point"""
    
    app = QApplication(sys.argv)
    
    # Set application style
    app.setStyle('Fusion')
    
    # Create main window
    main_window = BVMApplication(app)
    
    # Run application
    sys.exit(app.exec())

if __name__ == '__main__':
    main()
