/* JceDifferential.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE JAVA SIDE of the cross-language differential. It prints the same
 * canonical stream the Lua reference driver prints, over the same recording
 * mock host, for the same manifest-derived cases in the same order.
 *
 * The answers are rendered INTO THE LUA VALUE MODEL, because Lua is the
 * reference implementation: `i:` an integer, `f:` a double's bit pattern,
 * `b:` a boolean, `s:` a string, `nil`, `t:[..]` a table. Two renderings are
 * read from the manifest rather than chosen here -- an entry with
 * `miss_value` renders its absent answer as that number (Lua pushes it; Java
 * returns null), and a value_return of a C string renders a null as "",
 * exactly as the Lua binding pushes "".
 *
 * WHAT THIS FORBIDS: the two surfaces disagreeing about the number of values,
 * the type of any value, the value itself, which host member was called, in
 * what order, or with which arguments. WHAT IT CANNOT SEE: a manifest
 * decision that is wrong in the same way on both sides -- both emitters read
 * the one manifest, so a mis-declared shape or capacity is invisible here and
 * is the differential's stated limit, not an oversight.
 */
package com.jce.script.diff;

import com.jce.script.JceScript;

/** Prints the canonical stream for every differential case. */
public final class JceDifferential {

    private JceDifferential() { }

    private static final StringBuilder SB = new StringBuilder();

    private static String i(long v) {
        return "i:" + Long.toString(v);
    }

    private static String f(double v) {
        return "f:" + Long.toString(Double.doubleToRawLongBits(v));
    }

    private static String b(boolean v) {
        return "b:" + (v ? "true" : "false");
    }

    private static String s(String v) {
        return "s:" + v;
    }

    /* Each element goes through i(), NOT Long.toString: the Lua reference's
     * canon() builds a table as t:[ .. num(v[i]) .. ], and num() tags every
     * element with its Lua type ('i:' an integer, 'f:' a double's bits). An
     * untagged element renders t:[100] against Lua's t:[i:100] and the
     * differential reports a difference in the HARNESS as though it were a
     * difference in the surface -- which is exactly how it was found. */
    private static String t(long[] v) {
        StringBuilder sb = new StringBuilder("t:[");
        for (int k = 0; k < v.length; k++) {
            if (k > 0) {
                sb.append(',');
            }
            sb.append(i(v[k]));
        }
        return sb.append(']').toString();
    }

    /* The stream goes to a FILE named by argv[0], never to stdout: -Xcheck:jni
     * writes its diagnostics to the JVM's own stdout, and a stream that is
     * compared byte for byte cannot share a channel with them. "\n" is
     * written explicitly because println uses the platform separator and this
     * repository is LF. */
    private static java.io.PrintStream OUT;

    private static void line(String text) {
        OUT.print(text);
        OUT.print("\n");
    }

    private static void emit(int index, String label, String result) {
        line("CASE " + index + " " + label);
        line("R " + result);
        String tr = JceDiffHost.trace();
        int from = 0;
        while (from < tr.length()) {
            int nl = tr.indexOf('\n', from);
            int end = nl < 0 ? tr.length() : nl;
            line("T " + tr.substring(from, end));
            if (nl < 0) {
                break;
            }
            from = nl + 1;
        }
    }

    public static void main(String[] argv) throws java.io.IOException {
        if (argv.length < 1) {
            System.err.println("usage: JceDifferential <stream-file>");
            System.exit(2);
        }
        OUT = new java.io.PrintStream(new java.io.FileOutputStream(argv[0]),
                                      false, java.nio.charset.StandardCharsets.UTF_8);
        int total = JceDiffHost.caseCount();
        for (int i = 0; i < total; i++) {
            runCase(i);
        }
        line("DONE " + total);
        OUT.flush();
        OUT.close();
    }

