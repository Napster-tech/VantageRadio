#!/bin/bash

gst-launch-1.0 -v videotestsrc ! videoconvert ! videoscale ! video/x-raw,width=1280,height=720 ! x265enc bitrate=1000 option-string="bframes=1:intra-refresh=0:keyint=60:no-open-gop=1:repeat-headers=1" ! udpsink host=127.0.0.1 port=10000