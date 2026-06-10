package org.fieldvox.lab;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import org.json.JSONObject;

public final class RadioBrowserDiscoveryReceiver extends BroadcastReceiver {
    private static final String TAG = "FieldVox";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) {
            return;
        }

        String action = intent.getAction();
        boolean requested = "org.fieldvox.lab.RADIO_BROWSER_DISCOVER".equals(action)
                || intent.getBooleanExtra("fieldvox_radio_browser_discover", false);

        if (!requested) {
            Log.i(TAG, "RADIO_BROWSER_RECEIVER ignored action=" + action);
            return;
        }

        Log.i(TAG, "RADIO_BROWSER_RECEIVER requested");

        final BroadcastReceiver.PendingResult pendingResult = goAsync();
        final Context appContext = context.getApplicationContext();

        new Thread(() -> {
            try {
                JSONObject result = RadioBrowserDiscoveryCache.refresh(appContext, 1000);
                int slotCount = result.optInt("slot_count", 0);
                String server = result.optString("server_selected", "");
                int catalogCount = result.optInt("catalog_count", 0);
                Log.i(TAG, "RADIO_BROWSER_CACHE_OK catalog=" + catalogCount + " slots=" + slotCount + " server=" + server + " trigger=debug_receiver");
            } catch (Throwable t) {
                Log.w(TAG, "RADIO_BROWSER_CACHE_FAILED trigger=debug_receiver", t);
            } finally {
                pendingResult.finish();
            }
        }, "fieldvox-radio-browser-receiver").start();
    }
}
