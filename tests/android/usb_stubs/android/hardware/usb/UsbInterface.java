package android.hardware.usb;
public class UsbInterface {
    public int getInterfaceClass() { return 255; }
    public int getEndpointCount() { return 2; }
    public UsbEndpoint getEndpoint(int i) { return new UsbEndpoint(i==0 ? 128 : 0); }
    public int getId() { return 0; }
}
