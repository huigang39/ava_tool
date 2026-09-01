package android.hardware.usb;
public class UsbDevice {
    public int getInterfaceCount() { return 1; }
    public UsbInterface getInterface(int i) { return new UsbInterface(); }
    public int getVendorId() { return 0x1366; }
    public int getProductId() { return 0x1010; }
}
