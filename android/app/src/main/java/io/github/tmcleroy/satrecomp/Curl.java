// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
package io.github.tmcleroy.satrecomp;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

// What recomp-common's launcher asks the system's curl for elsewhere (the online matches, invitations: online.cpp),
// made here with Android's own HTTP (recomp-common/launcher/src/run_program.cpp: the activities' curl()). Of curl's
// arguments, those the launcher gives: -sS, --max-time SECONDS, -X METHOD, -H "NAME: VALUE", --data-binary BODY,
// -w "...%{http_code}" and the URL; and the updater's (update.cpp): -f (an HTTP error fails), -o FILE (the answer
// written to that file, as it comes: "" is written), --connect-timeout SECONDS, and -A, --speed-limit, --speed-time
// and --retry, whose values are passed over. -L needs nothing: redirects are followed.
final class Curl {
    private Curl() {}

    // What curl would write, or null where it would fail (no connection, no answer in time)
    static String run(String[] arguments) {
        String url = null, method = null, body = null, written = null, output = null;
        int seconds = 30;
        boolean fail = false;
        java.util.List<String[]> headers = new java.util.ArrayList<>();
        for (int i = 0; i < arguments.length; i++) {
            String a = arguments[i];
            boolean more = i + 1 < arguments.length;
            if (a.equals("--max-time") && more) {
                seconds = (int) Math.ceil(Double.parseDouble(arguments[++i]));
            } else if (a.equals("-X") && more) {
                method = arguments[++i];
            } else if (a.equals("-H") && more) {
                String header = arguments[++i];
                int colon = header.indexOf(':');
                if (colon > 0) {
                    headers.add(new String[] {header.substring(0, colon).trim(), header.substring(colon + 1).trim()});
                }
            } else if (a.equals("--data-binary") && more) {
                body = arguments[++i];
            } else if (a.equals("-w") && more) {
                written = arguments[++i];
            } else if (a.equals("-o") && more) {
                output = arguments[++i];
            } else if (a.equals("--connect-timeout") && more) {
                seconds = Math.max(seconds, (int) Math.ceil(Double.parseDouble(arguments[++i])));
            } else if ((a.equals("-A") || a.equals("--speed-limit") || a.equals("--speed-time") || a.equals("--retry")) && more) {
                i++;
            } else if (a.startsWith("-") && !a.startsWith("--") && a.indexOf('f') > 0) {
                fail = true;  // (-f, or flags together: -fsSL)
            } else if (!a.startsWith("-")) {
                url = a;
            }
        }
        if (url == null) {
            return null;
        }
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(seconds * 1000);
            connection.setReadTimeout(seconds * 1000);
            connection.setRequestMethod(method != null ? method : body != null ? "POST" : "GET");
            for (String[] header : headers) {
                connection.setRequestProperty(header[0], header[1]);
            }
            if (body != null) {
                connection.setDoOutput(true);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(body.getBytes(StandardCharsets.UTF_8));
                }
            }
            int status = connection.getResponseCode();
            if (fail && status >= 400) {
                return null;
            }
            if (output != null) {
                File file = new File(output);
                try (InputStream stream = connection.getInputStream(); OutputStream out = new FileOutputStream(file)) {
                    byte[] buffer = new byte[1 << 16];
                    for (int got; (got = stream.read(buffer)) > 0;) {
                        out.write(buffer, 0, got);
                    }
                } catch (Exception e) {
                    file.delete();
                    throw e;
                }
                return "";
            }
            // (curl writes the answer whatever its status, as these requests ask it to: no -f)
            InputStream in = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            ByteArrayOutputStream answer = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream stream = in) {
                    byte[] buffer = new byte[8192];
                    for (int got; (got = stream.read(buffer)) > 0;) {
                        answer.write(buffer, 0, got);
                    }
                }
            }
            String text = answer.toString("UTF-8");
            if (written != null) {
                text += written.replace("%{http_code}", String.valueOf(status)).replace("\\n", "\n");
            }
            return text;
        } catch (Exception e) {
            Log.w("satrecomp", "curl " + url + ": " + e);
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }
}
