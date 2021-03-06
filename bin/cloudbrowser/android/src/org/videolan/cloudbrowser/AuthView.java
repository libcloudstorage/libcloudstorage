package org.videolan.cloudbrowser;

import android.app.Activity;
import android.os.Bundle;
import android.net.Uri;
import android.content.Intent;

public class AuthView extends Activity {
    private boolean m_intent_run = false;
    public static AuthView m_current;

    @Override
    protected void onResume() {
        super.onResume();

        Bundle intent_extras = getIntent().getExtras();
        CharSequence sequence = intent_extras == null ?
            null : intent_extras.getCharSequence("address");
        String url = sequence == null ? null : sequence.toString();

        m_current = this;

        if (!m_intent_run) {
            m_intent_run = true;
            if (url != null) {
                Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
                Bundle extras = new Bundle();
                extras.putBinder("android.support.customtabs.extra.SESSION", null);
                intent.putExtras(extras);
                setTitle("Cloud Browser Consent Screen");
                startActivity(intent);
            }
        } else {
            finish();
        }
    }
}
