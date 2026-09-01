package com.ava.tool;

import java.io.*;
import java.util.*;

public final class HssTest {
    static int checks;
    static void check(boolean b) { checks++; if(!b)throw new AssertionError(); }
    interface Action { void run() throws Exception; }
    static void fails(Action a) throws Exception { try { a.run(); } catch(IOException e) { checks++; return; } throw new AssertionError("Expected failure"); }
    static byte[] word(long n) { byte[] b=new byte[4]; JlinkHss.put32(b,0,n); return b; }
    static byte[] frames(long... timestamps) {
        byte[] b=new byte[timestamps.length*8];
        for(int i=0;i<timestamps.length;i++) { JlinkHss.put32(b,i*8,timestamps[i]); JlinkHss.put32(b,i*8+4,i+1); }
        return b;
    }
    static final class Usb implements Jlink.Transport {
        final List<byte[]> writes=new ArrayList<>(); final ArrayDeque<byte[]> replies=new ArrayDeque<>();
        void startup() { byte[] caps=new byte[36]; JlinkHss.put32(caps,4,100); JlinkHss.put32(caps,8,100000); replies.add(caps); replies.add(word(0)); }
        public void write(byte[] b) { writes.add(b.clone()); }
        public byte[] read(int n) throws IOException {
            byte[] b=replies.poll(); if(b==null || b.length!=n)throw new IOException("Wrong read size "+n); return b;
        }
        public void close() {}
    }
    public static void main(String[] args) throws Exception {
        check(JlinkHss.periodFor(20000)==50); check(JlinkHss.periodFor(25000)==40);
        check(JlinkHss.periodFor(30000)==34); fails(()->JlinkHss.periodFor(0)); fails(()->JlinkHss.periodFor(50001));
        check(JlinkHss.supportedRam(0x2001fffcL)); check(!JlinkHss.supportedRam(0x20020000L));
        check(!JlinkHss.supportedRam(0x20000002L)); check(!JlinkHss.supportedRam(0x40000000L));
        Usb usb=new Usb(); usb.startup(); JlinkHss h=new JlinkHss(usb); h.start(0x20004bb4L,20000);
        check(usb.writes.get(0).length==101);
        check(Arrays.equals(usb.writes.get(2),JlinkHss.hex("c5000e0101000000b44b00200400000000000000000000003200000001000000")));
        usb.replies.add(word(16)); byte[] data=frames(0,50); usb.replies.add(data);
        check(Arrays.equals(h.read(),data)); check(Arrays.equals(usb.writes.get(3),JlinkHss.hex("c5000e0300200000")));
        usb.replies.add(word(0)); check(h.read().length==0);
        usb.replies.add(word(760)); h.stop(); int writes=usb.writes.size(); h.stop(); check(writes==usb.writes.size());
        check(Arrays.equals(usb.writes.get(writes-1),JlinkHss.hex("c5000e02")));
        for(long length:new long[]{7,8193,0xffffffffL}) {
            Usb bad=new Usb(); bad.startup(); JlinkHss broken=new JlinkHss(bad); broken.start(0x20000000L,25000);
            bad.replies.add(word(length)); fails(broken::read); int before=bad.writes.size(); broken.stop(); check(before==bad.writes.size());
        }
        Usb rejected=new Usb(); rejected.startup(); rejected.replies.removeLast(); rejected.replies.add(word(-1));
        fails(()->new JlinkHss(rejected).start(0x20000000L,20000));
        Usb invalid=new Usb(); fails(()->new JlinkHss(invalid).start(0xe000ed00L,20000)); check(invalid.writes.isEmpty());
        // Wrap, ring overwrite, all-points export and reduced plot are separate concerns.
        HssCapture capture=new HssCapture(50,3); capture.accept(frames(0xffffffceL,0,50,100,200));
        HssCapture.Snapshot all=capture.snapshot(20), plot=capture.snapshot(2);
        check(all.total==5 && all.retained==3 && all.gaps==1);
        check(Arrays.equals(all.times,new long[]{100,150,250})); check(Arrays.equals(all.values,new int[]{3,4,5}));
        check(Arrays.equals(plot.times,new long[]{100,250})); check(all.hz==16000);
        fails(()->capture.accept(frames(200))); fails(()->capture.accept(frames(199)));
        fails(()->capture.accept(new byte[7]));
        HssCapture sustained=new HssCapture(50);
        for(int block=0;block<250;block++) {
            long[] timestamps=new long[1000]; for(int i=0;i<1000;i++)timestamps[i]=(block*1000L+i)*50;
            sustained.accept(frames(timestamps));
        }
        HssCapture.Snapshot result=sustained.snapshot(HssCapture.CAPACITY);
        check(result.total==250000 && result.retained==200000 && result.hz==20000 && result.gaps==0);
        check(result.times[0]==2500000 && result.times[199999]==12499950);
        System.out.println("PASS "+checks+" HSS checks: wire framing, limits, failures, timestamps, bounded 20 kHz ring and export");
    }
}
