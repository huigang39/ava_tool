package com.ava.tool;
import java.io.*;
import java.util.*;

/** Independent SWD target model: validates direction, parity, posted AP reads and byte lanes. */
public final class ProtocolTest {
    static int assertions;
    static void check(boolean condition, String why) { assertions++; if (!condition) throw new AssertionError(why); }
    interface Checked { void run() throws Exception; }
    static void fails(Checked action, String text) throws Exception {
        try { action.run(); throw new AssertionError("Expected failure: " + text); }
        catch (IOException | IllegalArgumentException e) { check(e.getMessage().contains(text), e.toString()); }
    }
    static class Target implements Jlink.Transport {
        final ArrayDeque<Byte> reply = new ArrayDeque<>();
        final HashMap<Long, Integer> memory = new HashMap<>();
        int request, select, csw, tar, posted, waits, ackOverride, ioStatus;
        int widthSupported = 7, apId = 0x24770011, voltage = 3300, dpId = 0x2ba01477, readValue;
        boolean header, parityError, closed, stickyFault, earlySamples;
        int writes, idrFaults, idrRequests, abortClears;
        int dpWriteFault = -1;
        int discoveryReads, invalidDiscoveryReads, dpWrites;
        void bytes(int... values) { for (int x : values) reply.add((byte)x); }
        void word(long value) { bytes((int)value, (int)(value >> 8), (int)(value >> 16), (int)(value >> 24)); }
        static int get(byte[] a, int bit) { return (a[bit >> 3] >>> (bit & 7)) & 1; }
        static long bits(byte[] a, int offset, int n) { long v = 0; for (int i = 0; i < n; i++) v |= (long)get(a, offset + i) << i; return v; }
        static void put(byte[] a, int bit, long value, int n) { for (int i = 0; i < n; i++) if (((value >>> i) & 1) != 0) a[(bit+i)>>3] |= 1 << ((bit+i)&7); }
        int readRegister(boolean ap, int addr) {
            if (ap) {
                int value = posted, reg = (select & 0xf0) | addr;
                posted = reg == 0xfc ? apId : reg == 0 ? csw & ~7 | (csw & widthSupported) : reg == 12 ? memory.getOrDefault(tar & 0xfffffffcL,0) : 0;
                return value;
            }
            return addr==0 ? dpId : addr==4 ? 0xf0000000 | (stickyFault ? 0x20 : 0) : addr==12 ? posted : select;
        }
        @Override public void write(byte[] p) throws IOException {
            check(!closed, "closed transport"); check(reply.isEmpty(), "unconsumed response");
            switch (p[0] & 255) {
                case 1: bytes(5,0,'T','e','s','t',0); break;
                case 7: bytes(voltage, voltage >> 8,0,0,0,0,0,0); break;
                case 0xe8: word(1 << 17); break;
                case 0xc7: word(p[1] == -1 ? 3 : 0); break;
                case 5: check(p.length == 3, "speed frame"); break;
                case 0xcf: swd(p); break;
                default: throw new IOException("Unexpected command");
            }
        }
        void swd(byte[] p) throws IOException {
            int n = (p[2] & 255) | (p[3] & 255) << 8, length = (n+7)/8;
            byte[] dir = Arrays.copyOfRange(p,4,4+length), data = Arrays.copyOfRange(p,4+length,p.length), out = new byte[length+1];
            out[length] = (byte) ioStatus;
            if (n == 152) {
                check(bits(data,64,16) == 0xe79e, "SWJ switch");
                for (int i=0;i<n;i++) check(get(dir,i)==1,"SWJ direction");
                header=false;
            } else if (n == 72) {
                check(bits(data,0,64)==-1L && bits(data,64,8)==0,"line reset high then idle");
                for(int i=0;i<n;i++) check(get(dir,i)==1,"line reset output direction");
                header=false;
            } else if (n == 54) {
                // Full read-only ID transaction used before any register writes.
                check(bits(data,0,8)==0xa5,"discovery requests DPIDR only");
                check(bits(dir,0,8)==255 && bits(dir,8,38)==0 && bits(dir,46,8)==255,"discovery direction/turnarounds");
                discoveryReads++;
                int offset = earlySamples ? 8 : 9;
                if(invalidDiscoveryReads-- > 0) put(out,offset,7,3);
                else {
                    put(out,offset,1,3); put(out,offset+3,dpId & 0xffffffffL,32);
                    put(out,offset+35,(Integer.bitCount(dpId)&1) ^ (parityError?1:0),1);
                }
            } else if (n == 12 && !header) {
                request = (int)bits(data,0,8); check((request & 0xc1)==0x81,"SWD framing");
                check((Integer.bitCount(request & 0x3e) & 1)==0,"request parity");
                check(bits(dir,0,8)==255 && bits(dir,8,4)==0,"request turnaround");
                int ack = waits-- > 0 ? 2 : ackOverride != 0 ? ackOverride : 1;
                boolean ap = (request & 2) != 0, read = (request & 4) != 0;
                int addr = (request >> 1) & 12;
                if (ack == 1 && ap && read && ((select & 0xf0) | addr) == 0xfc) {
                    idrRequests++;
                    if (idrFaults > 0) { idrFaults--; stickyFault = true; }
                }
                if (ack == 1 && ((ap && stickyFault) || (!ap && !read && addr == dpWriteFault))) ack = 4;
                put(out,earlySamples ? 8 : 9,ack,3); header = ack == 1;
                if(header && read) {
                    readValue=readRegister(ap,addr);
                    if(earlySamples) put(out,11,readValue & 1,1);
                }
            } else if (n == 9 && !header) { check(get(dir,0)==0 && bits(dir,1,8)==255,"WAIT turnaround"); }
            else if (header) {
                boolean read = (request & 4)!=0, ap = (request & 2)!=0; int addr = (request >> 1) & 12;
                if (read) {
                    check(n==42 && bits(dir,0,34)==0 && bits(dir,34,8)==255,"read data phase");
                    int value = readValue;
                    if(earlySamples) put(out,0,(value & 0xffffffffL) >>> 1,31); else put(out,0,value & 0xffffffffL,32);
                    put(out,earlySamples ? 31 : 32,(Integer.bitCount(value)&1) ^ (parityError?1:0),1);
                } else {
                    check(n==42 && get(dir,0)==0 && bits(dir,1,41)==(1L<<41)-1,"write turnaround");
                    int value = (int)bits(data,1,32); check((Integer.bitCount(value)&1)==bits(data,33,1),"write parity");
                    if (ap) {
                        int reg = (select & 0xf0) | addr;
                        if (reg==0) csw=value; else if (reg==4) tar=value;
                        else if (reg==12) { int size=1 << (csw&7), shift=(tar&3)*8; long address=tar & 0xfffffffcL; int mask=size==4?-1:((1<<(size*8))-1)<<shift; memory.put(address, (memory.getOrDefault(address,0)&~mask)|(value&mask)); writes++; }
                    } else {
                        dpWrites++;
                        if (addr==8) select=value;
                        else if (addr==0 && (value & 4) != 0) { stickyFault=false; abortClears++; }
                    }
                }
                header=false;
            } else throw new IOException("Unexpected bit sequence " + n);
            for(byte b:out) reply.add(b);
        }
        @Override public byte[] read(int n) throws IOException { if(reply.size()<n)throw new IOException("Short read"); byte[] out=new byte[n];for(int i=0;i<n;i++)out[i]=reply.remove();return out; }
        @Override public void close(){closed=true;}
    }
    public static void main(String[] args) throws Exception {
        // Published physical capture, not synthesized by the target simulator:
        // https://jon.dev/traces/j-link-over-usb section 12.1 (EDU Mini V2).
        byte[] capturedHeader = {0x00, 0x09, 0x00};
        byte[] capturedData = {0x3b, (byte)0x8a, (byte)0xd8, (byte)0x85, 0x03, 0x00, 0x00};
        check(Jlink.bitField(capturedHeader,9,3)==4,"old decoder reproduces false FAULT from real OK capture");
        check(Jlink.bitField(capturedHeader,8,3)==1,"probe ACK starts at raw bit 8");
        check(Jlink.decodeRead(capturedHeader,capturedData,8)==0x0bb11477,"carry first data bit across USB scans");
        check((Integer.bitCount(0x0bb11477)&1)==Jlink.bitField(capturedData,31,1),"physical capture parity");
        Target early = new Target(); early.earlySamples=true; early.dpId=0x6ba02477;
        Jlink h7 = new Jlink(early); h7.connect(100,0);
        check(h7.dpId==0x6ba02477 && h7.isReady() && early.discoveryReads==2,"H7 early samples negotiate with two matching read-only IDs");
        for(int value : new int[]{0,1,2,0x80000000,0x80000001,0xffffffff,0x12345678}) {
            early.memory.put(0x20000000L,value);
            check(h7.readMemory(0x20000000L,4)==(value & 0xffffffffL),"early data layout bit-boundary read");
        }
        h7.writeMemory(0x20000001L,1,0xab);
        check(h7.readMemory(0x20000000L,4)==0x1234ab78L,"early ACK write and readback");
        early.waits=2; check(h7.readMemory(0x20000001L,1)==0xab,"early ACK WAIT handling");
        early.ackOverride=4; int written=early.writes;
        fails(()->h7.writeMemory(0x20000000L,4,42),"FAULT");
        check(!h7.isReady() && early.writes==written,"real early-layout FAULT must still stop writes");
        Target noSync = new Target(); noSync.invalidDiscoveryReads=10;
        fails(()->new Jlink(noSync).connect(100,0),"DPIDR 同步失败");
        check(noSync.discoveryReads==3 && noSync.dpWrites==0 && noSync.writes==0,"failed discovery never writes DP/AP or target");
        Target resync = new Target(); resync.invalidDiscoveryReads=1;
        new Jlink(resync).connect(100,0); check(resync.discoveryReads==3,"second line reset recovers initial sync failure");
        Target corrupt = new Target(); corrupt.earlySamples=true; corrupt.parityError=true;
        fails(()->new Jlink(corrupt).connect(100,0),"DPIDR 同步失败");
        check(corrupt.dpWrites==0,"parity-corrupt identification cannot authorize register writes");
        Target target = new Target(); Jlink j = new Jlink(target); j.connect(1000,0);
        check(j.isReady() && j.dpId==0x2ba01477 && j.apId==0x24770011,"DP/AP connect");
        target.memory.put(0x20000000L,0x12345678);
        check(j.readMemory(0x20000000L,4)==0x12345678,"32 bit read");
        check(j.readMemory(0x20000001L,1)==0x56,"byte lane");
        check(j.readMemory(0x20000002L,2)==0x1234,"halfword lane");
        j.writeMemory(0x20000001L,1,0xab); check(target.memory.get(0x20000000L)==0x1234ab78,"byte write preserves adjacent bytes");
        j.writeMemory(0x20000002L,2,0xcdef); check(target.memory.get(0x20000000L)==0xcdefab78,"halfword write");
        j.writeMemory(0x20000000L,4,0xffffffffL); check(j.readMemory(0x20000000L,4)==0xffffffffL,"unsigned word");
        target.waits=2; j.writeMemory(0x20000000L,4,42); check(target.memory.get(0x20000000L)==42,"WAIT retry");
        int before=target.writes; target.ackOverride=4; fails(()->j.writeMemory(0x20000000L,4,99),"FAULT");check(target.writes==before,"FAULT must not write");target.ackOverride=0;
        check(!j.isReady(),"FAULT invalidates connection");
        fails(()->j.writeMemory(0x20000000L,4,99),"Connect J-Link/SWD first");
        check(target.writes==before,"failed write is never replayed"); j.connect(1000,0);
        target.parityError=true; fails(()->j.readMemory(0x20000000L,4),"parity");target.parityError=false;
        check(!j.isReady(),"parity failure invalidates connection"); j.connect(1000,0);
        target.waits=100; fails(()->j.readMemory(0x20000000L,4),"WAIT timeout");target.waits=0;
        check(!j.isReady(),"WAIT timeout invalidates connection"); j.connect(1000,0);
        fails(()->j.readMemory(0x20000001L,4),"unaligned");fails(()->j.readMemory(0x100000000L,4),"unaligned");
        j.close();check(!j.isReady() && target.closed,"close state");
        Target low = new Target(); low.voltage=100; fails(()->new Jlink(low).connect(1000,0),"VTref");
        Target wrong = new Target();wrong.apId=0;fails(()->new Jlink(wrong).connect(1000,0),"not an ADIv5");
        Target status = new Target();status.ioStatus=6;fails(()->new Jlink(status).connect(1000,0),"status 6");
        Target transientFault = new Target(); transientFault.idrFaults=1;
        ArrayList<String> trace = new ArrayList<>();
        Jlink recovered = new Jlink(transientFault, trace::add); recovered.connect(1000,0);
        check(recovered.isReady() && transientFault.idrRequests==2,"retry identification once after sticky error");
        check(transientFault.abortClears==2 && transientFault.writes==0,"recovery clears only DP error, no memory writes");
        check(trace.stream().anyMatch(s->s.contains("DPIDR=2BA01477")),"DPIDR visible before connection completes");
        check(trace.stream().anyMatch(s->s.contains("AP0 READ reg=0xFC SELECT=000000F0")),"fault includes full AP register and SELECT");
        check(trace.stream().anyMatch(s->s.contains("F0000020")),"sticky status captured before clearing");
        Target persistentFault = new Target(); persistentFault.idrFaults=2;
        Jlink failed = new Jlink(persistentFault);
        fails(()->failed.connect(1000,0),"AP0 READ reg=0xFC");
        check(!failed.isReady() && persistentFault.idrRequests==2 && persistentFault.writes==0,"persistent FAULT stops after one identification retry");
        Target powerFault = new Target(); powerFault.dpWriteFault=4;
        fails(()->new Jlink(powerFault).connect(1000,0),"DP WRITE reg=0x04");
        check(powerFault.idrRequests==0,"power-stage failure is not mistaken for AP/address failure");
        check(Values.parse("u64","18446744073709551615")==-1L,"u64 parse");
        check(Values.format("u64",-1).equals("18446744073709551615"),"u64 format");
        check(Values.parse("i8","-128")==-128,"i8 parse");fails(()->Values.parse("i8","128"),"range");fails(()->Values.parse("u8","-1"),"range");
        check(Values.format("f32",Values.parse("f32","1.5")).equals("1.5"),"f32");
        fails(()->Values.parse("f32","NaN"),"finite");check(Values.address("0xE000ED00")==0xe000ed00L,"address");
        System.out.println("PASS: " + assertions + " protocol/value assertions (simulated target, not hardware)");
    }
}
