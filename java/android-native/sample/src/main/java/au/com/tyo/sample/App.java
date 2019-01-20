package au.com.tyo.sample;

import android.content.Context;
import android.content.Intent;

import au.com.tyo.app.CommonApp;
import au.com.tyo.json.form.DataJson;

/**
 * Created by Eric Tang (eric.tang@tyo.com.au) on 20/1/19.
 */
public class App extends CommonApp<UI, Controller, DataJson, DataJson> {

    public App(Context context) {
        super(context);
    }

    @Override
    public void bindDataFromOtherApps(Intent intent) {

    }
}
