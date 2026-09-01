package android.hardware.usb;
import java.util.ArrayDeque;
public class UsbDeviceConnection {
    public boolean forced, claim=true, throwClaim, throwRelease;
    public int closes, releases, reads;
    public final ArrayDeque<byte[]> replies = new ArrayDeque<>();
    public boolean claimInterface(UsbInterface i, boolean force) {
        forced=force;
        if(throwClaim) throw new IllegalStateException("claim failure");
        return claim;
    }
    public boolean releaseInterface(UsbInterface i) {
        releases++;
        if(throwRelease) throw new IllegalStateException("release failure");
        return true;
    }
    public void close() { closes++; }
    public int bulkTransfer(UsbEndpoint ep, byte[] buffer, int length, int timeout) {
        if(ep.getDirection()!=128) return length;
        reads++;
        if(replies.isEmpty()) return -1;
        byte[] reply=replies.remove(); System.arraycopy(reply,0,buffer,0,reply.length); return reply.length;
    }
}
