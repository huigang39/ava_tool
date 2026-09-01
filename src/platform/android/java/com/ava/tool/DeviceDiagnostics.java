package com.ava.tool;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

/** Local, bounded diagnostics. This class never opens a USB device or invokes root. */
final class DeviceDiagnostics {
    private final File journal;
    private String storageError = "";
    DeviceDiagnostics(File journal) { this.journal = journal; }
    synchronized void record(String event) {
        String line = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) + " " + event + "\n";
        byte[] data = line.getBytes(StandardCharsets.UTF_8);
        try (RandomAccessFile file = new RandomAccessFile(journal, "rw")) {
            if (file.length() + data.length > 65536) {
                byte[] tail = new byte[(int)Math.min(16384, file.length())];
                file.seek(file.length() - tail.length); file.readFully(tail);
                file.seek(0); file.write(tail); file.setLength(tail.length);
            }
            file.seek(file.length()); file.write(data, 0, Math.min(data.length, 8192)); file.getFD().sync();
        } catch (IOException e) { storageError = "记录未能保存：" + e.getMessage(); }
    }
    synchronized String history() {
        if (!journal.isFile()) return storageError.isEmpty() ? "此版本尚无持久连接记录（旧版本记录无法追溯）。" : storageError;
        try (RandomAccessFile file = new RandomAccessFile(journal, "r")) {
            byte[] bytes = new byte[(int)Math.min(file.length(), 32768)];
            file.seek(file.length() - bytes.length); file.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8) + (storageError.isEmpty() ? "" : "\n" + storageError);
        } catch (IOException e) { return "无法读取连接记录：" + e.getMessage(); }
    }
    static String bootReasons() {
        StringBuilder report = new StringBuilder();
        for (String key : new String[]{"ro.boot.bootreason", "sys.boot.reason", "persist.sys.boot.reason.history", "ro.smartisan.version"})
            report.append(key).append('=').append(property(key)).append('\n');
        report.append("空值/不可读不代表未发生故障；系统属性可能不足以区分供电异常、内核崩溃和系统看门狗。\n");
        return report.toString();
    }
    private static String property(String key) {
        Process p = null;
        try {
            p = new ProcessBuilder("/system/bin/getprop", key).redirectErrorStream(true).start();
            if (!p.waitFor(2, TimeUnit.SECONDS)) return "读取超时";
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[512]; int n;
            try (InputStream in = p.getInputStream()) {
                while (out.size() < 4096 && (n = in.read(buffer, 0, Math.min(buffer.length, 4096 - out.size()))) > 0) out.write(buffer, 0, n);
            }
            String value = new String(out.toByteArray(), StandardCharsets.UTF_8).trim();
            if (p.exitValue() != 0) return "不可读（exit=" + p.exitValue() + "）";
            return value.isEmpty() ? "未提供或当前应用无权读取" : value;
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); return "读取已取消"; }
        catch (IOException e) { return "不可读：" + e.getMessage(); }
        finally { if (p != null) { if (p.isAlive()) p.destroyForcibly(); try { p.getInputStream().close(); p.getErrorStream().close(); p.getOutputStream().close(); } catch (IOException ignored) {} } }
    }
}
