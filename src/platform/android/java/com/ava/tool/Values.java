package com.ava.tool;
import java.math.BigInteger;

final class Values {
    static final String[] TYPES = {"f32", "f64", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    static int size(String type) {
        for (String t : TYPES) if (t.equals(type)) return Integer.parseInt(t.substring(1)) / 8;
        throw new IllegalArgumentException("Unsupported type: " + type);
    }
    static long address(String text) {
        String s = text.trim().replaceFirst("(?i)^0x", "");
        long address = Long.parseLong(s, 16);
        if (address < 0 || address > 0xffffffffL) throw new IllegalArgumentException("Address must be 0..FFFFFFFF");
        return address;
    }
    static long parse(String type, String value) {
        if (type.equals("f32")) {
            float f = Float.parseFloat(value);
            if (!Float.isFinite(f)) throw new IllegalArgumentException("Enter a finite number");
            return Float.floatToRawIntBits(f) & 0xffffffffL;
        }
        if (type.equals("f64")) {
            double d = Double.parseDouble(value);
            if (!Double.isFinite(d)) throw new IllegalArgumentException("Enter a finite number");
            return Double.doubleToRawLongBits(d);
        }
        String s = value.trim();
        BigInteger n = s.startsWith("0x") || s.startsWith("0X") ? new BigInteger(s.substring(2), 16) : new BigInteger(s);
        int bits = size(type) * 8;
        boolean signed = type.charAt(0) == 'i';
        BigInteger min = signed ? BigInteger.ONE.shiftLeft(bits - 1).negate() : BigInteger.ZERO;
        BigInteger max = BigInteger.ONE.shiftLeft(signed ? bits - 1 : bits).subtract(BigInteger.ONE);
        if (n.compareTo(min) < 0 || n.compareTo(max) > 0) throw new IllegalArgumentException("Value out of " + type + " range");
        return n.longValue();
    }
    static String format(String type, long bits) {
        switch (type) {
            case "f32": return Float.toString(Float.intBitsToFloat((int) bits));
            case "f64": return Double.toString(Double.longBitsToDouble(bits));
            case "i8": return Byte.toString((byte) bits);
            case "i16": return Short.toString((short) bits);
            case "i32": return Integer.toString((int) bits);
            case "i64": return Long.toString(bits);
            case "u8": return Long.toString(bits & 255);
            case "u16": return Long.toString(bits & 65535);
            case "u32": return Long.toString(bits & 0xffffffffL);
            case "u64": return Long.toUnsignedString(bits);
            default: throw new IllegalArgumentException(type);
        }
    }
    static double numeric(String type, long bits) { return Double.parseDouble(format(type, bits)); }
    static String csv(String value) { return "\"" + value.replace("\"", "\"\"") + "\""; }
}
