// The HTTP half of oknative on Android: one form POST to the okplay room server.
// Native code calls okpPost from its own worker thread (never the UI or GL thread) through JNI and
// gets back "<http status>\n<body>", or "0\n<error>" when it couldn't connect.
// In UFO 50 Android this lives in PFO_Android.java; rename the class/package to suit your app and
// point the native g_HttpPost at it (see ../README.md, "Wiring it into a game").
package net.oldkid.oknative;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

public final class OkplayHttp {
    private OkplayHttp() { }

    public static String okpPost(String url, String body) {
        // A kept-alive connection the server already closed fails at once ("unexpected end of
        // stream") and Android doesn't retry a POST by itself, so try again on a fresh one.
        // (Up to 3 tries: the pool can hold more than one dead connection.)
        String r = okpPostOnce(url, body, false);
        for (int i = 0; i < 2 && (r.startsWith("0\njava.io.IOException") || r.startsWith("0\njava.io.EOFException")); i++)
            r = okpPostOnce(url, body, true);
        return r;
    }

    static String okpPostOnce(String url, String body, boolean fresh) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(6000);
            c.setReadTimeout(8000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
            c.setRequestProperty("User-Agent", "oknative-Android");
            if (fresh) c.setRequestProperty("Connection", "close");
            byte[] data = body.getBytes("UTF-8");
            c.setFixedLengthStreamingMode(data.length);
            OutputStream o = c.getOutputStream();
            o.write(data);
            o.close();
            int code = c.getResponseCode();
            InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream();
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            if (in != null) {
                byte[] tmp = new byte[8192];
                int n;
                while ((n = in.read(tmp)) > 0 && buf.size() < (1 << 20)) buf.write(tmp, 0, n);
                in.close();
            }
            return code + "\n" + buf.toString("UTF-8");
        } catch (Throwable t) {
            return "0\n" + t;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
