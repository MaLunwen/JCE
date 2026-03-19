package com.jce;

import org.libsdl.app.SDLActivity;

public class JCEActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "JCE" };
    }
}
