package com.ava.tool;
import android.hardware.usb.*;
import java.io.IOException;
import java.util.Arrays;

public final class UsbTransportTest {
    static void check(boolean ok, String text) { if(!ok) throw new AssertionError(text); }
    static AndroidUsbTransport open(UsbDeviceConnection c) throws IOException { return new AndroidUsbTransport(new UsbManager(c),new UsbDevice(),s->{}); }
    public static void main(String[] args) throws Exception {
        UsbDeviceConnection c = new UsbDeviceConnection();
        AndroidUsbTransport t=open(c);
        check(!c.forced,"must not detach kernel driver");
        c.replies.add(new byte[]{1,2,3,4});
        check(Arrays.equals(t.read(2),new byte[]{1,2}),"first half of USB packet");
        check(Arrays.equals(t.read(2),new byte[]{3,4}) && c.reads==1,"buffer over-read without redundant USB request");
        c.replies.add(new byte[]{5}); c.replies.add(new byte[]{6,7});
        check(Arrays.equals(t.read(3),new byte[]{5,6,7}),"split USB responses preserved");
        t.close(); t.close(); check(c.closes==1 && c.releases==1,"idempotent close");
        try { t.read(1); throw new AssertionError("read after close"); } catch(IOException expected) {}
        c = new UsbDeviceConnection(); c.claim=false;
        try { open(c); throw new AssertionError("claim unexpectedly succeeded"); } catch(IOException expected) {}
        check(c.closes==1 && !c.forced,"failed claim closes without forced fallback");
        c = new UsbDeviceConnection(); c.throwClaim=true;
        try { open(c); throw new AssertionError("claim exception lost"); } catch(IllegalStateException expected) {}
        check(c.closes==1,"claim exception closes file descriptor");
        c = new UsbDeviceConnection(); t=open(c); c.throwRelease=true;
        try { t.close(); throw new AssertionError("release exception lost"); } catch(IllegalStateException expected) {}
        t.close(); check(c.closes==1,"release exception still closes connection exactly once");
        // An HSS response contains a 4-byte length followed by up to 8192
        // payload bytes. Header and payload may share, or span, USB packets.
        byte[] full = new byte[8196]; JlinkHss.put32(full,0,8192);
        for(int i=4;i<full.length;i++) full[i]=(byte)(i*31);
        for(int chunk:new int[]{1,3,512,4096,8196}) {
            c=new UsbDeviceConnection(); t=open(c);
            for(int start=0;start<full.length;start+=chunk)
                c.replies.add(Arrays.copyOfRange(full,start,Math.min(full.length,start+chunk)));
            check(Jlink.u32(t.read(4),0)==8192,"HSS length with USB fragmentation");
            check(Arrays.equals(t.read(8192),Arrays.copyOfRange(full,4,full.length)),"HSS payload preserved at chunk="+chunk);
            t.close();
        }
        System.out.println("PASS: USB transport buffering, non-forced claim, failure cleanup and idempotent close (mock Android USB)");
    }
}
