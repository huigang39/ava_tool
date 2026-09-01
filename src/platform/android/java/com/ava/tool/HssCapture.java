package com.ava.tool;

import java.io.IOException;

/** Bounded primitive ring; acquisition never posts a Runnable per sample. */
final class HssCapture {
    static final int CAPACITY = 200000;
    private final long[] times;
    private final int[] values;
    private int next, count;
    private long total, previous, elapsedUs, gapIntervals;
    private boolean havePrevious;
    final int periodUs;
    HssCapture(int periodUs) { this(periodUs, CAPACITY); }
    HssCapture(int periodUs, int capacity) {
        if (periodUs < 1 || capacity < 1) throw new IllegalArgumentException();
        this.periodUs=periodUs; times=new long[capacity]; values=new int[capacity];
    }
    synchronized void accept(byte[] data) throws IOException {
        if (data.length % 8 != 0) throw new IOException("HSS 帧不完整");
        for (int i=0;i<data.length;i+=8) {
            long timestamp=Jlink.u32(data,i);
            if (havePrevious) {
                long delta=(timestamp-previous)&0xffffffffL;
                if (delta==0 || delta>0x7fffffffL) throw new IOException("HSS 时间戳重复或倒退；停止采样");
                elapsedUs+=delta;
                if (delta > periodUs*3L/2) gapIntervals++;
            }
            previous=timestamp; havePrevious=true;
            times[next]=elapsedUs; values[next]=(int)Jlink.u32(data,i+4);
            next=(next+1)%times.length; count=Math.min(count+1,times.length); total++;
        }
    }
    synchronized Snapshot snapshot(int maxPoints) {
        if (maxPoints < 1) throw new IllegalArgumentException();
        int n=Math.min(count,maxPoints), oldest=(next-count+times.length)%times.length;
        long[] ts=new long[n]; int[] vs=new int[n];
        for(int i=0;i<n;i++) {
            int logical=n==1 ? count-1 : (int)((long)i*(count-1)/(n-1));
            int at=(oldest+logical)%times.length; ts[i]=times[at]; vs[i]=values[at];
        }
        return new Snapshot(ts,vs,total,count,gapIntervals,elapsedUs);
    }
    static final class Snapshot {
        final long[] times; final int[] values;
        final long total, gaps; final int retained; final double hz;
        Snapshot(long[] ts,int[] vs,long total,int count,long gaps,long elapsed) {
            times=ts; values=vs; this.total=total; retained=count; this.gaps=gaps;
            hz=elapsed>0 ? (total-1)*1e6/elapsed : 0;
        }
    }
}