    /* ONE METHOD PER CASE, dispatched by a switch.  Inlining 150 cases into
     * runCase would put them in one method body, and a JVM method is capped
     * at 65535 bytes of bytecode -- a cap this file would cross silently as
     * the surface grows, failing at javac time with an error about a size
     * limit rather than about anything a reader of the manifest would
     * recognise. */
    private static void runCase(int index) {
        String label = JceDiffHost.caseLabel(index);
        switch (index) {
        case 0: c0(label); return;
        case 1: c1(label); return;
        case 2: c2(label); return;
        case 3: c3(label); return;
        case 4: c4(label); return;
        case 5: c5(label); return;
        case 6: c6(label); return;
        case 7: c7(label); return;
        case 8: c8(label); return;
        case 9: c9(label); return;
        case 10: c10(label); return;
        case 11: c11(label); return;
        case 12: c12(label); return;
        case 13: c13(label); return;
        case 14: c14(label); return;
        case 15: c15(label); return;
        case 16: c16(label); return;
        case 17: c17(label); return;
        case 18: c18(label); return;
        case 19: c19(label); return;
        case 20: c20(label); return;
        case 21: c21(label); return;
        case 22: c22(label); return;
        case 23: c23(label); return;
        case 24: c24(label); return;
        case 25: c25(label); return;
        case 26: c26(label); return;
        case 27: c27(label); return;
        case 28: c28(label); return;
        case 29: c29(label); return;
        case 30: c30(label); return;
        case 31: c31(label); return;
        case 32: c32(label); return;
        case 33: c33(label); return;
        case 34: c34(label); return;
        case 35: c35(label); return;
        case 36: c36(label); return;
        case 37: c37(label); return;
        case 38: c38(label); return;
        case 39: c39(label); return;
        case 40: c40(label); return;
        case 41: c41(label); return;
        case 42: c42(label); return;
        case 43: c43(label); return;
        case 44: c44(label); return;
        case 45: c45(label); return;
        case 46: c46(label); return;
        case 47: c47(label); return;
        case 48: c48(label); return;
        case 49: c49(label); return;
        case 50: c50(label); return;
        case 51: c51(label); return;
        case 52: c52(label); return;
        case 53: c53(label); return;
        case 54: c54(label); return;
        case 55: c55(label); return;
        case 56: c56(label); return;
        case 57: c57(label); return;
        case 58: c58(label); return;
        case 59: c59(label); return;
        case 60: c60(label); return;
        case 61: c61(label); return;
        case 62: c62(label); return;
        case 63: c63(label); return;
        case 64: c64(label); return;
        case 65: c65(label); return;
        case 66: c66(label); return;
        case 67: c67(label); return;
        case 68: c68(label); return;
        case 69: c69(label); return;
        case 70: c70(label); return;
        case 71: c71(label); return;
        case 72: c72(label); return;
        case 73: c73(label); return;
        case 74: c74(label); return;
        case 75: c75(label); return;
        case 76: c76(label); return;
        case 77: c77(label); return;
        case 78: c78(label); return;
        case 79: c79(label); return;
        case 80: c80(label); return;
        case 81: c81(label); return;
        case 82: c82(label); return;
        case 83: c83(label); return;
        case 84: c84(label); return;
        case 85: c85(label); return;
        case 86: c86(label); return;
        case 87: c87(label); return;
        case 88: c88(label); return;
        case 89: c89(label); return;
        case 90: c90(label); return;
        case 91: c91(label); return;
        case 92: c92(label); return;
        case 93: c93(label); return;
        case 94: c94(label); return;
        case 95: c95(label); return;
        case 96: c96(label); return;
        case 97: c97(label); return;
        case 98: c98(label); return;
        case 99: c99(label); return;
        case 100: c100(label); return;
        case 101: c101(label); return;
        case 102: c102(label); return;
        case 103: c103(label); return;
        case 104: c104(label); return;
        case 105: c105(label); return;
        case 106: c106(label); return;
        case 107: c107(label); return;
        case 108: c108(label); return;
        case 109: c109(label); return;
        case 110: c110(label); return;
        case 111: c111(label); return;
        case 112: c112(label); return;
        case 113: c113(label); return;
        case 114: c114(label); return;
        case 115: c115(label); return;
        case 116: c116(label); return;
        case 117: c117(label); return;
        case 118: c118(label); return;
        case 119: c119(label); return;
        case 120: c120(label); return;
        case 121: c121(label); return;
        case 122: c122(label); return;
        case 123: c123(label); return;
        case 124: c124(label); return;
        case 125: c125(label); return;
        case 126: c126(label); return;
        case 127: c127(label); return;
        case 128: c128(label); return;
        case 129: c129(label); return;
        case 130: c130(label); return;
        case 131: c131(label); return;
        case 132: c132(label); return;
        case 133: c133(label); return;
        case 134: c134(label); return;
        case 135: c135(label); return;
        case 136: c136(label); return;
        case 137: c137(label); return;
        case 138: c138(label); return;
        case 139: c139(label); return;
        case 140: c140(label); return;
        case 141: c141(label); return;
        case 142: c142(label); return;
        case 143: c143(label); return;
        case 144: c144(label); return;
        case 145: c145(label); return;
        case 146: c146(label); return;
        case 147: c147(label); return;
        case 148: c148(label); return;
        case 149: c149(label); return;
        case 150: c150(label); return;
        case 151: c151(label); return;
        case 152: c152(label); return;
        case 153: c153(label); return;
        case 154: c154(label); return;
        case 155: c155(label); return;
        case 156: c156(label); return;
        case 157: c157(label); return;
        case 158: c158(label); return;
        case 159: c159(label); return;
        case 160: c160(label); return;
        case 161: c161(label); return;
        case 162: c162(label); return;
        case 163: c163(label); return;
        case 164: c164(label); return;
        case 165: c165(label); return;
        case 166: c166(label); return;
        case 167: c167(label); return;
        case 168: c168(label); return;
        case 169: c169(label); return;
        case 170: c170(label); return;
        case 171: c171(label); return;
        case 172: c172(label); return;
        case 173: c173(label); return;
        case 174: c174(label); return;
        case 175: c175(label); return;
        case 176: c176(label); return;
        case 177: c177(label); return;
        case 178: c178(label); return;
        case 179: c179(label); return;
        case 180: c180(label); return;
        case 181: c181(label); return;
        case 182: c182(label); return;
        case 183: c183(label); return;
        case 184: c184(label); return;
        case 185: c185(label); return;
        case 186: c186(label); return;
        case 187: c187(label); return;
        case 188: c188(label); return;
        case 189: c189(label); return;
        case 190: c190(label); return;
        case 191: c191(label); return;
        case 192: c192(label); return;
        case 193: c193(label); return;
        case 194: c194(label); return;
        case 195: c195(label); return;
        case 196: c196(label); return;
        case 197: c197(label); return;
        case 198: c198(label); return;
        case 199: c199(label); return;
        case 200: c200(label); return;
        case 201: c201(label); return;
        case 202: c202(label); return;
        case 203: c203(label); return;
        case 204: c204(label); return;
        case 205: c205(label); return;
        case 206: c206(label); return;
        case 207: c207(label); return;
        case 208: c208(label); return;
        case 209: c209(label); return;
        case 210: c210(label); return;
        case 211: c211(label); return;
        case 212: c212(label); return;
        case 213: c213(label); return;
        case 214: c214(label); return;
        default:
            throw new IllegalStateException("no case " + index);
        }
    }

    /* get_position */
    private static void c0(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 0 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getPosition(78L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(0, label, r);
        }
    }

