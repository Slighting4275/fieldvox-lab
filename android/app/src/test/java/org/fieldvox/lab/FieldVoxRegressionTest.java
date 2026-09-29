package org.fieldvox.lab;

import org.junit.Assert;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public class FieldVoxRegressionTest {

    @Rule
    public TemporaryFolder tempFolder = new TemporaryFolder();

    @Test
    public void testSessionStemAcceptsNativeAndUnknownStems() {
        // Native C session stems have format session-<timestamp>-<seq>
        Assert.assertTrue("Native session stem should be valid",
                MainActivity.isSafeSessionStem("session-1790453871-1"));

        // Fallback stem when no sessions exist yet
        Assert.assertTrue("session-unknown should be valid",
                MainActivity.isSafeSessionStem("session-unknown"));

        // Live monitor session stems
        Assert.assertTrue("Live session stem should be valid",
                MainActivity.isSafeSessionStem("session-1743123456-live"));
        Assert.assertTrue("Live session stem with sequence should be valid",
                MainActivity.isSafeSessionStem("session-1743123456-1-live"));

        // Invalid stems
        Assert.assertFalse("Traversal stems must be rejected",
                MainActivity.isSafeSessionStem("../session-attack"));
        Assert.assertFalse("Non-session prefix must be rejected",
                MainActivity.isSafeSessionStem("not-a-session-123"));
        Assert.assertFalse("Null stem must be rejected",
                MainActivity.isSafeSessionStem(null));
        Assert.assertFalse("Empty stem must be rejected",
                MainActivity.isSafeSessionStem(""));
    }

    @Test
    public void testCountTriggerEventsNativeAndLive() throws Exception {
        File jsonlFile = tempFolder.newFile("test_triggers.jsonl");

        // Native C writes {"type":"trigger",...}
        // Live monitor writes {"event":"trigger",...}
        // Other events should not be counted
        String content = "{\"type\":\"trigger\",\"trigger\":1,\"t_ns\":1000,\"mag\":1.0,\"delta\":0.5,\"freq_hz\":440.0,\"threshold\":0.4,\"cooldown_ms\":450}\n"
                + "{\"type\":\"frame\",\"seq\":2}\n"
                + "{\"event\":\"trigger\",\"mode\":\"realtime_evp_monitor\",\"source\":\"microphone\",\"t_ms\":2000}\n"
                + "{\"type\":\"trigger\",\"trigger\":2,\"t_ns\":3000,\"mag\":1.2,\"delta\":0.6,\"freq_hz\":880.0,\"threshold\":0.4,\"cooldown_ms\":450}\n";

        try (FileOutputStream out = new FileOutputStream(jsonlFile)) {
            out.write(content.getBytes(StandardCharsets.UTF_8));
        }

        int count = MainActivity.countTriggerEvents(jsonlFile);
        Assert.assertEquals("Should count all 3 triggers (2 native type:trigger + 1 live event:trigger)", 3, count);
    }

    @Test
    public void testRadioBrowserCatalogThreshold() {
        // Full target: requires at least 500 vetted stations
        Assert.assertEquals(500, RadioBrowserDiscoveryCache.minimumAcceptableCatalogCount(1000));

        // Smaller requested slot targets: threshold scales proportionally instead of hardcoding 500
        Assert.assertEquals(50, RadioBrowserDiscoveryCache.minimumAcceptableCatalogCount(100));
        Assert.assertEquals(32, RadioBrowserDiscoveryCache.minimumAcceptableCatalogCount(64));
    }
}
