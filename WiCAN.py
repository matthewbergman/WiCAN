import time
import sys
import configparser
import os
import traceback

import can
import cantools

from PyQt5.QtWidgets import QApplication, QMainWindow, qApp, QDialog
from PyQt5.QtWidgets import QMdiArea, QMdiSubWindow
from PyQt5.QtWidgets import QBoxLayout, QGridLayout, QFrame
from PyQt5.QtWidgets import QAction, QMenu
from PyQt5.QtWidgets import QWidget, QTextEdit, QLabel, QComboBox, QGroupBox, QPushButton, QLineEdit
from PyQt5.QtWidgets import QTableWidget, QTableWidgetItem, QAbstractItemView, QHeaderView, QListWidget
from PyQt5.QtWidgets import QInputDialog, QFileDialog

from PyQt5.QtCore import Qt
from PyQt5.QtCore import QThread, QWaitCondition, QMutex
from PyQt5.QtCore import pyqtSlot, pyqtSignal
from PyQt5.QtCore import QTimer, QIODevice, QByteArray
from PyQt5.QtGui import QColor, QIcon

class CANThread(QThread):
    can_recv_signal = pyqtSignal(object)
    can_status_signal = pyqtSignal(int)

    def __init__(self):
        QThread.__init__(self)
        self.bus = None
        
    def connect(self, _type, _channel, _bitrate):
        try:
            self.bus = can.interface.Bus(bustype=_type, channel=_channel, bitrate=_bitrate)
            print("Connected to CAN device")
            self.can_status_signal.emit(0)
        except:
            self.bus = None
            print("Failed to find CAN device")
            traceback.print_exc()
            self.can_status_signal.emit(1)

    def disconnect(self):
        try:
            self.bus.shutdown()
            self.send_status_signal.emit(2)
        except:
            print("Failed to shut down bus")
        self.bus = None

    def reset(self):
        try:
            self.bus.reset()
        except:
            print("Failed to reset bus")

    def run(self):
        while True:
            if self.bus != None:
                for msg in self.bus:
                    self.can_recv_signal.emit(msg)
            else:
                time.sleep(1)

    @pyqtSlot(object)
    def send(self, msg):
        if self.bus != None:
            try:
                self.bus.send(msg, 1)
            except:
                print("Failed to send CAN message")
        else:
            print("Bus not initialized")

class MDIWindow(QMainWindow):

    can_send_signal = pyqtSignal(object)

    PCAN_STATE_CONNECTED = 1
    PCAN_STATE_DISCONNECTED = 0

    def __init__(self):
        super().__init__()

        self.pcan_state = self.PCAN_STATE_DISCONNECTED
        self.can_row = 0
        self.can_data = {}
        self.dbc_windows = {}

        self.loadPreferences()

        self.mdi = QMdiArea()
        self.setCentralWidget(self.mdi)
        
        bar = self.menuBar()

        file = bar.addMenu("File")
        file.addAction("Connect")
        file.addAction("Open DBC")
        file.triggered[QAction].connect(self.fileMenuClicked)

        view = bar.addMenu("View")
        view.addAction("Cascade")
        view.addAction("TiledC")
        view.triggered[QAction].connect(self.viewMenuClicked)
        
        self.setWindowTitle("WiCAN")
        self.statusBar().showMessage('Disconnected')

        self.createCANTableSubWindow()

        self.can_thread = CANThread()
        self.can_send_signal.connect(self.can_thread.send)
        self.can_thread.can_recv_signal.connect(self.handleCANMessage)
        self.can_thread.can_status_signal.connect(self.handleCANStatus)
        self.can_thread.start()

    def createCANTableSubWindow(self):
        self.can_table = QTableWidget(50,9)
        
        sub = QMdiSubWindow()
        sub.setWidget(self.can_table)
        sub.setWindowTitle("Raw CAN Frames")
        self.mdi.addSubWindow(sub)
        sub.show()

    def fileMenuClicked(self, menuitem):
        if menuitem.text() == "Connect":
            diag = ConnectDialog(self)
            diag.show()
            diag.exec_()
            settings = diag.getSettings()
            self.CANConnect(settings)
        elif menuitem.text() == "Open DBC":
            self.loadDBCFileDialog()

    def viewMenuClicked(self, menuitem):
        if menuitem.text() == "Cascade":
            self.mdi.cascadeSubWindows()
        elif menuitem.text() == "Tiled":
            self.mdi.tileSubWindows()

    def loadPreferences(self):
        self.dbc_path = os.path.dirname(os.path.realpath(__file__))

    def loadDBCFileDialog(self):
        options = QFileDialog.Options()
        options |= QFileDialog.DontUseNativeDialog
        types = "DBC Files (*.dbc)"
        file_path, _ = QFileDialog.getOpenFileName(self, "Open DBC", self.dbc_path, types, options=options)
        if file_path:
            dbc_win = DBCWindow(file_path, self)
            sub = QMdiSubWindow()
            sub.setWidget(dbc_win)
            #sub.setWindowTitle("Raw CAN Frames")
            self.mdi.addSubWindow(sub)
            sub.show()

    @pyqtSlot(int)
    def handleCANStatus(self, status):
        if status == 1:
            self.statusBar().showMessage("Failed to find CAN device")
            self.pcan_state = self.PCAN_STATE_DISCONNECTED
            
        elif status == 0:
            self.statusBar().showMessage("CAN device connected")
            self.pcan_state = self.PCAN_STATE_CONNECTED

        elif status == 2:
            self.statusBar().showMessage("Disconnected")
            self.pcan_state = self.PCAN_STATE_DISCONNECTED

    def CANConnect(self, connection):
        bustype = connection.bustype
        bitrate = connection.bitrate
        path = connection.path

        if bustype == 'PCAN':
            bustype = 'pcan'
            interface = 'PCAN_USBBUS1'
        elif bustype == 'KVaser':
            bustype = 'kvaser'
            interface = '0'
        elif bustype == 'IXXAT':
            bustype = 'ixxat'
            interface = '0'

        if bitrate == "125k":
            bitrate = 125000
        elif bitrate == "250k":
            bitrate = 250000
        elif bitrate == "500k":
            bitrate = 500000
        elif bitrate == "1M":
            bitrate = 1000000

        if self.pcan_state == self.PCAN_STATE_DISCONNECTED:
            self.statusBar().showMessage("Connecting...")
            self.can_thread.connect(bustype, interface, bitrate)
        elif self.pcan_state == self.PCAN_STATE_CONNECTED:
            self.statusBar().showMessage("Disconnecting...")
            self.can_thread.disconnect()

    @pyqtSlot(object)
    def handleCANMessage(self, msg):
        for file_name,window in self.dbc_windows:
            window.handleCANMessage(msg)

        can_id_hex = msg.arbitration_id
        can_id_printable = hex(can_id_hex)
        
        if can_id_hex not in self.can_data.keys():
            self.can_data[can_id_hex] = self.can_row
            can_id_item = QTableWidgetItem(can_id_printable)
            self.can_table.setItem(self.can_row, 0, can_id_item)
            row = self.can_row
            self.can_row += 1
        else:
            row = self.can_data[can_id_hex]

        for c in range(0,len(msg.data)):
            item = QTableWidgetItem(hex(msg.data[c]))
            self.can_table.setItem(row, c+1, item)