    /* set_position */
    private static void c1(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 1 " + label);
                line("R VMFAIL");
                return;
            }
            s.setPosition(80L, 106.375f, 1674.125f, 2181.125f);
            String r = "0|";
            emit(1, label, r);
        }
    }

    /* get_rotation */
    private static void c2(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 2 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getRotation(89L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(2, label, r);
        }
    }

    /* set_rotation */
    private static void c3(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 3 " + label);
                line("R VMFAIL");
                return;
            }
            s.setRotation(91L, 761.75f, 2123.25f, 578.125f);
            String r = "0|";
            emit(3, label, r);
        }
    }

    /* get_scale */
    private static void c4(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 4 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getScale(29L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(4, label, r);
        }
    }

    /* get_world_position */
    private static void c5(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 5 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getWorldPosition(48L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(5, label, r);
        }
    }

    /* set_scale */
    private static void c6(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 6 " + label);
                line("R VMFAIL");
                return;
            }
            s.setScale(12L, 598.0f, 706.5f, 313.625f);
            String r = "0|";
            emit(6, label, r);
        }
    }

    /* set_parent */
    private static void c7(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 7 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.setParent(45L, 48L, true);
            String r = "1|" + b(v);
            emit(7, label, r);
        }
    }

    /* set_parent [strict preserve_world] */
    private static void c8(String label) {
        line("CASE 8 " + label);
        line("R SKIPPED strict-preserve_world-is-a-compile-error-in-java");
    }

    /* get_parent */
    private static void c9(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 9 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.getParent(6L);
            String r = "1|" + i((long) v);
            emit(9, label, r);
        }
    }

    /* is_key_down */
    private static void c10(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 10 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isKeyDown(48);
            String r = "1|" + b(v);
            emit(10, label, r);
        }
    }

    /* find_with_tag */
    private static void c11(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 11 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.findWithTag("find_with_tag_tag");
            String r = "1|" + i((long) v);
            emit(11, label, r);
        }
    }

    /* destroy */
    private static void c12(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 12 " + label);
                line("R VMFAIL");
                return;
            }
            s.destroy(77L);
            String r = "0|";
            emit(12, label, r);
        }
    }

    /* spawn */
    private static void c13(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 13 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.spawn("spawn_prefab_path", 1196.375f, 1714.625f, 601.25f);
            String r = "1|" + i((long) v);
            emit(13, label, r);
        }
    }

    /* spawn [defaults] */
    private static void c14(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 14 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.spawn("spawn_prefab_path");
            String r = "1|" + i((long) v);
            emit(14, label, r);
        }
    }

    /* move_axis */
    private static void c15(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 15 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.moveAxis();
            String r = "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(15, label, r);
        }
    }

    /* jump_pressed */
    private static void c16(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 16 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.jumpPressed();
            String r = "1|" + b(v);
            emit(16, label, r);
        }
    }

    /* sprint */
    private static void c17(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 17 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.sprint();
            String r = "1|" + b(v);
            emit(17, label, r);
        }
    }

    /* attack_pressed */
    private static void c18(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 18 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.attackPressed();
            String r = "1|" + b(v);
            emit(18, label, r);
        }
    }

    /* set_time_scale */
    private static void c19(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 19 " + label);
                line("R VMFAIL");
                return;
            }
            s.setTimeScale(254.5f);
            String r = "0|";
            emit(19, label, r);
        }
    }

    /* pause */
    private static void c20(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 20 " + label);
                line("R VMFAIL");
                return;
            }
            s.pause(true);
            String r = "0|";
            emit(20, label, r);
        }
    }

    /* pause [defaults] */
    private static void c21(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 21 " + label);
                line("R VMFAIL");
                return;
            }
            s.pause();
            String r = "0|";
            emit(21, label, r);
        }
    }

    /* shake_camera */
    private static void c22(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 22 " + label);
                line("R VMFAIL");
                return;
            }
            s.shakeCamera(1814.625f);
            String r = "0|";
            emit(22, label, r);
        }
    }

    /* shake_camera [defaults] */
    private static void c23(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 23 " + label);
                line("R VMFAIL");
                return;
            }
            s.shakeCamera();
            String r = "0|";
            emit(23, label, r);
        }
    }

    /* music_set_intensity */
    private static void c24(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 24 " + label);
                line("R VMFAIL");
                return;
            }
            s.musicSetIntensity(2273.5f);
            String r = "0|";
            emit(24, label, r);
        }
    }

    /* music_get_intensity */
    private static void c25(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 25 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.musicGetIntensity();
            String r = "1|" + f((double) v);
            emit(25, label, r);
        }
    }

    /* music_request_transition */
    private static void c26(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 26 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.musicRequestTransition(13);
            String r = "1|" + f((double) v);
            emit(26, label, r);
        }
    }

    /* gas_activate */
    private static void c27(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 27 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.gasActivate(64L, 81);
            String r = "1|" + b(v);
            emit(27, label, r);
        }
    }

    /* gas_get */
    private static void c28(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 28 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.gasGet(24L, "gas_get_attr_name");
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(28, label, r);
        }
    }

    /* gas_apply */
    private static void c29(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 29 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.gasApply(2L, "gas_apply_attr_name", 52, 2178.75f, 833.75f);
            String r = "1|" + b(v);
            emit(29, label, r);
        }
    }

    /* gas_apply [defaults] */
    private static void c30(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 30 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.gasApply(2L, "gas_apply_attr_name", 0, 2178.75f);
            String r = "1|" + b(v);
            emit(30, label, r);
        }
    }

    /* raycast */
    private static void c31(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 31 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.RaycastResult v = s.raycast(new float[] {2443.375f, 2492.875f, 311.5f}, new float[] {1408.625f, 2363.5f, 295.375f}, 1438.5f);
            String r = v == null ? "1|" + i(0) : "8|" + i((long) v.entity) + "|" + f((double) v.point[0]) + "|" + f((double) v.point[1]) + "|" + f((double) v.point[2]) + "|" + f((double) v.normal[0]) + "|" + f((double) v.normal[1]) + "|" + f((double) v.normal[2]) + "|" + f((double) v.distance);
            emit(31, label, r);
        }
    }

    /* raycast_filtered */
    private static void c32(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 32 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.RaycastFilteredResult v = s.raycastFiltered(new float[] {2080.5f, 1822.5f, 1165.25f}, new float[] {2419.875f, 1239.875f, 2342.375f}, 245.25f, 71, false);
            String r = v == null ? "1|" + i(0) : "8|" + i((long) v.entity) + "|" + f((double) v.point[0]) + "|" + f((double) v.point[1]) + "|" + f((double) v.point[2]) + "|" + f((double) v.normal[0]) + "|" + f((double) v.normal[1]) + "|" + f((double) v.normal[2]) + "|" + f((double) v.distance);
            emit(32, label, r);
        }
    }

    /* raycast_filtered [defaults] */
    private static void c33(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 33 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.RaycastFilteredResult v = s.raycastFiltered(new float[] {2080.5f, 1822.5f, 1165.25f}, new float[] {2419.875f, 1239.875f, 2342.375f}, 245.25f);
            String r = v == null ? "1|" + i(0) : "8|" + i((long) v.entity) + "|" + f((double) v.point[0]) + "|" + f((double) v.point[1]) + "|" + f((double) v.point[2]) + "|" + f((double) v.normal[0]) + "|" + f((double) v.normal[1]) + "|" + f((double) v.normal[2]) + "|" + f((double) v.distance);
            emit(33, label, r);
        }
    }

    /* raycast_all */
    private static void c34(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 34 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.raycastAll(new float[] {724.875f, 90.5f, 1886.25f}, new float[] {2205.0f, 1525.625f, 1440.625f}, 2482.0f, 31, false);
            String r = "1|" + t(v);
            emit(34, label, r);
        }
    }

    /* raycast_all [defaults] */
    private static void c35(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 35 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.raycastAll(new float[] {724.875f, 90.5f, 1886.25f}, new float[] {2205.0f, 1525.625f, 1440.625f}, 2482.0f);
            String r = "1|" + t(v);
            emit(35, label, r);
        }
    }

    /* apply_impulse */
    private static void c36(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 36 " + label);
                line("R VMFAIL");
                return;
            }
            s.applyImpulse(88L, 583.25f, 1706.75f, 1721.25f);
            String r = "0|";
            emit(36, label, r);
        }
    }

    /* set_velocity */
    private static void c37(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 37 " + label);
                line("R VMFAIL");
                return;
            }
            s.setVelocity(46L, 2132.75f, 881.375f, 1655.125f);
            String r = "0|";
            emit(37, label, r);
        }
    }

    /* anim_set_float */
    private static void c38(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 38 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetFloat(79L, "anim_set_float_name", 2114.75f);
            String r = "0|";
            emit(38, label, r);
        }
    }

    /* anim_set_int */
    private static void c39(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 39 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetInt(49L, "anim_set_int_name", 78);
            String r = "0|";
            emit(39, label, r);
        }
    }

    /* anim_set_bool */
    private static void c40(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 40 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetBool(47L, "anim_set_bool_name", false);
            String r = "0|";
            emit(40, label, r);
        }
    }

    /* anim_set_trigger */
    private static void c41(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 41 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetTrigger(23L, "anim_set_trigger_name");
            String r = "0|";
            emit(41, label, r);
        }
    }

    /* is_action_down */
    private static void c42(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 42 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isActionDown("is_action_down_name");
            String r = "1|" + b(v);
            emit(42, label, r);
        }
    }

    /* is_action_pressed */
    private static void c43(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 43 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isActionPressed("is_action_pressed_name");
            String r = "1|" + b(v);
            emit(43, label, r);
        }
    }

    /* get_axis */
    private static void c44(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 44 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.getAxis("get_axis_name");
            String r = "1|" + f((double) v);
            emit(44, label, r);
        }
    }

    /* get_pointer_delta */
    private static void c45(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 45 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getPointerDelta();
            String r = "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(45, label, r);
        }
    }

    /* get_pointer_wheel */
    private static void c46(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 46 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.getPointerWheel();
            String r = "1|" + f((double) v);
            emit(46, label, r);
        }
    }

    /* is_pointer_down */
    private static void c47(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 47 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isPointerDown(1);
            String r = "1|" + b(v);
            emit(47, label, r);
        }
    }

    /* get_touch_count */
    private static void c48(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 48 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.getTouchCount();
            String r = "1|" + i((long) v);
            emit(48, label, r);
        }
    }

    /* get_touch */
    private static void c49(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 49 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.GetTouchResult v = s.getTouch(1);
            String r = v == null ? "1|nil" : "4|" + i((long) v.id) + "|" + f((double) v.x) + "|" + f((double) v.y) + "|" + f((double) v.pressure);
            emit(49, label, r);
        }
    }

    /* get_touch [index below base] */
    private static void c50(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 50 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.GetTouchResult v = s.getTouch(0);
            String r = v == null ? "1|nil" : "4|" + i((long) v.id) + "|" + f((double) v.x) + "|" + f((double) v.y) + "|" + f((double) v.pressure);
            emit(50, label, r);
        }
    }

    /* tr */
    private static void c51(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 51 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.tr("tr_key");
            String r = "1|" + s(v);
            emit(51, label, r);
        }
    }

    /* get_locale */
    private static void c52(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 52 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.getLocale();
            String r = "1|" + s(v);
            emit(52, label, r);
        }
    }

    /* set_locale */
    private static void c53(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 53 " + label);
                line("R VMFAIL");
                return;
            }
            s.setLocale("set_locale_locale");
            String r = "0|";
            emit(53, label, r);
        }
    }

    /* get_velocity */
    private static void c54(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 54 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getVelocity(44L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(54, label, r);
        }
    }

    /* vehicle_set_input */
    private static void c55(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 55 " + label);
                line("R VMFAIL");
                return;
            }
            s.vehicleSetInput(13L, 2206.0f, 2181.875f, 914.5f);
            String r = "0|";
            emit(55, label, r);
        }
    }

    /* vehicle_get_speed */
    private static void c56(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 56 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.vehicleGetSpeed(51L);
            String r = "1|" + f((double) v);
            emit(56, label, r);
        }
    }

    /* get_move */
    private static void c57(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 57 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getMove();
            String r = "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(57, label, r);
        }
    }

    /* ui_get_slider */
    private static void c58(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 58 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.uiGetSlider(74L);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(58, label, r);
        }
    }

    /* ui_set_slider */
    private static void c59(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 59 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetSlider(16L, 2225.5f);
            String r = "0|";
            emit(59, label, r);
        }
    }

    /* ui_get_progress */
    private static void c60(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 60 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.uiGetProgress(9L);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(60, label, r);
        }
    }

    /* ui_set_progress */
    private static void c61(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 61 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetProgress(10L, 198.5f);
            String r = "0|";
            emit(61, label, r);
        }
    }

    /* ui_get_toggle */
    private static void c62(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 62 " + label);
                line("R VMFAIL");
                return;
            }
            Boolean v = s.uiGetToggle(75L);
            String r = v == null ? "1|nil" : "1|" + b(v.booleanValue());
            emit(62, label, r);
        }
    }

    /* ui_set_toggle */
    private static void c63(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 63 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetToggle(17L, false);
            String r = "0|";
            emit(63, label, r);
        }
    }

    /* ui_set_text */
    private static void c64(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 64 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetText(84L, "ui_set_text_txt");
            String r = "0|";
            emit(64, label, r);
        }
    }

    /* send_message */
    private static void c65(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 65 " + label);
                line("R VMFAIL");
                return;
            }
            s.sendMessage(8L, "send_message_msg", 776.875, "send_message_str_arg");
            String r = "0|";
            emit(65, label, r);
        }
    }

    /* send_message [defaults] */
    private static void c66(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 66 " + label);
                line("R VMFAIL");
                return;
            }
            s.sendMessage(8L, "send_message_msg");
            String r = "0|";
            emit(66, label, r);
        }
    }

    /* broadcast */
    private static void c67(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 67 " + label);
                line("R VMFAIL");
                return;
            }
            s.broadcast("broadcast_msg", 1055.625, "broadcast_str_arg");
            String r = "0|";
            emit(67, label, r);
        }
    }

    /* broadcast [defaults] */
    private static void c68(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 68 " + label);
                line("R VMFAIL");
                return;
            }
            s.broadcast("broadcast_msg");
            String r = "0|";
            emit(68, label, r);
        }
    }

    /* has_component */
    private static void c69(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 69 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.hasComponent(40L, "has_component_comp_name");
            String r = "1|" + b(v);
            emit(69, label, r);
        }
    }

    /* is_component_enabled */
    private static void c70(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 70 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isComponentEnabled(41L, "is_component_enabled_comp_name");
            String r = "1|" + b(v);
            emit(70, label, r);
        }
    }

    /* set_component_enabled */
    private static void c71(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 71 " + label);
                line("R VMFAIL");
                return;
            }
            s.setComponentEnabled(48L, "set_component_enabled_comp_name", true);
            String r = "0|";
            emit(71, label, r);
        }
    }

    /* net_is_server */
    private static void c72(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 72 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.netIsServer();
            String r = "1|" + b(v);
            emit(72, label, r);
        }
    }

    /* net_is_client */
    private static void c73(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 73 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.netIsClient();
            String r = "1|" + b(v);
            emit(73, label, r);
        }
    }

    /* net_spawn */
    private static void c74(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 74 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.netSpawn("net_spawn_prefab_path", 301.375f, 1501.625f, 1861.125f);
            String r = "1|" + i((long) v);
            emit(74, label, r);
        }
    }

    /* rpc_send */
    private static void c75(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 75 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.rpcSend(23L, "rpc_send_event", 39, "rpc_send_payload");
            String r = "1|" + b(v);
            emit(75, label, r);
        }
    }

    /* rpc_send [defaults] */
    private static void c76(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 76 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.rpcSend(23L, "rpc_send_event");
            String r = "1|" + b(v);
            emit(76, label, r);
        }
    }

    /* particle_burst */
    private static void c77(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 77 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleBurst(36L, 53);
            String r = "0|";
            emit(77, label, r);
        }
    }

    /* particle_set_emitting */
    private static void c78(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 78 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleSetEmitting(41L, true);
            String r = "0|";
            emit(78, label, r);
        }
    }

    /* particle_set_color */
    private static void c79(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 79 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleSetColor(76L, 1588.75f, 2102.125f, 1560.25f);
            String r = "0|";
            emit(79, label, r);
        }
    }

    /* find_by_name */
    private static void c80(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 80 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.FindByNameResult v = s.findByName("find_by_name_name");
            String first = v.first == null ? "nil" : i(v.first.longValue());
            String r = "2|" + first + "|" + i(v.count);
            emit(80, label, r);
        }
    }

    /* find_by_prefix */
    private static void c81(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 81 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.findByPrefix("find_by_prefix_prefix");
            String r = "1|" + t(v);
            emit(81, label, r);
        }
    }

    /* comp_get */
    private static void c82(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 82 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.compGet(26L, "comp_get_type");
            String r = v == null ? "1|nil" : "1|" + s(v);
            emit(82, label, r);
        }
    }

    /* comp_set */
    private static void c83(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 83 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.compSet(97L, "comp_set_type", "comp_set_json");
            String r = "1|" + b(v);
            emit(83, label, r);
        }
    }

    /* render_get */
    private static void c84(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 84 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.renderGet();
            String r = v == null ? "1|nil" : "1|" + s(v);
            emit(84, label, r);
        }
    }

    /* render_set */
    private static void c85(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 85 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.renderSet("render_set_json");
            String r = "1|" + b(v);
            emit(85, label, r);
        }
    }

    /* audio_set_volume */
    private static void c86(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 86 " + label);
                line("R VMFAIL");
                return;
            }
            s.audioSetVolume(55L, 966.25f);
            String r = "0|";
            emit(86, label, r);
        }
    }

    /* ui_get_dropdown */
    private static void c87(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 87 " + label);
                line("R VMFAIL");
                return;
            }
            Integer v = s.uiGetDropdown(37L);
            String r = v == null ? "1|nil" : "1|" + i(v.longValue());
            emit(87, label, r);
        }
    }

    /* ui_set_dropdown */
    private static void c88(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 88 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetDropdown(39L, 22);
            String r = "0|";
            emit(88, label, r);
        }
    }

    /* ui_get_input_text */
    private static void c89(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 89 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.uiGetInputText(47L);
            String r = "1|" + s(v);
            emit(89, label, r);
        }
    }

    /* ui_set_input_text */
    private static void c90(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 90 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetInputText(41L, "ui_set_input_text_text");
            String r = "0|";
            emit(90, label, r);
        }
    }

    /* ui_get_scroll */
    private static void c91(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 91 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.uiGetScroll(70L);
            String r = v == null ? "1|nil" : "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(91, label, r);
        }
    }

    /* ui_set_scroll */
    private static void c92(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 92 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetScroll(18L, 728.375f, 1564.75f);
            String r = "0|";
            emit(92, label, r);
        }
    }

    /* world_get_hour */
    private static void c93(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 93 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetHour();
            String r = "1|" + f((double) v);
            emit(93, label, r);
        }
    }

    /* world_set_hour */
    private static void c94(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 94 " + label);
                line("R VMFAIL");
                return;
            }
            s.worldSetHour(1754.875f);
            String r = "0|";
            emit(94, label, r);
        }
    }

    /* world_is_daytime */
    private static void c95(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 95 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.worldIsDaytime();
            String r = "1|" + b(v);
            emit(95, label, r);
        }
    }

    /* world_get_weather */
    private static void c96(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 96 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.worldGetWeather();
            String r = "1|" + i((long) v);
            emit(96, label, r);
        }
    }

    /* world_get_weather_intensity */
    private static void c97(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 97 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetWeatherIntensity();
            String r = "1|" + f((double) v);
            emit(97, label, r);
        }
    }

    /* world_get_wind_speed */
    private static void c98(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 98 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetWindSpeed();
            String r = "1|" + f((double) v);
            emit(98, label, r);
        }
    }

    /* request_scene */
    private static void c99(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 99 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.requestScene("request_scene_scene_path");
            String r = "1|" + b(v);
            emit(99, label, r);
        }
    }

    /* is_transitioning */
    private static void c100(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 100 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isTransitioning();
            String r = "1|" + b(v);
            emit(100, label, r);
        }
    }

    /* audio_play */
    private static void c101(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 101 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioPlay(72L);
            String r = "1|" + b(v);
            emit(101, label, r);
        }
    }

    /* audio_stop */
    private static void c102(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 102 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioStop(79L);
            String r = "1|" + b(v);
            emit(102, label, r);
        }
    }

    /* audio_is_playing */
    private static void c103(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 103 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioIsPlaying(86L);
            String r = "1|" + b(v);
            emit(103, label, r);
        }
    }

    /* save_game */
    private static void c104(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 104 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.saveGame("save_game_path");
            String r = "1|" + b(v);
            emit(104, label, r);
        }
    }

    /* load_game */
    private static void c105(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 105 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.loadGame("load_game_path");
            String r = "1|" + b(v);
            emit(105, label, r);
        }
    }

    /* overlap_sphere */
    private static void c106(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 106 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapSphere(1035.5f, 207.625f, 1063.125f, 2067.5f, 32);
            String r = "1|" + t(v);
            emit(106, label, r);
        }
    }

    /* overlap_sphere [defaults] */
    private static void c107(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 107 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapSphere(1035.5f, 207.625f, 1063.125f, 2067.5f);
            String r = "1|" + t(v);
            emit(107, label, r);
        }
    }

    /* overlap_box */
    private static void c108(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 108 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapBox(1952.625f, 352.875f, 352.875f, 2421.625f, 209.5f, 1736.125f, 38);
            String r = "1|" + t(v);
            emit(108, label, r);
        }
    }

    /* overlap_box [defaults] */
    private static void c109(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 109 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapBox(1952.625f, 352.875f, 352.875f, 2421.625f, 209.5f, 1736.125f);
            String r = "1|" + t(v);
            emit(109, label, r);
        }
    }

    /* get_param */
    private static void c110(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 110 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.GetParamResult v = s.getParam(85L, "get_param_name");
            String r = v == null ? "1|nil" : "3|" + i((long) v.outKind) + "|" + f((double) v.outNumber) + "|" + i((long) v.outEntity);
            emit(110, label, r);
        }
    }

    /* get_param_text */
    private static void c111(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 111 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.getParamText(86L, "get_param_text_name");
            String r = "1|" + s(v);
            emit(111, label, r);
        }
    }

    /* curve_eval */
    private static void c112(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 112 " + label);
                line("R VMFAIL");
                return;
            }
            Double v = s.curveEval("curve_eval_path", "curve_eval_channel", 2253.625);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(112, label, r);
        }
    }

    /* vcam_activate */
    private static void c113(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.fullHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 113 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.vcamActivate("vcam_activate_name");
            String r = "1|" + i((long) v);
            emit(113, label, r);
        }
    }

    /* get_position [no host] */
    private static void c114(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 114 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getPosition(78L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(114, label, r);
        }
    }

    /* set_position [no host] */
    private static void c115(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 115 " + label);
                line("R VMFAIL");
                return;
            }
            s.setPosition(80L, 106.375f, 1674.125f, 2181.125f);
            String r = "0|";
            emit(115, label, r);
        }
    }

    /* get_rotation [no host] */
    private static void c116(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 116 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getRotation(89L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(116, label, r);
        }
    }

    /* set_rotation [no host] */
    private static void c117(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 117 " + label);
                line("R VMFAIL");
                return;
            }
            s.setRotation(91L, 761.75f, 2123.25f, 578.125f);
            String r = "0|";
            emit(117, label, r);
        }
    }

    /* get_scale [no host] */
    private static void c118(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 118 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getScale(29L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(118, label, r);
        }
    }

    /* get_world_position [no host] */
    private static void c119(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 119 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getWorldPosition(48L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(119, label, r);
        }
    }

    /* set_scale [no host] */
    private static void c120(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 120 " + label);
                line("R VMFAIL");
                return;
            }
            s.setScale(12L, 598.0f, 706.5f, 313.625f);
            String r = "0|";
            emit(120, label, r);
        }
    }

    /* set_parent [no host] */
    private static void c121(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 121 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.setParent(45L, 48L, true);
            String r = "1|" + b(v);
            emit(121, label, r);
        }
    }

    /* get_parent [no host] */
    private static void c122(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 122 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.getParent(6L);
            String r = "1|" + i((long) v);
            emit(122, label, r);
        }
    }

    /* is_key_down [no host] */
    private static void c123(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 123 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isKeyDown(48);
            String r = "1|" + b(v);
            emit(123, label, r);
        }
    }

    /* find_with_tag [no host] */
    private static void c124(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 124 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.findWithTag("find_with_tag_tag");
            String r = "1|" + i((long) v);
            emit(124, label, r);
        }
    }

    /* destroy [no host] */
    private static void c125(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 125 " + label);
                line("R VMFAIL");
                return;
            }
            s.destroy(77L);
            String r = "0|";
            emit(125, label, r);
        }
    }

    /* spawn [no host] */
    private static void c126(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 126 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.spawn("spawn_prefab_path", 1196.375f, 1714.625f, 601.25f);
            String r = "1|" + i((long) v);
            emit(126, label, r);
        }
    }

    /* move_axis [no host] */
    private static void c127(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 127 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.moveAxis();
            String r = "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(127, label, r);
        }
    }

    /* jump_pressed [no host] */
    private static void c128(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 128 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.jumpPressed();
            String r = "1|" + b(v);
            emit(128, label, r);
        }
    }

    /* sprint [no host] */
    private static void c129(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 129 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.sprint();
            String r = "1|" + b(v);
            emit(129, label, r);
        }
    }

    /* attack_pressed [no host] */
    private static void c130(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 130 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.attackPressed();
            String r = "1|" + b(v);
            emit(130, label, r);
        }
    }

    /* set_time_scale [no host] */
    private static void c131(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 131 " + label);
                line("R VMFAIL");
                return;
            }
            s.setTimeScale(254.5f);
            String r = "0|";
            emit(131, label, r);
        }
    }

    /* pause [no host] */
    private static void c132(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 132 " + label);
                line("R VMFAIL");
                return;
            }
            s.pause(true);
            String r = "0|";
            emit(132, label, r);
        }
    }

    /* shake_camera [no host] */
    private static void c133(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 133 " + label);
                line("R VMFAIL");
                return;
            }
            s.shakeCamera(1814.625f);
            String r = "0|";
            emit(133, label, r);
        }
    }

    /* music_set_intensity [no host] */
    private static void c134(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 134 " + label);
                line("R VMFAIL");
                return;
            }
            s.musicSetIntensity(2273.5f);
            String r = "0|";
            emit(134, label, r);
        }
    }

    /* music_get_intensity [no host] */
    private static void c135(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 135 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.musicGetIntensity();
            String r = "1|" + f((double) v);
            emit(135, label, r);
        }
    }

    /* music_request_transition [no host] */
    private static void c136(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 136 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.musicRequestTransition(13);
            String r = "1|" + f((double) v);
            emit(136, label, r);
        }
    }

    /* gas_activate [no host] */
    private static void c137(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 137 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.gasActivate(64L, 81);
            String r = "1|" + b(v);
            emit(137, label, r);
        }
    }

    /* gas_get [no host] */
    private static void c138(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 138 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.gasGet(24L, "gas_get_attr_name");
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(138, label, r);
        }
    }

    /* gas_apply [no host] */
    private static void c139(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 139 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.gasApply(2L, "gas_apply_attr_name", 52, 2178.75f, 833.75f);
            String r = "1|" + b(v);
            emit(139, label, r);
        }
    }

    /* raycast [no host] */
    private static void c140(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 140 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.RaycastResult v = s.raycast(new float[] {2443.375f, 2492.875f, 311.5f}, new float[] {1408.625f, 2363.5f, 295.375f}, 1438.5f);
            String r = v == null ? "1|" + i(0) : "8|" + i((long) v.entity) + "|" + f((double) v.point[0]) + "|" + f((double) v.point[1]) + "|" + f((double) v.point[2]) + "|" + f((double) v.normal[0]) + "|" + f((double) v.normal[1]) + "|" + f((double) v.normal[2]) + "|" + f((double) v.distance);
            emit(140, label, r);
        }
    }

    /* raycast_filtered [no host] */
    private static void c141(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 141 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.RaycastFilteredResult v = s.raycastFiltered(new float[] {2080.5f, 1822.5f, 1165.25f}, new float[] {2419.875f, 1239.875f, 2342.375f}, 245.25f, 71, false);
            String r = v == null ? "1|" + i(0) : "8|" + i((long) v.entity) + "|" + f((double) v.point[0]) + "|" + f((double) v.point[1]) + "|" + f((double) v.point[2]) + "|" + f((double) v.normal[0]) + "|" + f((double) v.normal[1]) + "|" + f((double) v.normal[2]) + "|" + f((double) v.distance);
            emit(141, label, r);
        }
    }

    /* raycast_all [no host] */
    private static void c142(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 142 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.raycastAll(new float[] {724.875f, 90.5f, 1886.25f}, new float[] {2205.0f, 1525.625f, 1440.625f}, 2482.0f, 31, false);
            String r = "1|" + t(v);
            emit(142, label, r);
        }
    }

    /* apply_impulse [no host] */
    private static void c143(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 143 " + label);
                line("R VMFAIL");
                return;
            }
            s.applyImpulse(88L, 583.25f, 1706.75f, 1721.25f);
            String r = "0|";
            emit(143, label, r);
        }
    }

    /* set_velocity [no host] */
    private static void c144(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 144 " + label);
                line("R VMFAIL");
                return;
            }
            s.setVelocity(46L, 2132.75f, 881.375f, 1655.125f);
            String r = "0|";
            emit(144, label, r);
        }
    }

    /* anim_set_float [no host] */
    private static void c145(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 145 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetFloat(79L, "anim_set_float_name", 2114.75f);
            String r = "0|";
            emit(145, label, r);
        }
    }

    /* anim_set_int [no host] */
    private static void c146(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 146 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetInt(49L, "anim_set_int_name", 78);
            String r = "0|";
            emit(146, label, r);
        }
    }

    /* anim_set_bool [no host] */
    private static void c147(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 147 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetBool(47L, "anim_set_bool_name", false);
            String r = "0|";
            emit(147, label, r);
        }
    }

    /* anim_set_trigger [no host] */
    private static void c148(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 148 " + label);
                line("R VMFAIL");
                return;
            }
            s.animSetTrigger(23L, "anim_set_trigger_name");
            String r = "0|";
            emit(148, label, r);
        }
    }

    /* is_action_down [no host] */
    private static void c149(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 149 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isActionDown("is_action_down_name");
            String r = "1|" + b(v);
            emit(149, label, r);
        }
    }

    /* is_action_pressed [no host] */
    private static void c150(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 150 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isActionPressed("is_action_pressed_name");
            String r = "1|" + b(v);
            emit(150, label, r);
        }
    }

    /* get_axis [no host] */
    private static void c151(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 151 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.getAxis("get_axis_name");
            String r = "1|" + f((double) v);
            emit(151, label, r);
        }
    }

    /* get_pointer_delta [no host] */
    private static void c152(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 152 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getPointerDelta();
            String r = "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(152, label, r);
        }
    }

    /* get_pointer_wheel [no host] */
    private static void c153(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 153 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.getPointerWheel();
            String r = "1|" + f((double) v);
            emit(153, label, r);
        }
    }

    /* is_pointer_down [no host] */
    private static void c154(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 154 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isPointerDown(1);
            String r = "1|" + b(v);
            emit(154, label, r);
        }
    }

    /* get_touch_count [no host] */
    private static void c155(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 155 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.getTouchCount();
            String r = "1|" + i((long) v);
            emit(155, label, r);
        }
    }

    /* get_touch [no host] */
    private static void c156(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 156 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.GetTouchResult v = s.getTouch(1);
            String r = v == null ? "1|nil" : "4|" + i((long) v.id) + "|" + f((double) v.x) + "|" + f((double) v.y) + "|" + f((double) v.pressure);
            emit(156, label, r);
        }
    }

    /* tr [no host] */
    private static void c157(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 157 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.tr("tr_key");
            String r = "1|" + s(v);
            emit(157, label, r);
        }
    }

    /* get_locale [no host] */
    private static void c158(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 158 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.getLocale();
            String r = "1|" + s(v);
            emit(158, label, r);
        }
    }

    /* set_locale [no host] */
    private static void c159(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 159 " + label);
                line("R VMFAIL");
                return;
            }
            s.setLocale("set_locale_locale");
            String r = "0|";
            emit(159, label, r);
        }
    }

    /* get_velocity [no host] */
    private static void c160(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 160 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getVelocity(44L);
            String r = v == null ? "1|nil" : "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(160, label, r);
        }
    }

    /* vehicle_set_input [no host] */
    private static void c161(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 161 " + label);
                line("R VMFAIL");
                return;
            }
            s.vehicleSetInput(13L, 2206.0f, 2181.875f, 914.5f);
            String r = "0|";
            emit(161, label, r);
        }
    }

    /* vehicle_get_speed [no host] */
    private static void c162(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 162 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.vehicleGetSpeed(51L);
            String r = "1|" + f((double) v);
            emit(162, label, r);
        }
    }

    /* get_move [no host] */
    private static void c163(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 163 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.getMove();
            String r = "3|" + f((double) v[0]) + "|" + f((double) v[1]) + "|" + f((double) v[2]);
            emit(163, label, r);
        }
    }

    /* ui_get_slider [no host] */
    private static void c164(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 164 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.uiGetSlider(74L);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(164, label, r);
        }
    }

    /* ui_set_slider [no host] */
    private static void c165(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 165 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetSlider(16L, 2225.5f);
            String r = "0|";
            emit(165, label, r);
        }
    }

    /* ui_get_progress [no host] */
    private static void c166(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 166 " + label);
                line("R VMFAIL");
                return;
            }
            Float v = s.uiGetProgress(9L);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(166, label, r);
        }
    }

    /* ui_set_progress [no host] */
    private static void c167(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 167 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetProgress(10L, 198.5f);
            String r = "0|";
            emit(167, label, r);
        }
    }

    /* ui_get_toggle [no host] */
    private static void c168(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 168 " + label);
                line("R VMFAIL");
                return;
            }
            Boolean v = s.uiGetToggle(75L);
            String r = v == null ? "1|nil" : "1|" + b(v.booleanValue());
            emit(168, label, r);
        }
    }

    /* ui_set_toggle [no host] */
    private static void c169(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 169 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetToggle(17L, false);
            String r = "0|";
            emit(169, label, r);
        }
    }

    /* ui_set_text [no host] */
    private static void c170(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 170 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetText(84L, "ui_set_text_txt");
            String r = "0|";
            emit(170, label, r);
        }
    }

    /* send_message [no host] */
    private static void c171(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 171 " + label);
                line("R VMFAIL");
                return;
            }
            s.sendMessage(8L, "send_message_msg", 776.875, "send_message_str_arg");
            String r = "0|";
            emit(171, label, r);
        }
    }

    /* broadcast [no host] */
    private static void c172(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 172 " + label);
                line("R VMFAIL");
                return;
            }
            s.broadcast("broadcast_msg", 1055.625, "broadcast_str_arg");
            String r = "0|";
            emit(172, label, r);
        }
    }

    /* has_component [no host] */
    private static void c173(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 173 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.hasComponent(40L, "has_component_comp_name");
            String r = "1|" + b(v);
            emit(173, label, r);
        }
    }

    /* is_component_enabled [no host] */
    private static void c174(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 174 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isComponentEnabled(41L, "is_component_enabled_comp_name");
            String r = "1|" + b(v);
            emit(174, label, r);
        }
    }

    /* set_component_enabled [no host] */
    private static void c175(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 175 " + label);
                line("R VMFAIL");
                return;
            }
            s.setComponentEnabled(48L, "set_component_enabled_comp_name", true);
            String r = "0|";
            emit(175, label, r);
        }
    }

    /* net_is_server [no host] */
    private static void c176(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 176 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.netIsServer();
            String r = "1|" + b(v);
            emit(176, label, r);
        }
    }

    /* net_is_client [no host] */
    private static void c177(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 177 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.netIsClient();
            String r = "1|" + b(v);
            emit(177, label, r);
        }
    }

    /* net_spawn [no host] */
    private static void c178(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 178 " + label);
                line("R VMFAIL");
                return;
            }
            long v = s.netSpawn("net_spawn_prefab_path", 301.375f, 1501.625f, 1861.125f);
            String r = "1|" + i((long) v);
            emit(178, label, r);
        }
    }

    /* rpc_send [no host] */
    private static void c179(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 179 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.rpcSend(23L, "rpc_send_event", 39, "rpc_send_payload");
            String r = "1|" + b(v);
            emit(179, label, r);
        }
    }

    /* particle_burst [no host] */
    private static void c180(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 180 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleBurst(36L, 53);
            String r = "0|";
            emit(180, label, r);
        }
    }

    /* particle_set_emitting [no host] */
    private static void c181(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 181 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleSetEmitting(41L, true);
            String r = "0|";
            emit(181, label, r);
        }
    }

    /* particle_set_color [no host] */
    private static void c182(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 182 " + label);
                line("R VMFAIL");
                return;
            }
            s.particleSetColor(76L, 1588.75f, 2102.125f, 1560.25f);
            String r = "0|";
            emit(182, label, r);
        }
    }

    /* find_by_name [no host] */
    private static void c183(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 183 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.FindByNameResult v = s.findByName("find_by_name_name");
            String first = v.first == null ? "nil" : i(v.first.longValue());
            String r = "2|" + first + "|" + i(v.count);
            emit(183, label, r);
        }
    }

    /* find_by_prefix [no host] */
    private static void c184(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 184 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.findByPrefix("find_by_prefix_prefix");
            String r = "1|" + t(v);
            emit(184, label, r);
        }
    }

    /* comp_get [no host] */
    private static void c185(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 185 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.compGet(26L, "comp_get_type");
            String r = v == null ? "1|nil" : "1|" + s(v);
            emit(185, label, r);
        }
    }

    /* comp_set [no host] */
    private static void c186(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 186 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.compSet(97L, "comp_set_type", "comp_set_json");
            String r = "1|" + b(v);
            emit(186, label, r);
        }
    }

    /* render_get [no host] */
    private static void c187(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 187 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.renderGet();
            String r = v == null ? "1|nil" : "1|" + s(v);
            emit(187, label, r);
        }
    }

    /* render_set [no host] */
    private static void c188(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 188 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.renderSet("render_set_json");
            String r = "1|" + b(v);
            emit(188, label, r);
        }
    }

    /* audio_set_volume [no host] */
    private static void c189(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 189 " + label);
                line("R VMFAIL");
                return;
            }
            s.audioSetVolume(55L, 966.25f);
            String r = "0|";
            emit(189, label, r);
        }
    }

    /* ui_get_dropdown [no host] */
    private static void c190(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 190 " + label);
                line("R VMFAIL");
                return;
            }
            Integer v = s.uiGetDropdown(37L);
            String r = v == null ? "1|nil" : "1|" + i(v.longValue());
            emit(190, label, r);
        }
    }

    /* ui_set_dropdown [no host] */
    private static void c191(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 191 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetDropdown(39L, 22);
            String r = "0|";
            emit(191, label, r);
        }
    }

    /* ui_get_input_text [no host] */
    private static void c192(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 192 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.uiGetInputText(47L);
            String r = "1|" + s(v);
            emit(192, label, r);
        }
    }

    /* ui_set_input_text [no host] */
    private static void c193(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 193 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetInputText(41L, "ui_set_input_text_text");
            String r = "0|";
            emit(193, label, r);
        }
    }

    /* ui_get_scroll [no host] */
    private static void c194(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 194 " + label);
                line("R VMFAIL");
                return;
            }
            float[] v = s.uiGetScroll(70L);
            String r = v == null ? "1|nil" : "2|" + f((double) v[0]) + "|" + f((double) v[1]);
            emit(194, label, r);
        }
    }

    /* ui_set_scroll [no host] */
    private static void c195(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 195 " + label);
                line("R VMFAIL");
                return;
            }
            s.uiSetScroll(18L, 728.375f, 1564.75f);
            String r = "0|";
            emit(195, label, r);
        }
    }

    /* world_get_hour [no host] */
    private static void c196(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 196 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetHour();
            String r = "1|" + f((double) v);
            emit(196, label, r);
        }
    }

    /* world_set_hour [no host] */
    private static void c197(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 197 " + label);
                line("R VMFAIL");
                return;
            }
            s.worldSetHour(1754.875f);
            String r = "0|";
            emit(197, label, r);
        }
    }

    /* world_is_daytime [no host] */
    private static void c198(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 198 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.worldIsDaytime();
            String r = "1|" + b(v);
            emit(198, label, r);
        }
    }

    /* world_get_weather [no host] */
    private static void c199(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 199 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.worldGetWeather();
            String r = "1|" + i((long) v);
            emit(199, label, r);
        }
    }

    /* world_get_weather_intensity [no host] */
    private static void c200(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 200 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetWeatherIntensity();
            String r = "1|" + f((double) v);
            emit(200, label, r);
        }
    }

    /* world_get_wind_speed [no host] */
    private static void c201(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 201 " + label);
                line("R VMFAIL");
                return;
            }
            float v = s.worldGetWindSpeed();
            String r = "1|" + f((double) v);
            emit(201, label, r);
        }
    }

    /* request_scene [no host] */
    private static void c202(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 202 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.requestScene("request_scene_scene_path");
            String r = "1|" + b(v);
            emit(202, label, r);
        }
    }

    /* is_transitioning [no host] */
    private static void c203(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 203 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.isTransitioning();
            String r = "1|" + b(v);
            emit(203, label, r);
        }
    }

    /* audio_play [no host] */
    private static void c204(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 204 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioPlay(72L);
            String r = "1|" + b(v);
            emit(204, label, r);
        }
    }

    /* audio_stop [no host] */
    private static void c205(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 205 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioStop(79L);
            String r = "1|" + b(v);
            emit(205, label, r);
        }
    }

    /* audio_is_playing [no host] */
    private static void c206(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 206 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.audioIsPlaying(86L);
            String r = "1|" + b(v);
            emit(206, label, r);
        }
    }

    /* save_game [no host] */
    private static void c207(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 207 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.saveGame("save_game_path");
            String r = "1|" + b(v);
            emit(207, label, r);
        }
    }

    /* load_game [no host] */
    private static void c208(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 208 " + label);
                line("R VMFAIL");
                return;
            }
            boolean v = s.loadGame("load_game_path");
            String r = "1|" + b(v);
            emit(208, label, r);
        }
    }

    /* overlap_sphere [no host] */
    private static void c209(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 209 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapSphere(1035.5f, 207.625f, 1063.125f, 2067.5f, 32);
            String r = "1|" + t(v);
            emit(209, label, r);
        }
    }

    /* overlap_box [no host] */
    private static void c210(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 210 " + label);
                line("R VMFAIL");
                return;
            }
            long[] v = s.overlapBox(1952.625f, 352.875f, 352.875f, 2421.625f, 209.5f, 1736.125f, 38);
            String r = "1|" + t(v);
            emit(210, label, r);
        }
    }

    /* get_param [no host] */
    private static void c211(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 211 " + label);
                line("R VMFAIL");
                return;
            }
            JceScript.GetParamResult v = s.getParam(85L, "get_param_name");
            String r = v == null ? "1|nil" : "3|" + i((long) v.outKind) + "|" + f((double) v.outNumber) + "|" + i((long) v.outEntity);
            emit(211, label, r);
        }
    }

    /* get_param_text [no host] */
    private static void c212(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 212 " + label);
                line("R VMFAIL");
                return;
            }
            String v = s.getParamText(86L, "get_param_text_name");
            String r = "1|" + s(v);
            emit(212, label, r);
        }
    }

    /* curve_eval [no host] */
    private static void c213(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 213 " + label);
                line("R VMFAIL");
                return;
            }
            Double v = s.curveEval("curve_eval_path", "curve_eval_channel", 2253.625);
            String r = v == null ? "1|nil" : "1|" + f(v.doubleValue());
            emit(213, label, r);
        }
    }

    /* vcam_activate [no host] */
    private static void c214(String label) {
        JceDiffHost.reset();
        try (JceScript s = JceScript.open(JceDiffHost.partialHostPointer(),
                JceDiffHost.hostSize())) {
            if (s == null) {
                line("CASE 214 " + label);
                line("R VMFAIL");
                return;
            }
            int v = s.vcamActivate("vcam_activate_name");
            String r = "1|" + i((long) v);
            emit(214, label, r);
        }
    }

}
