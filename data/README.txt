Drop .mp3 files in this folder, then:

    ~/firmware-lab/.venv313/bin/pio run -e newsheen-speaker -t uploadfs

They land on the puck's LittleFS partition and show up in the demo rotation and
on the web UI. You can also skip this entirely and upload from a phone at
http://192.168.4.1/ once the puck's Wi-Fi AP is up.

Decoding budget: 44.1 kHz or lower, mono or stereo, CBR up to about 192 kbps.
The MP3 partition is ~12 MB, so roughly 8 minutes at 192 kbps.
