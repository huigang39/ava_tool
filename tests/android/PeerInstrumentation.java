package com.ava.tool;

import android.app.Instrumentation;
import android.os.Bundle;
import java.util.Arrays;

/** Runs the actual Android JNI transport, without opening USB. */
public final class PeerInstrumentation extends Instrumentation {
    private final byte[] memory = new byte[4096];
    private long epoch = 1;
    public synchronized String peerDevice() {
        return "{\"name\":\"Android fixture\",\"epoch\":"+epoch+",\"ready\":true}";
    }
    public synchronized boolean peerRead(long e, long a, byte[] out) {
        if(a>=0x20000000L)a-=0x20000000L;
        if(e!=epoch||a<0||a+out.length>memory.length)return false;
        System.arraycopy(memory,(int)a,out,0,out.length);return true;
    }
    public synchronized boolean peerWrite(long e, long a, byte[] in) {
        if(a>=0x20000000L)a-=0x20000000L;
        if(e!=epoch||a<0||a+in.length>memory.length)return false;
        System.arraycopy(in,0,memory,(int)a,in.length);return true;
    }
    public synchronized boolean peerReset(long e) {if(e!=epoch)return false;epoch++;return true;}
    @Override public void onCreate(Bundle args) {super.onCreate(args);start();}
    @Override public void onStart() {
        Bundle result=new Bundle();int code=0;
        try {
            String key="0123456789abcdef0123456789abcdef";
            NativePeer.connect("10.0.2.2",45993,key);
            if(!Arrays.equals(NativePeer.read(0x20000000L,4),new byte[]{0,0,(byte)0xa0,0x3f}))throw new AssertionError("Initial float");
            NativePeer.claim(true);
            byte[] value={0,0,0x20,0x40};NativePeer.write(0x20000000L,value,false);
            if(!Arrays.equals(NativePeer.read(0x20000000L,4),value))throw new AssertionError("Write/read");
            int frame=NativePeer.startSamples("[{\"address\":536870912,\"size\":4}]",20000);
            Thread.sleep(120);byte[] samples=NativePeer.samples();
            if(frame!=8||samples.length<16||samples.length%frame!=0)throw new AssertionError("Sampling");
            NativePeer.stopSamples();NativePeer.claim(false);NativePeer.close();
            NativePeer.startServer(this,"Android fixture",45994,key,true);
            Bundle phase=new Bundle();phase.putString("phase","server-ready");sendStatus(0,phase);
            // The host runs its independent Python peer against this endpoint.
            Thread.sleep(12000);
            synchronized(this){if(!Arrays.equals(Arrays.copyOf(memory,4),new byte[]{0,0,0x60,0x40}))throw new AssertionError("Host did not write Android fixture");}
            result.putString("result","PASS: Android controller and exporter, desktop interoperability, read/write and timestamped samples");
            code=-1;
        } catch(Throwable e) {result.putString("error",e.toString());}
        finally {NativePeer.close();NativePeer.stopServer();finish(code,result);}
    }
}
