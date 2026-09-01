package com.ava.tool;
import java.io.IOException;
final class NativePeer {
    static { System.loadLibrary("ava_core"); }
    static native String key() throws IOException;
    static native String discover() throws IOException;
    static native void startServer(Object backend, String name, int port, String key, boolean writes) throws IOException;
    static native void stopServer();
    static native int viewers();
    static native boolean remoteOwnsWrites();
    static native void connect(String host, int port, String key) throws IOException;
    static native void close();
    static native boolean connected();
    static native boolean ownsControl();
    static native void claim(boolean acquire) throws IOException;
    static native byte[] read(long address, int size) throws IOException;
    static native void write(long address, byte[] value, boolean local) throws IOException;
    static native void reset(boolean local) throws IOException;
    static native int startSamples(String blocks, int periodUs) throws IOException;
    static native byte[] samples() throws IOException;
    static native void stopSamples();
}
