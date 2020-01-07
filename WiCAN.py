# WiCAN.py
#
# Matt Bergman
# Copyright (C) 2018
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import sys
import can
import time

from PyQt5.QtWidgets import QWidget
from PyQt5.QtWidgets import QGridLayout
from PyQt5.QtWidgets import QLabel
from PyQt5.QtWidgets import QComboBox
from PyQt5.QtWidgets import QGroupBox
from PyQt5.QtWidgets import QApplication
from PyQt5.QtWidgets import QTableWidget
from PyQt5.QtWidgets import QTableWidgetItem
from PyQt5.QtWidgets import QPushButton
from PyQt5.QtWidgets import QLineEdit
from PyQt5.QtCore import Qt
from PyQt5.QtCore import QThread
from PyQt5.QtCore import QIODevice
from PyQt5.QtCore import QWaitCondition
from PyQt5.QtCore import QMutex
from PyQt5.QtCore import QByteArray
from PyQt5.QtCore import pyqtSlot
from PyQt5.QtCore import pyqtSignal

class CANThread(QThread):
    recv_signal = pyqtSignal(object)

    def __init__(self, bus, chan):
        QThread.__init__(self)
        self.bus = can.interface.Bus(bustype=bus, channel=chan, bitrate=500000)
        print("CAN thread "+bus)

    # run method gets called when we start the thread
    def run(self):
        print("CAN THREAD START")
        for msg in self.bus:
            self.recv_signal.emit(msg)
        print("CAN THREAD DONE")

class Form(QWidget):
    def __init__(self):
        QWidget.__init__(self, flags=Qt.Widget)
        
        self.row = 0
        self.can_data = {}
        self.can_thread = None
        
        self.table = QTableWidget(50,9)
        self.path = QLineEdit(self)
        self.bus_type = QComboBox(self)
        self.connect_btn = QPushButton("Connect")
        self.disconnect_btn = QPushButton("Disconnect")
        self.init_widget()

    def init_widget(self):
        self.setWindowTitle("WiCAN")
        grid = QGridLayout()
        self.setLayout(grid)

        self.connect_btn.clicked.connect(self.slot_clicked_connect_button)
        self.disconnect_btn.clicked.connect(self.slot_clicked_disconnect_button)
        self.bus_type.activated[str].connect(self.onBusTypeChange)

        self.bus_type.addItem("serial")
        self.bus_type.addItem("ixxat")
        self.bus_type.addItem("pcan")

        self.path.setText("wican.local")

        grid.addWidget(self.bus_type,0,0)
        grid.addWidget(QLabel("Path"),0,1)
        grid.addWidget(self.path,0,2)
        grid.addWidget(self.connect_btn,0,3)
        grid.addWidget(self.disconnect_btn,0,4)
        grid.addWidget(self.table,1,0,1,5)

    def read_data(self, msg):
        can_id_hex = msg.arbitration_id
        can_id_printable = hex(can_id_hex)
        
        if can_id_hex not in self.can_data.keys():
            self.can_data[can_id_hex] = self.row
            can_id_item = QTableWidgetItem(can_id_printable)
            self.table.setItem(self.row, 0, can_id_item)
            row = self.row
            self.row += 1
        else:
            row = self.can_data[can_id_hex]

        for c in range(0,len(msg.data)):
            item = QTableWidgetItem(hex(msg.data[c]))
            self.table.setItem(row, c+1, item)

    @pyqtSlot(name="clickedConnectButton")
    def slot_clicked_connect_button(self):
        if self.bus_type.currentText() == 'ixxat':
            self.can_thread = CANThread('ixxat',0)
        elif self.bus_type.currentText() == 'pcan':
            self.can_thread = CANThread('pcan', 'PCAN_USBBUS1')
        elif self.bus_type.currentText() == 'serial':
            self.can_thread = CANThread('serial','socket://'+self.path.text()+':8080')
            
        self.can_thread.recv_signal.connect(self.read_data)
        self.can_thread.start()

    @pyqtSlot(name="clickedDisConnectButton")
    def slot_clicked_disconnect_button(self):
        #TODO: still doesn't work
        if self.can_thread != None:
            self.can_thread.quit()

    def onBusTypeChange(self, text):
        if self.bus_type.currentText() == 'ixxat' or self.bus_type.currentText() == 'pcan':
            self.path.setText("")
            self.path.setReadOnly(True)
        elif self.bus_type.currentText() == 'serial':
            self.path.setReadOnly(False)
            self.path.setText("wican.local")
        
if __name__ == "__main__":
    app = QApplication(sys.argv)
    excepthook = sys.excepthook
    sys.excepthook = lambda t, val, tb: excepthook(t, val, tb)
    form = Form()
    form.setGeometry(100, 100, 1450, 1600)
    form.show()
    exit(app.exec_())
