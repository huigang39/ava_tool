package com.ava.tool;

import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.hardware.usb.*;
import android.net.wifi.WifiManager;
import android.os.*;
import java.io.IOException;
import java.util.concurrent.*;
import org.json.JSONObject;

/** Owns the local USB link while sharing; Activity recreation never owns its lifetime. */
public final class PeerService extends Service {
    private static final Object hardwareLock = new Object();
    private static Jlink hardware;
    private static volatile UsbDevice device;
    private static long epoch = 1;
    static volatile boolean running;
    private static volatile boolean available;
    static volatile String status = "未共享";
    static volatile String pairingKey = "";
    private final ExecutorService lifetime = Executors.newSingleThreadExecutor();
    private PowerManager.WakeLock wake;
    private WifiManager.MulticastLock multicast;
    private boolean receiverRegistered;
    private final BroadcastReceiver detached = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            UsbDevice unplugged = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            UsbDevice current=device;
            if (current == null || !current.equals(unplugged)) return;
            status = "USB 已拔出，共享已停止"; available=false; stopSelf();
        }
    };
    static void adopt(Jlink link, UsbDevice usb) throws IOException {
        synchronized (hardwareLock) {
            if (running || hardware != null) throw new IOException("共享正在运行或关闭，请稍后重试");
            hardware=link;device=usb;epoch++;running=true;available=link.isReady();status="正在开启共享";
        }
    }
    static void abandon() {
        synchronized (hardwareLock) { if(hardware!=null)try{hardware.close();}catch(IOException ignored){}hardware=null;device=null;epoch++;running=false;available=false; }
    }
    static boolean ready() { return available; }
    static long readMemory(long address,int size) throws IOException {
        synchronized(hardwareLock){if(hardware==null)throw new IOException("本机 USB 未连接");return hardware.readMemory(address,size);}
    }
    // JNI callbacks use the same lock as local UI reads and the USB lifetime.
    public String peerDevice() {
        synchronized(hardwareLock){
            try {return new JSONObject().put("name",hardware==null?"USB J-Link":hardware.firmware).put("epoch",epoch).put("ready",hardware!=null&&hardware.isReady()).toString();}
            catch(Exception impossible){return "{}";}
        }
    }
    public boolean peerRead(long expected,long address,byte[] bytes) throws IOException {
        synchronized(hardwareLock){
            if(expected!=epoch||hardware==null||!hardware.isReady())return false;
            for(int i=0;i<bytes.length;) {int size=(address+i)%4==0&&bytes.length-i>=4?4:(address+i)%2==0&&bytes.length-i>=2?2:1;long value=hardware.readMemory(address+i,size);for(int n=0;n<size;n++)bytes[i+n]=(byte)(value>>(n*8));i+=size;}return true;
        }
    }
    public boolean peerWrite(long expected,long address,byte[] bytes) throws IOException {
        synchronized(hardwareLock){
            if(expected!=epoch||hardware==null||!hardware.isReady())return false;
            for(int i=0;i<bytes.length;) {int size=(address+i)%4==0&&bytes.length-i>=4?4:(address+i)%2==0&&bytes.length-i>=2?2:1;long value=0;for(int n=0;n<size;n++)value|=(bytes[i+n]&255L)<<(n*8);hardware.writeMemory(address+i,size,value);i+=size;}return true;
        }
    }
    public boolean peerReset(long expected) throws IOException {
        synchronized(hardwareLock){if(expected!=epoch||hardware==null)return false;try{hardware.resetCortexM();}finally{try{hardware.close();}finally{hardware=null;device=null;epoch++;available=false;}}return true;}
    }
    @Override public void onCreate() {
        super.onCreate();
        NotificationManager manager=(NotificationManager)getSystemService(NOTIFICATION_SERVICE);
        manager.createNotificationChannel(new NotificationChannel("device-sharing","设备共享",NotificationManager.IMPORTANCE_LOW));
        Intent open=new Intent(this,MainActivity.class);
        PendingIntent pending=PendingIntent.getActivity(this,0,open,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE);
        Notification notice=new Notification.Builder(this,"device-sharing").setSmallIcon(R.drawable.ic_connection)
            .setContentTitle("AvA Tool · 设备共享").setContentText("本机 J-Link 可由已配对设备访问，点击管理共享").setContentIntent(pending).setOngoing(true).build();
        if(Build.VERSION.SDK_INT>=29)startForeground(41,notice,ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);else startForeground(41,notice);
        wake=((PowerManager)getSystemService(POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"ava_tool:device-sharing");wake.acquire();
        WifiManager wifi=(WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);
        if(wifi!=null){multicast=wifi.createMulticastLock("ava_tool:discovery");multicast.setReferenceCounted(false);multicast.acquire();}
        IntentFilter filter=new IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if(Build.VERSION.SDK_INT>=33)registerReceiver(detached,filter,Context.RECEIVER_NOT_EXPORTED);else registerReceiver(detached,filter);
        receiverRegistered=true;
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId) {
        if(intent==null){stopSelf();return START_NOT_STICKY;}
        String key=intent.getStringExtra("key");boolean writes=intent.getBooleanExtra("writes",false);
        lifetime.execute(()->{
            try {if(!ready())throw new IOException("本机 USB 未连接");NativePeer.startServer(this,Build.MODEL,45991,key,writes);pairingKey=key;status="共享中 · 端口 45991";}
            catch(Exception e){status="共享失败："+e.getMessage();stopSelf();}
        });
        return START_NOT_STICKY;
    }
    @Override public android.os.IBinder onBind(Intent intent){return null;}
    @Override public void onDestroy(){
        if(receiverRegistered)unregisterReceiver(detached);
        lifetime.execute(()->{
            NativePeer.stopServer();abandon();pairingKey="";
            if(wake!=null&&wake.isHeld())wake.release();if(multicast!=null&&multicast.isHeld())multicast.release();
            if(!status.startsWith("共享失败")&&!status.startsWith("USB"))status="未共享";
        });lifetime.shutdown();super.onDestroy();
    }
}
