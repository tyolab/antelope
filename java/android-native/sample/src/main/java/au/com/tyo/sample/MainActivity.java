package au.com.tyo.sample;

import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.List;

import au.com.tyo.android.CommonCache;
import au.com.tyo.antelope.Antelope;
import au.com.tyo.app.CommonActivity;
import au.com.tyo.io.FileUtils;


public class MainActivity extends CommonActivity {

    private static final String INDEX_FILE = "index.db";
    private static final String TAG = "MainActivity";

    static  {
        Antelope.loadNativeLibrary();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        final CommonCache commonCache = new CommonCache(this, "data");

        new Thread(new Runnable() {
            @Override
            public void run() {
                if (!commonCache.exists(INDEX_FILE)) {
                    try {
                        FileUtils.copyFile(getResources().openRawResource(R.raw.index), new File(commonCache.getCacheFilePathName(INDEX_FILE)));
                    } catch (IOException e) {
                        Log.e(TAG, "failed to copy moby index to app data directory", e);
                    }
                }

                Antelope antelope = new Antelope("-findex " + commonCache.getCacheFilePathName(INDEX_FILE));
                antelope.start();
                List list = antelope.listTitle("moby");
                Log.i(TAG, "List term (moby) and get hits: " + list.size());
            }
        }).start();
    }
}
