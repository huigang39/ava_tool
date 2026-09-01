package com.ava.tool;

import android.hardware.usb.*;
import java.io.IOException;

/** Preserves over-read bytes (version headers and SWD status can share a USB packet). */
final class AndroidUsbTransport implements Jlink.Transport {
    private final UsbDeviceConnection connection;
    private final UsbInterface iface;
    private final UsbEndpoint in, out;
    private final byte[] buffer = new byte[16384];
    private int cursor, available;
    private boolean closed;
    AndroidUsbTransport(UsbManager manager, UsbDevice device, Jlink.Log log) throws IOException {
        UsbInterface selected = null;
        UsbEndpoint read = null, write = null;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            // Exclude the CDC UART interfaces of J-Link OB probes.
            if (candidate.getInterfaceClass() != UsbConstants.USB_CLASS_VENDOR_SPEC) continue;
            UsbEndpoint ri = null, wo = null;
            for (int e = 0; e < candidate.getEndpointCount(); e++) {
                UsbEndpoint endpoint = candidate.getEndpoint(e);
                if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) ri = endpoint; else wo = endpoint;
            }
            if (ri != null && wo != null) { selected = candidate; read = ri; write = wo; break; }
        }
        if (selected == null) throw new IOException("No J-Link vendor bulk USB interface");
        iface = selected; in = read; out = write;
        log.line("USB：打开设备，VID=" + device.getVendorId() + " PID=" + device.getProductId() + " interface=" + iface.getId());
        connection = manager.openDevice(device);
        if (connection == null) throw new IOException("USB permission missing or device disconnected");
        boolean claimed = false;
        try {
            log.line("USB：申请接口（不强制卸载系统驱动）");
            claimed = connection.claimInterface(iface, false);
            if (!claimed) throw new IOException("USB interface is busy or unavailable; no driver was forcibly detached");
        } finally { if (!claimed) connection.close(); }
        log.line("USB：接口已获得");
    }
    @Override public void write(byte[] data) throws IOException {
        if (closed) throw new IOException("USB closed");
        int n = connection.bulkTransfer(out, data, data.length, 2000);
        if (n != data.length) throw new IOException("USB write failed/short: " + n + "/" + data.length);
    }
    @Override public byte[] read(int length) throws IOException {
        if (closed || length < 0 || length > 16384) throw new IOException("Invalid USB read");
        byte[] result = new byte[length];
        int done = 0;
        while (done < length) {
            if (cursor == available) {
                available = connection.bulkTransfer(in, buffer, buffer.length, 2000);
                cursor = 0;
                if (available <= 0) throw new IOException("USB read timeout/disconnected: " + available);
            }
            int n = Math.min(length - done, available - cursor);
            System.arraycopy(buffer, cursor, result, done, n);
            cursor += n; done += n;
        }
        return result;
    }
    @Override public void close() {
        if (!closed) {
            closed = true;
            try { connection.releaseInterface(iface); } finally { connection.close(); }
        }
    }
}
