package com.ava.tool;

import java.io.Closeable;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Locale;

/** USB J-Link + ARM ADIv5 SWD transport. No SEGGER desktop runtime or root needed.
 * Wire command reference: https://github.com/jonas-schievink/jaylink (MIT/Apache-2.0).
 * All access must be serialized on the application's I/O worker.
 */
public final class Jlink implements Closeable {
    public interface Transport extends Closeable {
        void write(byte[] data) throws IOException;
        byte[] read(int length) throws IOException;
    }
    public interface Log { void line(String text); }
    private static final class SwdFault extends IOException {
        SwdFault(String context) { super("SWD FAULT [" + context + "]"); }
    }
    private final Transport usb;
    private final Log log;
    private int ap;
    private int select = -1;
    private int csw = -1;
    private int ackOffset = 9;
    private boolean ready;
    private JlinkHss hss;
    public String firmware = "";
    public int millivolts;
    public int dpId, apId;
    public Jlink(Transport transport) { this(transport, text -> {}); }
    public Jlink(Transport transport, Log log) { usb = transport; this.log = log; }
    private void trace(String text) { log.line(text); }

    private byte[] command(int response, int... values) throws IOException {
        byte[] bytes = new byte[values.length];
        for (int i = 0; i < values.length; i++) bytes[i] = (byte) values[i];
        usb.write(bytes);
        return response == 0 ? new byte[0] : usb.read(response);
    }
    static long u32(byte[] b, int offset) {
        return (b[offset] & 255L) | (b[offset + 1] & 255L) << 8
            | (b[offset + 2] & 255L) << 16 | (b[offset + 3] & 255L) << 24;
    }
    public String identify() throws IOException {
        byte[] length = command(2, 0x01);
        int n = (length[0] & 255) | (length[1] & 255) << 8;
        if (n < 1 || n > 4096) throw new IOException("Invalid J-Link firmware response length: " + n);
        firmware = new String(usb.read(n), StandardCharsets.UTF_8).replace("\0", "").trim();
        byte[] state = command(8, 0x07);
        millivolts = (state[0] & 255) | (state[1] & 255) << 8;
        return firmware + " / VTref " + millivolts + " mV";
    }
    public void connect(int speedKhz, int accessPort) throws IOException {
        if (hss != null) throw new IOException("Close HSS session before reconnecting");
        ready = false;
        select = csw = -1;
        dpId = apId = 0;
        if (speedKhz < 1 || speedKhz > 12000 || accessPort < 0 || accessPort > 255)
            throw new IOException("SWD speed must be 1..12000 kHz; AP must be 0..255");
        ap = accessPort;
        identify();
        trace(firmware + " / VTref " + millivolts + " mV / SWD " + speedKhz + " kHz / AP" + ap);
        if (millivolts < 1000) throw new IOException("VTref too low (" + millivolts + " mV). Power the target and connect VTref/GND.");
        long caps = u32(command(4, 0xe8), 0);
        if ((caps & (1L << 17)) == 0) throw new IOException("This J-Link does not support interface selection / SWD");
        long interfaces = u32(command(4, 0xc7, 0xff), 0);
        if ((interfaces & 2) == 0) throw new IOException("This J-Link does not advertise SWD");
        command(4, 0xc7, 1);
        command(0, 0x05, speedKhz, speedKhz >> 8);
        trace("连接阶段：SWJ 切换与线路复位（不复位 MCU）");
        // SWJ sequence: 64 ones, JTAG-to-SWD 0xE79E (LSB first), reset, idle.
        Bits b = new Bits();
        b.out(-1L, 64); b.out(0xe79e, 16); b.out(-1L, 64); b.out(0, 8);
        exchange(b);
        trace("连接阶段：只读 DPIDR，校验 J-Link 输入采样布局");
        discoverSampling();
        trace(String.format(Locale.ROOT, "DPIDR=%08X", dpId));
        if (dpId == 0 || dpId == -1 || (dpId & 1) == 0) throw new IOException("Invalid SWD DPIDR; check SWDIO/SWCLK/GND");
        trace("连接阶段：清除 DP 粘滞错误，选择 DP bank 0");
        transfer(false, false, 0, 0x1e); // clear sticky errors, without resetting CPU
        transfer(false, false, 8, 0); select = 0;
        trace("连接阶段：请求调试与系统电源");
        transfer(false, false, 4, 0x50000000); // power requests; ORUNDETECT disabled
        boolean powered = false;
        int ctrl = 0;
        for (int i = 0; i < 100; i++) {
            ctrl = transfer(false, true, 4, 0);
            if ((ctrl & 0xa0000000) == 0xa0000000) { powered = true; break; }
            pause(5);
        }
        trace(String.format(Locale.ROOT, "DP CTRL/STAT=%08X", ctrl));
        if (!powered) throw new IOException("Debug/system power-up acknowledgement timed out");
        trace("连接阶段：读取 AP" + ap + " IDR（寄存器 0xFC）");
        try {
            apId = readAp(0xfc);
        } catch (SwdFault fault) {
            // Only retry this identification read. Never replay a failed memory write.
            trace(fault.getMessage());
            trace("读取 AP IDR 失败：记录 DP 状态，清除粘滞错误后仅重试一次 IDR；不更改 MCU 内存。");
            try { trace(String.format(Locale.ROOT, "故障时 DP CTRL/STAT=%08X", transfer(false, true, 4, 0))); }
            catch (IOException diagnostic) { trace("DP 状态不可读：" + diagnostic.getMessage()); }
            transfer(false, false, 0, 0x1e);
            select = csw = -1;
            apId = readAp(0xfc);
        }
        trace(String.format(Locale.ROOT, "AP%d IDR=%08X", ap, apId));
        // ADIv5 AHB-AP class = 8, type = 1. Other APs require a different memory driver.
        if (((apId >>> 13) & 15) != 8 || (apId & 15) != 1)
            throw new IOException(String.format("AP%d is not an ADIv5 AHB-AP (IDR=%08X); select the CPU AHB AP", ap, apId));
        ready = true;
    }
    private void lineReset() throws IOException {
        Bits reset = new Bits(); reset.out(-1L, 64); reset.out(0, 8); exchange(reset);
    }
    static boolean validDpId(int value) {
        // ARM JEP106 designer and architected fixed bit; reject floating/noisy data.
        return value != -1 && (value & 1) == 1 && ((value >>> 1) & 0x7ff) == 0x23b;
    }
    static int identifySampling(byte[] raw) throws IOException {
        int match = -1;
        for (int offset = 8; offset <= 9; offset++) {
            int value = bitField(raw, offset + 3, 32);
            if (bitField(raw, offset, 3) == 1 && validDpId(value)
                && (Integer.bitCount(value) & 1) == bitField(raw, offset + 35, 1)) {
                if (match != -1) throw new IOException("Ambiguous J-Link input sampling");
                match = offset;
            }
        }
        return match;
    }
    private static String hex(byte[] bytes, int length) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < length; i++) s.append(String.format(Locale.ROOT, "%02X", bytes[i] & 255));
        return s.toString();
    }
    private void discoverSampling() throws IOException {
        int confirmedOffset = -1, confirmedId = 0;
        // Two matching, parity-checked read-only observations are required before any DP/AP writes.
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) lineReset();
            Bits read = new Bits(); read.out(0xa5, 8); read.in(38); read.out(0, 8);
            byte[] raw = exchange(read);
            trace("DPIDR 原始采样=" + hex(raw, 7));
            int offset = identifySampling(raw);
            if (offset < 0) { confirmedOffset = -1; continue; }
            int value = bitField(raw, offset + 3, 32);
            if (confirmedOffset == offset && confirmedId == value) {
                ackOffset = offset; dpId = value;
                trace("J-Link 输入采样：ACK bit=" + offset + "，DPIDR/奇偶校验两次一致");
                return;
            }
            confirmedOffset = offset; confirmedId = value;
        }
        throw new IOException("SWD DPIDR 同步失败：无法确认 ACK、数据与奇偶校验；请复制完整日志。尚未访问 AP 或写入目标。");
    }
    private static void pause(int ms) throws IOException {
        try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException("Cancelled", e); }
    }
    static final class Bits {
        final byte[] direction = new byte[256];
        final byte[] data = new byte[256];
        int count;
        void out(long value, int n) {
            for (int i = 0; i < n; i++, count++) {
                direction[count >> 3] |= 1 << (count & 7);
                if (((value >>> i) & 1) != 0) data[count >> 3] |= 1 << (count & 7);
            }
        }
        void in(int n) { count += n; }
    }
    private byte[] exchange(Bits bits) throws IOException {
        int n = (bits.count + 7) / 8;
        byte[] packet = new byte[4 + 2 * n];
        packet[0] = (byte) 0xcf;
        packet[2] = (byte) bits.count;
        packet[3] = (byte) (bits.count >> 8);
        System.arraycopy(bits.direction, 0, packet, 4, n);
        System.arraycopy(bits.data, 0, packet, 4 + n, n);
        usb.write(packet);
        byte[] result = usb.read(n + 1);
        if (result[n] != 0) throw new IOException("J-Link SWD I/O status " + (result[n] & 255));
        return result;
    }
    static int bitField(byte[] bytes, int offset, int count) {
        int result = 0;
        for (int i = 0; i < count; i++) result |= ((bytes[(offset + i) >> 3] >>> ((offset + i) & 7)) & 1) << i;
        return result;
    }
    private void idleAfterAck() throws IOException {
        Bits b = new Bits(); b.in(1); b.out(0, 8); exchange(b);
    }
    /** Split ACK/data phases so WAIT never causes a speculative memory write. */
    int transfer(boolean isAp, boolean read, int address, int value) throws IOException {
        String context = String.format(Locale.ROOT, "%s %s reg=0x%02X SELECT=%s",
            isAp ? "AP" + ap : "DP", read ? "READ" : "WRITE", isAp ? (select & 0xf0) | (address & 12) : address & 12,
            select == -1 ? "unknown" : String.format(Locale.ROOT, "%08X", select));
        try { return transferRaw(isAp, read, address, value, context); }
        catch (IOException e) {
            ready = false; select = csw = -1;
            if (e instanceof SwdFault) throw e;
            throw new IOException(e.getMessage() + " [" + context + "]", e);
        }
    }
    private int transferRaw(boolean isAp, boolean read, int address, int value, String context) throws IOException {
        int request = 0x81 | (isAp ? 2 : 0) | (read ? 4 : 0) | ((address & 12) << 1);
        request |= (Integer.bitCount(request & 0x1e) & 1) << 5;
        for (int retry = 0; retry < 40; retry++) {
            Bits head = new Bits(); head.out(request, 8); head.in(4);
            byte[] header = exchange(head);
            int ack = bitField(header, ackOffset, 3);
            if (ack == 2) { idleAfterAck(); pause(1); continue; }
            if (ack != 1) {
                idleAfterAck();
                if (ack == 4) throw new SwdFault(context);
                throw new IOException("SWD ACK " + ack + ": check wiring and target power");
            }
            Bits tail = new Bits();
            if (read) {
                tail.in(34); tail.out(0, 8);
                byte[] data = exchange(tail);
                // J-Link's early input layout returns data bit 0 in the last header sample.
                // Carry that bit across the USB call boundary; shifting ACK alone loses data.
                int result = decodeRead(header, data, ackOffset);
                int parityOffset = ackOffset == 8 ? 31 : 32;
                if ((Integer.bitCount(result) & 1) != bitField(data, parityOffset, 1)) throw new IOException("SWD parity error");
                return result;
            }
            tail.in(1); tail.out(value & 0xffffffffL, 32); tail.out(Integer.bitCount(value) & 1, 1); tail.out(0, 8);
            exchange(tail);
            return 0;
        }
        throw new IOException("SWD WAIT timeout");
    }
    static int decodeRead(byte[] header, byte[] data, int offset) {
        return offset == 8 ? (bitField(data, 0, 31) << 1) | bitField(header, 11, 1) : bitField(data, 0, 32);
    }
    private void selectAp(int address) throws IOException {
        int desired = (ap << 24) | (address & 0xf0);
        if (select != desired) { transfer(false, false, 8, desired); select = desired; }
    }
    private int readAp(int address) throws IOException {
        selectAp(address);
        transfer(true, true, address, 0);
        return transfer(false, true, 12, 0); // posted AP read
    }
    private void writeAp(int address, int value) throws IOException {
        selectAp(address); transfer(true, false, address, value);
    }
    private void setupMemory(long address, int size) throws IOException {
        if (!ready) throw new IOException("Connect J-Link/SWD first");
        if ((size != 1 && size != 2 && size != 4) || address < 0 || address > 0xffffffffL || address % size != 0)
            throw new IOException("Invalid or unaligned memory access");
        int desired = 0x23000000 | (size == 4 ? 2 : size == 2 ? 1 : 0);
        if (csw != desired) {
            writeAp(0, desired);
            int actual = readAp(0);
            if ((actual & 7) != (desired & 7)) throw new IOException("AHB-AP does not support this access width");
            csw = desired;
        }
        writeAp(4, (int) address);
    }
    public long readMemory(long address, int size) throws IOException {
        setupMemory(address, size);
        long value = readAp(12) & 0xffffffffL;
        int ctrl = transfer(false, true, 4, 0);
        if ((ctrl & 0x22) != 0) { ready = false; throw new IOException("AHB memory read fault; reconnect"); }
        return (value >>> ((address & 3) * 8)) & (size == 4 ? 0xffffffffL : (1L << (size * 8)) - 1);
    }
    public void writeMemory(long address, int size, long value) throws IOException {
        setupMemory(address, size);
        writeAp(12, (int) (value << ((address & 3) * 8)));
        transfer(false, true, 12, 0); // complete posted write
        int ctrl = transfer(false, true, 4, 0);
        if ((ctrl & 0x22) != 0) { ready = false; throw new IOException("AHB memory write fault; reconnect"); }
    }
    public void resetCortexM() throws IOException {
        long aircr = readMemory(0xe000ed0cL, 4);
        writeMemory(0xe000ed0cL, 4, 0x05fa0004L | (aircr & 0x700));
        ready = false; select = csw = -1;
    }
    public boolean isReady() { return ready; }
    JlinkHss startHss(long address, int hz) throws IOException {
        JlinkHss.periodFor(hz);
        if (!JlinkHss.supportedRam(address)) throw new IOException("HSS 仅支持对齐的 H74x RAM 32 位变量");
        if (!ready || hss != null) throw new IOException("请先连接 J-Link，再启动 HSS");
        if (ap != 0 || dpId != 0x6ba02477 || apId != 0x84770001
            || !firmware.startsWith("J-Link Pro V4 compiled Sep 22 2022 15:00:37"))
            throw new IOException("HSS 实验版仅验证过 Pro V4 / 2022-09-22 固件与 STM32H74x M7 AP0");
        if (readMemory(0xe000ed00L,4) != 0x411fc271L)
            throw new IOException("HSS Cortex-M7 CPUID 与已验证配置不一致");
        hss = new JlinkHss(usb);
        ready=false; select=csw=-1; // HSS exclusively owns AP/DP state until close.
        hss.start(address,hz);
        trace("HSS 已启动：单变量，目标="+hz+" Hz，周期="+JlinkHss.periodFor(hz)+" us；停止后需重新连接");
        return hss;
    }
    @Override public void close() throws IOException {
        ready=false;
        try { if (hss != null) hss.stop(); }
        finally { hss=null; usb.close(); }
    }
}