class DBCWindow(QWidget):
    def __init__(self, file_path, parent):
        QWidget.__init__(self, flags=Qt.Widget)

        file_name = os.path.basename(file_path)
        self.can_list_map = {}
        self.list_counter = 0
        self.dbc = cantools.database.load_file(file_path, database_format='dbc', cache_dir=None)#file_name.split(".")[0])

        self.setWindowTitle(file_name)
        layout = QBoxLayout(QBoxLayout.LeftToRight, parent=self)
        self.setLayout(layout)

        self.list_recv = QListWidget()
        self.list_send = QListWidget()

        layout.addWidget(self.list_recv)
        layout.addWidget(self.list_send)

        self.parent = parent
        self.parent.dbc_windows[file_name] = self

    def handleCANMessage(self, msg):
        try:
            frame = self.dbc.decode_message(msg.arbitration_id, msg.data, decode_choices=True, scaling=True)
        except:
            return

    def closeEvent(self, event):
        del self.parent.dbc_windows[file_name]

class ConnectDialog(QDialog):
    def __init__(self, parent):
        super().__init__(parent)

        self.setWindowTitle("Connect")

        grid = QGridLayout()
        
        self.combo_bustype = QComboBox()
        self.combo_bustype.addItem("PCAN")
        self.combo_bustype.addItem("KVaser")
        self.combo_bustype.addItem("Ixxat")
        self.combo_bustype.addItem("Serial")
        self.combo_bustype.addItem("Socket")
        self.combo_bustype.activated[str].connect(self.onBusTypeChange)

        self.combo_rate = QComboBox()
        self.combo_rate.addItem("500k")
        self.combo_rate.addItem("250k")
        self.combo_rate.addItem("1M")
        self.combo_rate.addItem("125k")

        self.path = QLineEdit(self)
        
        self.btn_open = QPushButton("Connect")
        self.btn_open.clicked.connect(self.openClicked)

        row = 0
        grid.addWidget(self.combo_bustype, row, 0)
        grid.addWidget(self.combo_rate, row, 1)
        row += 1

        grid.addWidget(QLabel("Path"), row, 0)
        grid.addWidget(self.path, row, 1)
        row += 1

        grid.addWidget(self.btn_open, row, 1)

        self.setLayout(grid)
        self.resize(300, 300)

        self.onBusTypeChange(None)

    def onBusTypeChange(self, text):
        if self.combo_bustype.currentText() == 'Serial':
            self.path.setReadOnly(False)
            self.path.setText("COM1")
        elif self.combo_bustype.currentText() == 'Socket':
            self.path.setReadOnly(False)
            self.path.setText("wican.local:8080")
        else:
            self.path.setText("")
            self.path.setReadOnly(True)

    def openClicked(self):
        self.close()

    def getSettings(self):
        settings = CANConnection(self.combo_bustype.currentText(), self.combo_rate.currentText(), self.path.text())
        return settings

class CANConnection():
    def __init__(self, bustype, bitrate, path):
        self.bustype = bustype
        self.bitrate = bitrate
        self.path = path

app = QApplication(sys.argv)
mdi = MDIWindow()
mdi.setGeometry(100, 100, 1000, 1000)
mdi.show()
app.exec_()
