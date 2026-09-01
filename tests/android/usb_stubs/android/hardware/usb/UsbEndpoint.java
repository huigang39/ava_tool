package android.hardware.usb;
public class UsbEndpoint {
    private final int direction;
    public UsbEndpoint(int direction) { this.direction=direction; }
    public int getType() { return 2; }
    public int getDirection() { return direction; }
}
