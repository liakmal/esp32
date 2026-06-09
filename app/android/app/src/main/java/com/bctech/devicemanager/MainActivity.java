package com.bctech.devicemanager;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        registerPlugin(WifiPlugin.class);
        super.onCreate(savedInstanceState);
    }
}
