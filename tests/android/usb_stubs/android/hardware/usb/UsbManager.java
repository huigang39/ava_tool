package android.hardware.usb;
public class UsbManager {
    public UsbDeviceConnection connection;
    public UsbManager(UsbDeviceConnection connection) { this.connection=connection; }
    public UsbDeviceConnection openDevice(UsbDevice device) { return connection; }
}
