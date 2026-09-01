package com.ava.tool;

import java.io.*;
import java.util.concurrent.TimeUnit;

/** Explicit hardware lab, not run by the ordinary test suite. */
public final class HssProbeTest {
    static final class Bridge implements Jlink.Transport {
        final Process process;
        final DataInputStream input;
        final DataOutputStream output;
        boolean closed;
        Bridge(String dll, String serial) throws Exception {
            process = new ProcessBuilder("python", "tests/android/probe_usb_bridge.py", dll, serial)
                .redirectError(ProcessBuilder.Redirect.INHERIT).start();
            input = new DataInputStream(process.getInputStream());
            output = new DataOutputStream(process.getOutputStream());
            response();
        }
        byte[] response() throws IOException {
            int n = input.readInt();
            if (Math.abs((long)n) > 1048576) throw new IOException("Invalid bridge response");
            byte[] data = new byte[Math.abs(n)]; input.readFully(data);
            if (n < 0) throw new IOException(new String(data, "UTF-8"));
            return data;
        }
        public void write(byte[] data) throws IOException {
            output.writeByte('W'); output.writeInt(data.length); output.write(data); output.flush(); response();
        }
        public byte[] read(int n) throws IOException {
            output.writeByte('R'); output.writeInt(n); output.flush(); return response();
        }
        public void close() throws IOException {
            if (closed) return; closed = true;
            try { output.writeByte('Q'); output.flush(); }
            finally {
                output.close(); input.close();
                try {
                    if (!process.waitFor(5,TimeUnit.SECONDS)) { process.destroyForcibly(); throw new IOException("USB bridge did not close"); }
                    if (process.exitValue()!=0) throw new IOException("USB bridge exited with "+process.exitValue());
                } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
            }
        }
    }
    public static void main(String[] args) throws Exception {
        try (Bridge transport = new Bridge(args[0], args[1]); Jlink j = new Jlink(transport, System.out::println)) {
            long address=args.length>5 ? Long.decode(args[5]) : 0x20004bb4L;
            j.connect(args.length > 3 ? Integer.parseInt(args[3]) : 12000, 0);
            System.out.printf("CPUID=%08X address=%08X value=%08X%n", j.readMemory(0xe000ed00L, 4),address,j.readMemory(address, 4));
            int hz=Integer.parseInt(args[2]);
            HssCapture capture=new HssCapture(JlinkHss.periodFor(hz));
            JlinkHss hss=j.startHss(address,hz);
            long until=System.nanoTime()+(args.length>4 ? Long.parseLong(args[4]) : 3)*1_000_000_000L;
            while(System.nanoTime()<until) {
                Thread.sleep(5);
                capture.accept(hss.read());
            }
            HssCapture.Snapshot result=capture.snapshot(HssCapture.CAPACITY);
            System.out.printf("HSS requested=%d samples=%d measured=%.2f Hz gaps=%d retained=%d%n",hz,result.total,result.hz,result.gaps,result.retained);
            int changes=0; for(int i=1;i<result.values.length;i++)if(result.values[i]!=result.values[i-1])changes++;
            System.out.println("Retained value changes="+changes);
            if(result.total<1000 || result.hz<1000) throw new IOException("Insufficient hardware samples");
        }
    }
}
