package com.ava.tool;
import java.util.Arrays;
public final class PeerJniTest {
    private final byte[] memory=new byte[64];
    private long epoch=1;
    public synchronized String peerDevice(){return "{\"name\":\"Java fixture\",\"epoch\":"+epoch+",\"ready\":true}";}
    public synchronized boolean peerRead(long e,long a,byte[] out){if(e!=epoch||a<0||a+out.length>memory.length)return false;System.arraycopy(memory,(int)a,out,0,out.length);return true;}
    public synchronized boolean peerWrite(long e,long a,byte[] in){if(e!=epoch||a<0||a+in.length>memory.length)return false;System.arraycopy(in,0,memory,(int)a,in.length);return true;}
    public synchronized boolean peerReset(long e){if(e!=epoch)return false;epoch++;return true;}
    public static void main(String[] args)throws Exception{
        String key=NativePeer.key();PeerJniTest fake=new PeerJniTest();
        NativePeer.startServer(fake,"Java fixture",45994,key,true);
        try{
            NativePeer.connect("127.0.0.1",45994,key);if(!NativePeer.connected())throw new AssertionError();
            byte[] value={1,2,3,4};boolean rejected=false;try{NativePeer.write(0,value,false);}catch(java.io.IOException e){rejected=true;}if(!rejected)throw new AssertionError();
            NativePeer.claim(true);NativePeer.write(0,value,false);
            if(!Arrays.equals(NativePeer.read(0,4),value))throw new AssertionError();
            rejected=false;try{NativePeer.write(0,value,true);}catch(java.io.IOException e){rejected=true;}if(!rejected)throw new AssertionError("Local write bypassed lease");
            int frame=NativePeer.startSamples("[{\"address\":0,\"size\":4}]",20000);Thread.sleep(100);
            byte[] samples=NativePeer.samples();if(frame!=8||samples.length<16||samples.length%8!=0)throw new AssertionError();NativePeer.stopSamples();
            NativePeer.claim(false);NativePeer.write(4,value,true);NativePeer.claim(true);NativePeer.reset(false);if(fake.epoch!=2)throw new AssertionError();
        }finally{NativePeer.close();NativePeer.stopServer();}
        System.out.println("PASS: Java -> JNI -> authenticated TCP -> JNI worker callbacks, read/write, local lease enforcement, sampling and reset");
    }
}
