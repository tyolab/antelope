package au.com.tyo.sample;

import android.os.Bundle;
import android.support.v7.app.AppCompatActivity;

import au.com.tyo.antelope.Antelope;

public class MainActivity extends AppCompatActivity {

    static  {
        Antelope.loadNativeLibrary();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
    }
}
