package com.ava.tool;
import java.io.*;
import java.nio.file.Files;
import java.util.Arrays;

public final class DiagnosticsTest {
    static void check(boolean ok, String text) { if(!ok) throw new AssertionError(text); }
    public static void main(String[] args) throws Exception {
        File dir = Files.createTempDirectory("ava-journal-test").toFile();
        File file = new File(dir,"journal.txt");
        try {
            DeviceDiagnostics d = new DeviceDiagnostics(file);
            check(!file.exists() && d.history().contains("旧版本"),"opening diagnostics does not invent past logs");
            d.record("连接成功 / 中文");
            check(new DeviceDiagnostics(file).history().contains("连接成功 / 中文"),"journal survives instance restart and preserves UTF-8");
            char[] large = new char[4000]; Arrays.fill(large,'x');
            for(int i=0;i<20;i++) d.record(new String(large));
            d.record("LAST CONNECTION ERROR");
            check(file.length()<=65536 && new DeviceDiagnostics(file).history().contains("LAST CONNECTION ERROR"),"bounded journal retains latest failure");
            DeviceDiagnostics unavailable = new DeviceDiagnostics(new File(dir,"missing-parent/journal.txt"));
            unavailable.record("ignored");
            check(unavailable.history().contains("未能保存"),"storage failure is reported without breaking USB control");
            System.out.println("PASS: bounded durable diagnostic journal; reopen, UTF-8, pruning, I/O failure");
        } finally { Files.deleteIfExists(file.toPath()); Files.deleteIfExists(dir.toPath()); }
    }
}
