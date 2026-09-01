package com.ava.tool;
import java.io.IOException;
final class NativeCore {
    static { System.loadLibrary("ava_core"); }
    static native String parseElf(String path) throws IOException;
}
