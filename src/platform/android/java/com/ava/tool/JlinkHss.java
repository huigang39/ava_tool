package com.ava.tool;

import java.io.IOException;

/** Narrow, trace-verified HSS profile: Pro V4 (2022-09-22), H74x M7/AP0.
 * Derived from the user's successful 2026-10-26 USB capture, not a vendor spec.
 * All methods run on the same I/O worker as Jlink. No MCU code is installed.
 */
final class JlinkHss {
    static final int MAX_BYTES = 8192;
    // Volatile probe CPU configuration, frame 359 of hss-session-02.pcap.
    static final String CPU_CONFIG = "c4000407ff00010e010801010900540aff00010e00000a0002000000000000000000f000000000e000003000000000000000000000ff0000000000000000000400000000000000000000000000000001000000000100000200000000000000030000000000";
    private final Jlink.Transport usb;
    private boolean running, synchronizedStream = true;
    JlinkHss(Jlink.Transport usb) { this.usb = usb; }
    static byte[] hex(String text) {
        byte[] b = new byte[text.length()/2];
        for (int i=0;i<b.length;i++) b[i]=(byte)Integer.parseInt(text.substring(i*2,i*2+2),16);
        return b;
    }
    static void put32(byte[] b, int offset, long value) {
        for (int i=0;i<4;i++) b[offset+i]=(byte)(value >>> (8*i));
    }
    static int periodFor(int hz) throws IOException {
        if (hz < 1000 || hz > 50000) throw new IOException("HSS 目标频率范围：1000..50000 Hz");
        return (1000000 + hz - 1) / hz;
    }
    static boolean supportedRam(long a) {
        return a % 4 == 0 && ((a >= 0x20000000L && a <= 0x2001fffcL)
            || (a >= 0x24000000L && a <= 0x2407fffcL)
            || (a >= 0x30000000L && a <= 0x30047ffcL)
            || (a >= 0x38000000L && a <= 0x3800fffcL));
    }
    static byte[] startPacket(long address, int periodUs) {
        byte[] p = new byte[32]; p[0]=(byte)0xc5; p[2]=14; p[3]=1;
        put32(p,4,1); put32(p,8,address); put32(p,12,4);
        put32(p,24,periodUs); put32(p,28,1); // timestamp in microseconds
        return p;
    }
    void start(long address, int hz) throws IOException {
        int period = periodFor(hz);
        if (!supportedRam(address)) throw new IOException("HSS 当前仅支持 H74x 对齐的片内 RAM 中的 32 位变量");
        if (running) throw new IOException("HSS already running");
        try {
            usb.write(hex(CPU_CONFIG));
            usb.write(new byte[]{(byte)0xc5,0,14,0});
            byte[] caps = usb.read(36);
            if (Jlink.u32(caps,0) != 0 || Jlink.u32(caps,4) < 1 || Jlink.u32(caps,8) < period)
                throw new IOException("探针 HSS 能力不符合已验证协议");
            usb.write(startPacket(address,period));
            int status=(int)Jlink.u32(usb.read(4),0);
            if (status != 0) throw new IOException("HSS 启动失败，状态="+status);
            running = true;
        } catch (IOException e) { synchronizedStream=false; throw e; }
    }
    byte[] read() throws IOException {
        if (!running || !synchronizedStream) throw new IOException("HSS 未启动或连接已失效");
        try {
            usb.write(new byte[]{(byte)0xc5,0,14,3,0,0x20,0,0});
            long n=Jlink.u32(usb.read(4),0);
            // Reject before allocation; never continue commands over an unframed reply.
            if (n > MAX_BYTES || n % 8 != 0) throw new IOException("HSS 数据长度异常："+n+"；请重新连接");
            return n == 0 ? new byte[0] : usb.read((int)n);
        } catch (IOException e) { synchronizedStream=false; throw e; }
    }
    void stop() throws IOException {
        if (!running) return;
        running=false;
        if (!synchronizedStream) return; // unread/unknown framing: close USB instead
        try {
            usb.write(new byte[]{(byte)0xc5,0,14,2});
            // Successful stop returns bytes remaining/discarded, not just zero.
            int status=(int)Jlink.u32(usb.read(4),0);
            if (status < 0) throw new IOException("HSS 停止失败："+status);
        } catch (IOException e) { synchronizedStream=false; throw e; }
    }
}
