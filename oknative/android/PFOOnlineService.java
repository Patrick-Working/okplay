package ${YYAndroidPackageName};

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

// Runs while you're in an online lobby or game (started and stopped by the touch overlay), so
// Android keeps the app running when it's in the background: without it, newer Android versions
// freeze or stop a backgrounded app within seconds and the other players lose the connection.
// libPFO.so's background pump keeps the session answered meanwhile; the game itself waits.
public class PFOOnlineService extends Service {
    static final String CHANNEL = "pfo_online";
    static boolean running = false;

    static void start(Context c) {
        if (running) return;
        try {
            Intent i = new Intent(c, PFOOnlineService.class);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
            running = true;
        } catch (Throwable t) {
            Log.i("yoyo", "PFO: couldn't start the online service: " + t);
        }
    }

    static void stop(Context c) {
        if (!running) return;
        running = false;
        try { c.stopService(new Intent(c, PFOOnlineService.class)); } catch (Throwable t) { }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (Build.VERSION.SDK_INT >= 26 && nm != null)
                nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Online play", NotificationManager.IMPORTANCE_LOW));
            Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
            PendingIntent pi = open == null ? null : PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
            b.setContentTitle("UFO 50 - online")
             .setContentText("Keeping your online game connected. Tap to go back.")
             .setSmallIcon(getApplicationInfo().icon)
             .setOngoing(true);
            if (pi != null) b.setContentIntent(pi);
            Notification n = b.build();
            if (Build.VERSION.SDK_INT >= 34) startForeground(50, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(50, n);
        } catch (Throwable t) {
            Log.i("yoyo", "PFO: online service: " + t);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() { running = false; super.onDestroy(); }

    @Override public IBinder onBind(Intent intent) { return null; }
}
