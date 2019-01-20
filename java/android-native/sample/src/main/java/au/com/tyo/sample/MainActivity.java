package au.com.tyo.sample;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.List;

import au.com.tyo.android.CommonCache;
import au.com.tyo.antelope.Antelope;
import au.com.tyo.antelope.AntelopeDoc;
import au.com.tyo.io.FileUtils;


public class MainActivity extends Activity {

    private static final String INDEX_FILE = "index.db";
    private static final String TAG = "MainActivity";

    private Antelope antelope = null;

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

                antelope = new Antelope("-findex " + commonCache.getCacheFilePathName(INDEX_FILE));
                antelope.start();
                List<AntelopeDoc> list = antelope.listTerm("moby");
                Log.i(TAG, "List term (moby) and get hits: " + list.size());
                for (AntelopeDoc doc : list)
                    Log.i(TAG, doc.toString());

                try {
                    list = antelope.search("moby");

                    Log.i(TAG, "\nSearching term (moby) and get hits: " + list.size());
                    for (AntelopeDoc doc : list)
                        Log.i(TAG, doc.toString());
                } catch (Exception e) {
                    Log.e(TAG, "failed to search", e);
                }

            }
        }).start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        if (null != antelope)
            antelope.stop();
    }
}
