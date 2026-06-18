"""
serial_service.py — Small QSerialPort wrapper for the STM32 Edge Vision dashboard.

Keeps serial-port setup, reads, and line writes outside the GUI controller.
The firmware uses USART3 at configurable baud rate (default 2 000 000), 8N1,
without hardware flow control.
"""

from __future__ import annotations

from PySide6.QtCore import QObject
from PySide6.QtSerialPort import QSerialPort, QSerialPortInfo


class SerialService(QObject):
    """Thin wrapper around QSerialPort."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.serial = QSerialPort(self)

    def configure(self, port_name: str, baudrate: int) -> None:
        """Apply UART settings used by the STM32 firmware (8N1, no flow control)."""
        self.serial.setPortName(port_name)
        self.serial.setBaudRate(baudrate)
        self.serial.setDataBits(QSerialPort.Data8)
        self.serial.setParity(QSerialPort.NoParity)
        self.serial.setStopBits(QSerialPort.OneStop)
        self.serial.setFlowControl(QSerialPort.NoFlowControl)

    def open(self, port_name: str, baudrate: int) -> bool:
        """Open the selected serial port. Returns True on success."""
        self.close()
        self.configure(port_name, baudrate)
        return self.serial.open(QSerialPort.ReadWrite)

    def close(self) -> None:
        """Close the serial port if it is currently open."""
        if self.serial.isOpen():
            self.serial.close()

    def is_open(self) -> bool:
        """Return True while the serial port is open."""
        return self.serial.isOpen()

    def port_name(self) -> str:
        """Return the current serial port name."""
        return self.serial.portName()

    def error_string(self) -> str:
        """Return the last serial error string from Qt."""
        return self.serial.errorString()

    def write_line(self, command: str) -> bool:
        """Write one CRLF-terminated ASCII command to the STM32 protocol parser."""
        if not self.is_open():
            return False
        payload = (command.strip() + "\r\n").encode("utf-8")
        written = self.serial.write(payload)
        return written == len(payload)

    def read_bytes(self) -> bytes:
        """Read all bytes currently available from the UART receive buffer."""
        return bytes(self.serial.readAll())

    @staticmethod
    def available_ports() -> list[str]:
        """Return available COM ports sorted by name."""
        ports = sorted(
            QSerialPortInfo.availablePorts(),
            key=lambda p: p.portName().lower(),
        )
        return [p.portName() for p in ports]
