package com.ava.tool;
import java.io.IOException;
public final class NativeCoreTest {
    public static void main(String[] args) throws Exception {
        String json = NativeCore.parseElf(args[0]);
        for (String required : new String[]{"g_voltage", "g_motor.speed", "g_motor.status", "g_samples[0]", "g_samples[3]", "f32", "u32", "u8"})
            if (!json.contains("\""+required+"\"")) throw new AssertionError("Missing " + required + " in " + json);
        for(int i=1;i<args.length;i++) {
            try { NativeCore.parseElf(args[i]); throw new AssertionError("Malformed ELF accepted: " + args[i]); }
            catch(IOException expected) { }
        }
        System.out.println("PASS: Java -> JNI -> shared ELF/DWARF core; structs/arrays/types and malformed ELF rejection");
    }
}
